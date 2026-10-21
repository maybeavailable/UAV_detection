#include"webrtc/congestion_control/gcc_transport_controller.h"
#include"webrtc/congestion_control/pacer.h"
#include"log/log.h"
#include<algorithm>
#include<cmath>
#include<deque>
#include<filesystem>
#include<fstream>
#include<iomanip>
#include<iostream>
#include<map>
#include<queue>
#include<random>
#include<stdexcept>
#include<tuple>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = TransportTime;
const Time epoch{1000s};
constexpr std::size_t tagBytes = 16, maxWireBytes = 1200;
struct Phase { double start; std::uint64_t capacity; };
struct Scenario
{
    std::string name;
    std::vector<Phase> phases;
    double seconds{60};
    std::chrono::milliseconds propagation{25}, jitter{0}; // each direction; forward jitter may reorder packets
    double randomLoss{0}, feedbackLoss{0};
    std::uint64_t applicationLimit{0}, maximumBitrate{5000000};
    bool alrProbing{false};
};
struct Sample
{
    double seconds;
    std::uint64_t capacity, target, effective, pacing, received;
    double queueMs, loss;
    std::uint64_t probePackets;
};
struct Datagram
{
    RtcpBytes bytes;
    std::size_t wireSize;
    bool control;
    std::uint64_t remaining; // bit * microsecond; capacity * elapsed_us supplies service
};
struct Arrival
{
    Time at; std::uint64_t id; RtcpBytes bytes; std::size_t wireSize;
    bool atSender, control;
    bool operator<(const Arrival& other) const { return std::tie(at, id) > std::tie(other.at, other.id); }
};
RtcpSession::Config rtcpConfig(std::uint32_t ssrc)
{
    RtcpSession::Config c; c.senderSsrc = ssrc; c.cname = "gcc-simulation";
    c.parameters.reducedSize = true;
    RtcpMediaParameters m; m.payloadType = 96; m.clockRate = 90000; m.transportCc = true;
    m.sendTwccExtension = m.receiveTwccExtension = 5; c.parameters.media = {m};
    return c;
}
GccTransportConfig gccConfig(const Scenario& s)
{
    GccTransportConfig c; c.max_bitrate = s.maximumBitrate;
    c.streams.requests_alr_probing = s.alrProbing; return c;
}
// A deterministic virtual network, not a model of the estimator. All estimation,
// probe scheduling, pacing, RTP/TWCC encoding, history matching and RR handling
// use production classes. Capacity is visible only to the FIFO link and metrics.
class Simulation
{
public:
    explicit Simulation(Scenario scenario) : scenario_(std::move(scenario)), gcc_(gccConfig(scenario_)), pacer_(Pacer::Config{}, epoch)
    {
        RtcpSession::Callbacks callbacks;
        callbacks.onSentPacket = [&](auto p) { apply(gcc_.OnSentPacket(std::move(p), now_)); };
        callbacks.onTransportFeedback = [&](auto f) { apply(gcc_.OnFeedback(std::move(f), now_)); };
        callbacks.onReport = [&](auto r) { apply(gcc_.OnReport(r, now_)); };
        sender_ = std::make_unique<RtcpSession>(rtcpConfig(10), callbacks);
        receiver_ = std::make_unique<RtcpSession>(rtcpConfig(20));
        apply(gcc_.Start(0, true, now_));
    }
    std::vector<Sample> run()
    {
        const auto steps = static_cast<unsigned>(scenario_.seconds * 1000);
        for (unsigned ms = 0; ms <= steps; ++ms)
        {
            now_ = epoch + std::chrono::milliseconds(ms);
            capacity_ = scenario_.phases.front().capacity;
            for (const auto& p : scenario_.phases) if (ms >= p.start * 1000) capacity_ = p.capacity;
            if (ms) serviceLink();
            deliver();
            if (ms % 10 == 0)
            {
                sender_->poll(now_, ntp(now_)); receiver_->poll(now_, ntp(now_));
                for (auto& p : sender_->takeOutput()) link(std::move(p.bytes), true);
                for (auto& p : receiver_->takeOutput())
                    if (!randomDrop(scenario_.feedbackLoss)) schedule(std::move(p.bytes), 0, true, true, now_ + scenario_.propagation);
            }
            if (ms % 25 == 0) apply(gcc_.Process(now_, pacer_.snapshot(now_).queuedBytes, sender_->outstandingData()));
            if (now_ >= nextFrame_) { generateFrame(); nextFrame_ += 33333us; }
            pump();
            if (ms % 100 == 0) sample(ms / 1000.0);
        }
        CHECK(gcc_.snapshot().sent_packets > 100 && gcc_.snapshot().feedback_packets > 100);
        CHECK(probePackets_ > 0 && sender_->snapshot().malformedPackets == 0);
        return samples_;
    }
    void save(const std::filesystem::path& directory) const
    {
        std::filesystem::create_directories(directory);
        std::ofstream f(directory / (scenario_.name + ".csv")); CHECK(f);
        f << "time_s,capacity_bps,target_bps,effective_bps,pacing_bps,received_bps,queue_ms,loss_ratio,probe_packets\n";
        f << std::fixed << std::setprecision(6);
        for (const auto& s : samples_) f << s.seconds << ',' << s.capacity << ',' << s.target << ',' << s.effective << ','
            << s.pacing << ',' << s.received << ',' << s.queueMs << ',' << s.loss << ',' << s.probePackets << '\n';
    }
    double dropFraction() const { return transmitted_ ? double(dropped_) / transmitted_ : 0; }
private:
    static std::uint64_t ntp(Time t)
    {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
        return (std::uint64_t(us / 1000000) << 32) + std::uint64_t(us % 1000000) * 0x100000000ULL / 1000000;
    }
    bool randomDrop(double probability) { return double(random_()) / (double(std::mt19937::max()) + 1.0) < probability; }
    void apply(NetworkControlUpdate update)
    {
        if (update.target_rate) target_ = *update.target_rate;
        if (update.pacer_config)
        {
            const auto rate = update.pacer_config->data_rate();
            const auto padding = update.pacer_config->pad_rate(); const auto old = pacer_.snapshot(now_);
            if (rate != old.rateBps || padding != old.paddingRateBps) CHECK(pacer_.setRates(rate, padding, now_));
        }
        if (update.congestion_window) window_ = update.congestion_window;
    }
    std::uint64_t effectiveRate() const
    { return static_cast<std::uint64_t>(target_.target_rate * (1.0 - target_.cwnd_reduce_ratio)); }
    RtcpBytes rtp(std::size_t wireSize, bool padding)
    {
        RtcpBytes bytes(padding ? 20 + std::clamp<std::size_t>(wireSize > 36 ? wireSize - 36 : 1, 1, 255) : wireSize - tagBytes - 8, 0);
        bytes[0] = padding ? 0xa0 : 0x80; bytes[1] = 96;
        auto& seq = padding ? paddingSequence_ : mediaSequence_;
        bytes[2] = static_cast<std::uint8_t>(seq >> 8); bytes[3] = static_cast<std::uint8_t>(seq++);
        const auto stamp = std::uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now_ - epoch).count() * 9 / 100);
        for (unsigned i = 0; i < 4; ++i) bytes[4 + i] = static_cast<std::uint8_t>(stamp >> (24 - i * 8));
        bytes[11] = padding ? 43 : 42;
        if (padding)
        {
            // Leave 20 bytes for the RTP header with TWCC, then append padding.
            bytes.resize(bytes.size() - 8); bytes.back() = static_cast<std::uint8_t>(bytes.size() - 12);
        }
        CHECK(sender_->reserveRtpExtensions(bytes));
        return bytes;
    }
    void generateFrame()
    {
        const auto rate = scenario_.applicationLimit ? std::min(effectiveRate(), scenario_.applicationLimit) : effectiveRate();
        frameCredit_ += static_cast<long double>(rate) / (8 * 30);
        while (frameCredit_ >= 60)
        {
            const auto bytes = std::min(maxWireBytes, static_cast<std::size_t>(frameCredit_)); frameCredit_ -= bytes;
            auto packet = rtp(bytes, false); const auto id = ++packetId_;
            const auto accepted = pacer_.enqueue({id, 0, 42, Pacer::Priority::Video, packet.size() + tagBytes}, now_);
            if (accepted == Pacer::Admission::Accepted)
            { gcc_.OnPacketQueued(packet.size() + tagBytes); pending_.emplace(id, std::move(packet)); }
        }
    }
    void send(RtcpBytes packet, const PacedPacketInfo& pacing = {})
    {
        CHECK(sender_->prepareRtp(packet)); const auto size = packet.size() + tagBytes;
        const auto id = sender_->sendingRtp(packet, size, now_, now_, 0, pacing); CHECK(id);
        sender_->sentRtp(packet, {}, now_, false, id);
        if (pacing.probe_cluster_id >= 0) ++probePackets_;
        gcc_.prober().SetAllowProbeWithoutMediaPacket(true);
        link(std::move(packet), false);
    }
    void pump()
    {
        pacer_.setCongested(window_ && sender_->outstandingData() >= *window_);
        for (const auto& p : pacer_.expire(now_)) pending_.erase(p.id);
        const auto cluster = gcc_.prober().CurrentCluster(now_); const auto due = gcc_.prober().NextProbeTime(now_);
        if (cluster && due && *due <= now_ && !pacer_.snapshot(now_).congested)
        {
            const auto needed = std::max<std::size_t>(1, gcc_.prober().RecommendedMinProbeSize()); std::size_t bytes = 0;
            for (unsigned i = 0; i < 64 && bytes < needed && bytes < 65536; ++i)
            {
                if (const auto p = pacer_.pop(now_, true))
                { auto packet = std::move(pending_.at(p->id)); pending_.erase(p->id); bytes += packet.size() + tagBytes; send(std::move(packet), *cluster); }
                else
                { auto packet = rtp(needed - bytes, true); const auto size = packet.size() + tagBytes; bytes += size; send(std::move(packet), *cluster); pacer_.accountProbeBytes(size); }
            }
            if (bytes) gcc_.prober().ProbeSent(now_, bytes);
        }
        if (!gcc_.prober().is_probing()) for (unsigned i = 0; i < 64; ++i)
        {
            const auto p = pacer_.pop(now_); if (!p) break;
            auto packet = std::move(pending_.at(p->id)); pending_.erase(p->id); send(std::move(packet));
        }
    }
    void link(RtcpBytes bytes, bool control)
    {
        const auto size = bytes.size() + (control ? 20 : tagBytes); ++transmitted_;
        // Fixed 64 KiB tail-drop buffer; no unlimited hidden queue.
        if (queuedBytes_ + size > 65536) { ++dropped_; return; }
        queuedBytes_ += size; fifo_.push_back({std::move(bytes), size, control, size * 8ULL * 1000000});
    }
    void serviceLink()
    {
        std::uint64_t remaining = capacity_ * 1000, consumed = 0;
        while (!fifo_.empty() && remaining)
        {
            auto& front = fifo_.front(); const auto service = std::min(remaining, front.remaining);
            remaining -= service; consumed += service; front.remaining -= service;
            if (!front.remaining)
            {
                const auto departure = now_ - 1ms + std::chrono::microseconds((consumed + capacity_ - 1) / capacity_);
                queuedBytes_ -= front.wireSize;
                if (randomDrop(scenario_.randomLoss)) ++dropped_;
                else
                {
                    const auto jitter = scenario_.jitter.count() ? std::chrono::microseconds(int(random_() % (scenario_.jitter.count() * 2000 + 1)) - scenario_.jitter.count() * 1000) : 0us;
                    schedule(std::move(front.bytes), front.wireSize, false, front.control, departure + scenario_.propagation + jitter);
                }
                fifo_.pop_front();
            }
        }
    }
    void schedule(RtcpBytes bytes, std::size_t size, bool sender, bool control, Time at)
    { arrivals_.push({at, ++arrivalId_, std::move(bytes), size, sender, control}); }
    void deliver()
    {
        while (!arrivals_.empty() && arrivals_.top().at <= now_)
        {
            auto p = arrivals_.top(); arrivals_.pop();
            if (p.control)
            { CHECK((p.atSender ? sender_ : receiver_)->receivedRtcp(p.bytes, p.atSender ? now_ : p.at)); }
            else
            { receiver_->receivedRtp(p.bytes, p.at); delivered_.emplace_back(p.at, p.wireSize); }
        }
    }
    void sample(double seconds)
    {
        while (!delivered_.empty() && delivered_.front().first <= now_ - 1s) delivered_.pop_front();
        std::uint64_t received = 0; for (const auto& p : delivered_) received += p.second * 8;
        samples_.push_back({seconds, capacity_, target_.target_rate, effectiveRate(), pacer_.snapshot(now_).rateBps, received,
            double(queuedBytes_) * 8000 / capacity_, target_.network_estimate.loss_rate_ratio, probePackets_});
    }
    Scenario scenario_; GccTransportController gcc_; Pacer pacer_;
    std::unique_ptr<RtcpSession> sender_, receiver_;
    Time now_{epoch}, nextFrame_{epoch};
    TargetTransferRate target_; std::optional<std::size_t> window_;
    std::uint64_t capacity_{0}, packetId_{0}, arrivalId_{0}, probePackets_{0}, transmitted_{0}, dropped_{0};
    std::uint16_t mediaSequence_{0}, paddingSequence_{0};
    long double frameCredit_{0}; std::size_t queuedBytes_{0};
    std::map<std::uint64_t, RtcpBytes> pending_;
    std::deque<Datagram> fifo_;
    std::priority_queue<Arrival> arrivals_;
    std::deque<std::pair<Time, std::size_t>> delivered_;
    std::mt19937 random_{0x5eed}; std::vector<Sample> samples_;
};
double average(const std::vector<Sample>& samples, double begin, double end, bool received = false)
{
    double total = 0; unsigned count = 0;
    for (const auto& s : samples) if (s.seconds >= begin && s.seconds < end) { total += received ? s.received : s.target; ++count; }
    CHECK(count); return total / count;
}
// First start of a continuous three-second interval inside the acceptance band.
// It is a response-time metric, not a claim that AIMD stops oscillating afterwards.
std::optional<double> settling(const std::vector<Sample>& samples, double begin, double end, std::uint64_t capacity)
{
    for (std::size_t i = 0; i + 30 < samples.size(); ++i)
    {
        if (samples[i].seconds < begin || samples[i + 30].seconds >= end) continue;
        bool inside = true;
        for (std::size_t j = i; j <= i + 30; ++j)
            inside = inside && samples[j].target >= capacity * 0.8 && samples[j].target <= capacity * 1.15;
        if (inside) return samples[i].seconds - begin;
    }
    return {};
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        LogManager::getInstance().setLevel(LogLevel::ERROR);
        std::vector<Scenario> scenarios;
        scenarios.push_back({"fixed_500k", {{0, 500000}}});
        scenarios.push_back({"fixed_1m", {{0, 1000000}}});
        scenarios.push_back({"fixed_3m", {{0, 3000000}}});
        Scenario step{"capacity_step", {{0, 1000000}, {30, 300000}, {60, 1500000}}}; step.seconds = 110; scenarios.push_back(step);
        Scenario loss{"random_loss_5pct", {{0, 1000000}}}; loss.randomLoss = 0.05; loss.jitter = 2ms; scenarios.push_back(loss);
        Scenario rtt{"rtt_200ms", {{0, 1000000}}}; rtt.propagation = 100ms; scenarios.push_back(rtt);
        Scenario feedback{"feedback_loss_10pct", {{0, 1000000}}}; feedback.feedbackLoss = 0.1; scenarios.push_back(feedback);
        Scenario alr{"application_limited", {{0, 2000000}}}; alr.applicationLimit = 100000; alr.alrProbing = true; scenarios.push_back(alr);
        std::cout << "scenario,phase_start_s,capacity_bps,mean_target_last_10s,mean_received_last_10s,target_capacity_ratio,settling_s,drop_fraction\n";
        for (const auto& scenario : scenarios)
        {
            Simulation simulation(scenario); const auto samples = simulation.run();
            if (argc > 1) simulation.save(argv[1]);
            for (const auto& s : samples) CHECK(s.target >= 30000 && s.target <= scenario.maximumBitrate);
            for (std::size_t i = 0; i < scenario.phases.size(); ++i)
            {
                const auto phase = scenario.phases[i];
                const auto end = i + 1 < scenario.phases.size() ? scenario.phases[i + 1].start : scenario.seconds;
                const auto target = average(samples, end - 10, end);
                const auto received = average(samples, end - 10, end, true);
                const auto response = settling(samples, phase.start, end, phase.capacity);
                std::cout << scenario.name << ',' << phase.start << ',' << phase.capacity << ',' << std::llround(target) << ',' << std::llround(received)
                    << ',' << std::fixed << std::setprecision(4) << target / phase.capacity << ',' << response.value_or(-1) << ',' << simulation.dropFraction() << '\n';
                // Steady-window tolerances test useful capacity estimation rather than exact trajectories.
                CHECK(target >= phase.capacity * (scenario.randomLoss ? 0.6 : 0.8));
                CHECK(target <= phase.capacity * 1.15);
                if (!scenario.randomLoss) CHECK(response && *response < (i == 2 ? 25 : 10));
                if (scenario.applicationLimit) CHECK(received >= scenario.applicationLimit * 0.8 && received < scenario.applicationLimit * 1.5);
                else CHECK(received >= phase.capacity * (scenario.randomLoss ? 0.55 : 0.75) && received <= phase.capacity * 1.15);
                if (scenario.phases.size() > 1 && i == 1) CHECK(response && *response < 5);
            }
        }
        std::cout << "PASS: eight closed-loop capacity/delay/loss/ALR scenarios (530 seconds of virtual network time)\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
