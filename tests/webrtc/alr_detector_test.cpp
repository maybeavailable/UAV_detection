#include"webrtc/congestion_control/alr_detector.h"
#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"

#include<cstdint>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = AlrDetector::Time;
AlrDetector::Config baseConfig() { return {0.65, 0.80, 0.50}; }
struct Fixture
{
    Time now{1s}; int clock_reads{0};
    AlrDetector detector;
    explicit Fixture(AlrDetector::Config config = {})
        : detector(config, [this] { ++clock_reads; return now; }) { detector.SetEstimatedBitrate(300000); }
    void traffic(std::chrono::milliseconds duration, unsigned percent)
    {
        for (auto t = 0ms; t < duration; t += 10ms)
        {
            now += 10ms;
            detector.OnBytesSent(300000ULL * percent / 80000, now);
        }
    }
    std::optional<Time> start() const { return detector.GetApplicationLimitedRegionStartTime(); }
};
void upstreamScenarios()
{
    for (const auto& config : {AlrDetector::Config{}, baseConfig()})
    {
        Fixture f(config); CHECK(!f.start());
        f.traffic(1s, 90); CHECK(!f.start());
        f.traffic(1500ms, 20); CHECK(f.start());
        f.traffic(4s, 100); CHECK(!f.start());

        Fixture spike(config); spike.traffic(1s, 20); CHECK(spike.start());
        const auto started = spike.start();
        spike.traffic(100ms, 150); CHECK(spike.start() == started);
        spike.traffic(3s, 100); CHECK(!spike.start());

        Fixture estimate(config); estimate.traffic(1s, 20); CHECK(estimate.start());
        const auto before = estimate.start(); estimate.detector.SetEstimatedBitrate(60000);
        CHECK(estimate.start() == before); // bitrate changes cannot immediately exit ALR
        estimate.traffic(1s, 50); CHECK(!estimate.start());
    }
    Fixture custom({0.90, 0.0, -0.10});
    custom.traffic(1s, 100); CHECK(!custom.start());
    custom.traffic(2100ms, 85); CHECK(custom.start());
}
void thresholdsAndClock()
{
    Fixture f({1.0, 0.8, 0.5}); f.detector.SetEstimatedBitrate(8000);
    f.now = Time(123s + 987654us + 999ns);
    f.detector.OnBytesSent(std::numeric_limits<std::size_t>::max(), Time(0ms));
    CHECK(!f.start() && f.clock_reads == 0); // first bytes are intentionally ignored
    f.detector.OnBytesSent(0, Time(400ms)); CHECK(!f.start()); // exactly 0.8: strict greater-than
    f.detector.OnBytesSent(0, Time(401ms));
    CHECK(f.start() == Time(123s + 987654us) && f.clock_reads == 1);
    const auto entered = f.start();
    f.detector.OnBytesSent(0, Time(401ms)); CHECK(f.start() == entered && f.clock_reads == 1);
    f.detector.OnBytesSent(151, Time(401ms)); CHECK(f.start() == entered); // exactly 0.5: no exit
    f.detector.OnBytesSent(1, Time(401ms)); CHECK(!f.start() && f.clock_reads == 1);
    f.now += 1s;
    f.detector.OnBytesSent(0, Time(553ms)); CHECK(f.start() && f.start() != entered && f.clock_reads == 2);

    Fixture order({1.0, 0.8, 0.5}); order.detector.SetEstimatedBitrate(8000);
    order.detector.OnBytesSent(0, Time(0ms));
    order.detector.OnBytesSent(1000000, Time(1s)); // cap debt before earning the elapsed-time budget
    CHECK(order.start());
    order.detector.OnBytesSent(std::numeric_limits<std::size_t>::max(), Time(1s)); CHECK(!order.start());

    AlrDetector real({1.0, 0.8, 0.5}); real.SetEstimatedBitrate(8000);
    const auto before = std::chrono::steady_clock::now();
    real.OnBytesSent(0, Time(0ms)); real.OnBytesSent(0, Time(1s));
    const auto after = std::chrono::steady_clock::now();
    CHECK(real.GetApplicationLimitedRegionStartTime());
    const auto earliest = Time(std::chrono::duration_cast<std::chrono::microseconds>(before.time_since_epoch()));
    CHECK(*real.GetApplicationLimitedRegionStartTime() >= earliest && *real.GetApplicationLimitedRegionStartTime() <= after);
}
void roundingAndLowRates()
{
    Fixture duration({1.0, 0.0, -0.1}); duration.detector.SetEstimatedBitrate(8000);
    duration.detector.OnBytesSent(0, Time(499us)); duration.detector.OnBytesSent(0, Time(500us));
    CHECK(!duration.start()); // a 1 us duration must not be rounded from absolute millisecond timestamps
    duration.detector.OnBytesSent(0, Time(1000us)); CHECK(duration.start()); // 500 us rounds to 1 ms

    Fixture down(baseConfig()); down.detector.SetEstimatedBitrate(2306); // 1498.9 bps -> 1499 -> 1 kbps
    down.detector.OnBytesSent(0, Time(0ms)); down.detector.OnBytesSent(0, Time(400ms));
    CHECK(down.start()); // 50 bytes / 62-byte window > 0.8
    Fixture up(baseConfig()); up.detector.SetEstimatedBitrate(2307); // 1499.55 bps -> 1500 -> 2 kbps
    up.detector.OnBytesSent(0, Time(0ms)); up.detector.OnBytesSent(0, Time(400ms));
    CHECK(!up.start()); // 100 bytes / 125-byte window == 0.8
    up.detector.OnBytesSent(0, Time(404ms)); CHECK(up.start());

    AlrDetector unconfigured; unconfigured.OnBytesSent(0, Time(0ms)); unconfigured.OnBytesSent(0, Time(1s));
    CHECK(!unconfigured.GetApplicationLimitedRegionStartTime());
    Fixture tiny; tiny.detector.SetEstimatedBitrate(1);
    tiny.detector.OnBytesSent(0, Time(0ms)); tiny.detector.OnBytesSent(0, Time(1s)); CHECK(!tiny.start());
    Fixture active; active.traffic(1s, 20); CHECK(active.start());
    const auto old = active.start(); active.detector.SetEstimatedBitrate(1); active.traffic(1s, 100);
    CHECK(active.start() == old); // ratio 0 cannot cross the default negative stop threshold
}
void validation()
{
    for (int kind = 0; kind < 8; ++kind)
    {
        AlrDetector::Config config;
        switch (kind)
        {
        case 0: config.bandwidth_usage_ratio = 0; break;
        case 1: config.bandwidth_usage_ratio = 1.1; break;
        case 2: config.bandwidth_usage_ratio = std::numeric_limits<double>::infinity(); break;
        case 3: config.start_budget_level_ratio = std::numeric_limits<double>::quiet_NaN(); break;
        case 4: config.stop_budget_level_ratio = -1.01; break;
        case 5: config.start_budget_level_ratio = 1.01; break;
        case 6: config.stop_budget_level_ratio = config.start_budget_level_ratio; break;
        case 7: config.stop_budget_level_ratio = std::numeric_limits<double>::quiet_NaN(); break;
        }
        bool threw = false;
        try { AlrDetector invalid(config); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
    bool threw = false;
    try { AlrDetector invalid({}, {}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    Fixture f({1.0, 0.8, 0.5}); f.detector.SetEstimatedBitrate(8000);
    for (const auto rate : {std::uint64_t(0), IntervalBudget::MaxRateBps + 500, std::numeric_limits<std::uint64_t>::max()})
    {
        threw = false;
        try { f.detector.SetEstimatedBitrate(rate); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
    f.detector.OnBytesSent(0, Time(1s));
    threw = false;
    try { f.detector.OnBytesSent(1000000, Time(900ms)); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    f.detector.OnBytesSent(0, Time(1400ms)); CHECK(!f.start());
    f.detector.OnBytesSent(0, Time(1401ms)); CHECK(f.start()); // invalid input preserved time and target rate

    Fixture wide; wide.detector.SetEstimatedBitrate(IntervalBudget::MaxRateBps);
    wide.detector.OnBytesSent(0, Time::min()); wide.detector.OnBytesSent(0, Time::max()); CHECK(wide.start());
}
class RecordingEstimator : public BitrateEstimator
{
public:
    bool in_alr{false}; int fast_changes{0};
    void Update(Time, std::size_t, bool alr) override { in_alr = alr; }
    void ExpectFastRateChange() override { ++fast_changes; }
};
void acknowledgedEstimatorHandoff()
{
    Fixture f; auto recording = std::make_unique<RecordingEstimator>(); auto* raw = recording.get();
    AcknowledgedBitrateEstimator acknowledged(std::move(recording));
    f.traffic(1s, 20); CHECK(f.start()); acknowledged.SetAlr(true);
    PacketResult packet; packet.sent_packet.size = 1000;
    packet.sent_packet.send_time = f.now; packet.receive_time = Time(100s);
    acknowledged.IncomingPacketFeedbackVector({packet}); CHECK(raw->in_alr && raw->fast_changes == 0);
    f.traffic(4s, 100); CHECK(!f.start());
    acknowledged.SetAlr(false);
    const auto feedback_time = f.now;
    acknowledged.SetAlrEndedTime(feedback_time); // controller observes the exit when feedback is processed
    packet.sent_packet.send_time = feedback_time; packet.receive_time = Time(101s);
    acknowledged.IncomingPacketFeedbackVector({packet}); CHECK(!raw->in_alr && raw->fast_changes == 0);
    packet.sent_packet.send_time = feedback_time + 1ms; packet.receive_time = Time(101s + 1ms);
    acknowledged.IncomingPacketFeedbackVector({packet}); CHECK(raw->fast_changes == 1);
}
void upstreamReplay()
{
    // Golden states/start times from the pinned original ALR and IntervalBudget
    // functions, with their units/environment replaced by standalone shims.
    const AlrDetector::Config configs[] = {AlrDetector::Config{}, baseConfig(), {0.9, 0, -0.1}};
    const std::chrono::microseconds steps[] = {0us, 499us, 500us, 1500us, 5ms, 10ms, 30ms, 50ms};
    const std::uint64_t rates[] = {300000, 60000, 123456, 800000, 2307, 300000};
    const int expected_enters[] = {2, 7, 5}, expected_exits[] = {1, 6, 4};
    const std::uint64_t expected_hashes[] = {16277197741455298810ULL, 13307352425876188752ULL, 8487246095392356067ULL};
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        Fixture f(configs[scenario]); f.now = Time(10s);
        std::uint32_t random = 0x616c7200U + scenario;
        const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        auto sent = 1000000us; int enters = 0, exits = 0; bool was = false;
        for (int i = 0; i < 1200; ++i)
        {
            const auto rate = rates[i / 200];
            if (i % 200 == 0) f.detector.SetEstimatedBitrate(rate);
            auto elapsed = steps[(next() >> 16) % 8];
            if (i % 257 == 256) elapsed = 1800ms;
            sent += elapsed; f.now += std::chrono::microseconds(12345 + (next() >> 16) % 1000);
            const int phase = i % 300;
            const int usage = phase < 80 ? 120 : (phase < 200 ? 10 : (phase < 220 ? 180 : 20));
            const auto bytes = rate * elapsed.count() * usage / 800000000 + (i % 37 == 0 ? 75 : 0);
            f.detector.OnBytesSent(bytes, Time(sent));
            const auto started = f.start(); const bool active = started.has_value();
            enters += active && !was; exits += !active && was; was = active;
            mix(active);
            if (started) mix(std::chrono::duration_cast<std::chrono::microseconds>(started->time_since_epoch()).count());
        }
        CHECK(enters == expected_enters[scenario] && exits == expected_exits[scenario]);
        CHECK(hash == expected_hashes[scenario] && f.clock_reads == enters);
    }
}
} // namespace
int main()
{
    try
    {
        upstreamScenarios(); thresholdsAndClock(); roundingAndLowRates(); validation(); acknowledgedEstimatorHandoff(); upstreamReplay();
        std::cout << "PASS: ALR traffic, thresholds, processing clock, rate rounding and estimator handoff\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
