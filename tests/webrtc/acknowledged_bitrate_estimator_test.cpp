#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"

#include<iostream>
#include<limits>
#include<stdexcept>
#include<utility>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = TransportTime;
PacketResult packet(Time sent, Time received, std::size_t size = 1000, std::int64_t seq = 0)
{
    PacketResult p; p.sent_packet.send_time = sent; p.receive_time = received;
    p.sent_packet.size = size; p.sent_packet.sequence_number = seq; return p;
}
class RecordingEstimator : public BitrateEstimator
{
public:
    struct Event { bool fast; Time at; std::size_t bytes; bool alr; };
    std::vector<Event> events;
    void Update(Time at, std::size_t bytes, bool alr) override { events.push_back({false, at, bytes, alr}); }
    void ExpectFastRateChange() override { events.push_back({true, {}, 0, false}); }
    std::optional<std::uint64_t> bitrate() const override { return 42000; }
};
void forwardingAndAlr()
{
    auto recorder = std::make_unique<RecordingEstimator>(); auto* raw = recorder.get();
    AcknowledgedBitrateEstimator e(std::move(recorder));
    CHECK(e.bitrate() == 42000 && !e.PeekRate());
    e.SetAlr(true); e.SetAlrEndedTime(Time(101ms)); e.IncomingPacketFeedbackVector({});
    auto p = packet(Time(100ms), Time(10s), 10); p.sent_packet.prior_unacked_data = 200;
    p.ambiguous_receive_time = true; // still counts bytes, like upstream
    e.IncomingPacketFeedbackVector({p, packet(Time(101ms), Time(10s + 10ms), 20)});
    CHECK(raw->events.size() == 2);
    CHECK(!raw->events[0].fast && raw->events[0].at == Time(10s) && raw->events[0].bytes == 210 && raw->events[0].alr);
    CHECK(!raw->events[1].fast && raw->events[1].bytes == 20 && raw->events[1].alr);
    e.SetAlr(false);
    e.IncomingPacketFeedbackVector({packet(Time(102ms), Time(11s)), packet(Time(103ms), Time(11s + 1ms))});
    CHECK(raw->events.size() == 5 && raw->events[2].fast);
    CHECK(!raw->events[3].fast && !raw->events[3].alr && !raw->events[4].fast);
    // A second ALR exit can arm another transition, using send time strictly >.
    e.SetAlrEndedTime(Time(110ms));
    e.IncomingPacketFeedbackVector({packet(Time(110ms), Time(12s)), packet(Time(111ms), Time(12s + 1ms))});
    CHECK(raw->events.size() == 8 && !raw->events[5].fast && raw->events[6].fast);
}
void inputValidation()
{
    bool threw = false;
    try { AcknowledgedBitrateEstimator e(std::unique_ptr<BitrateEstimator>{}); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    auto recorder = std::make_unique<RecordingEstimator>(); auto* raw = recorder.get();
    AcknowledgedBitrateEstimator e(std::move(recorder)); e.SetAlrEndedTime(Time(0ms));
    const auto a = packet(Time(1ms), Time(10ms), 10, 1);
    const auto b = packet(Time(2ms), Time(20ms), 10, 2);
    auto bad = b; bad.receive_time.reset();
    const auto rejected = [&](const std::vector<PacketResult>& packets) {
        bool rejected = false;
        try { e.IncomingPacketFeedbackVector(packets); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && raw->events.empty());
    };
    rejected({a, bad}); bad = b; bad.sent_packet.send_time.reset(); rejected({a, bad});
    rejected({b, a});
    bad = b; bad.sent_packet.size = std::numeric_limits<std::size_t>::max();
    bad.sent_packet.prior_unacked_data = 1; rejected({a, bad});
    auto tie = a; tie.sent_packet.sequence_number = 0; rejected({a, tie});
    e.IncomingPacketFeedbackVector({tie, a, b}); // invalid batches did not consume the pending ALR exit
    CHECK(raw->events.size() == 4 && raw->events[0].fast);
}
void realEstimator()
{
    AcknowledgedBitrateEstimator e;
    CHECK(!e.bitrate() && !e.PeekRate());
    std::vector<PacketResult> packets;
    for (int i = 0; i <= 50; ++i) packets.push_back(packet(Time(i * 10ms), Time(1s + i * 10ms)));
    e.IncomingPacketFeedbackVector({packets.begin(), packets.begin() + 20});
    CHECK(!e.bitrate() && e.PeekRate());
    e.IncomingPacketFeedbackVector({packets.begin() + 20, packets.end()});
    CHECK(e.bitrate() == 800000 && !e.PeekRate());

    // Changing feedback arrival time / batching does not change arrival throughput.
    AcknowledgedBitrateEstimator single; single.IncomingPacketFeedbackVector(packets);
    CHECK(single.bitrate() == e.bitrate() && single.PeekRate() == e.PeekRate());
    AcknowledgedBitrateEstimator::Config config; config.initial_window = 150ms;
    AcknowledgedBitrateEstimator shorter(config); shorter.IncomingPacketFeedbackVector(packets);
    CHECK(shorter.bitrate() == 800000);
}
void transportFeedbackPipeline()
{
    TransportFeedbackAdapter adapter; AcknowledgedBitrateEstimator e;
    const Time t(100s);
    for (int i = 0; i <= 50; ++i)
    {
        const auto sent = t + i * 10ms;
        adapter.ProcessSentPacket({-1, sent, {false, true, 100}}); // allocation-only audio bytes
        CHECK(adapter.AddPacket({static_cast<std::uint16_t>(i), 1000, 42,
            static_cast<std::uint16_t>(i), false, false}, {}, 28, sent));
        const auto result = adapter.ProcessSentPacket({i, sent, {true, true, 0}});
        CHECK(result && result->size == 1028 && result->prior_unacked_data == 100);
    }
    for (int base = 0; base <= 40; base += 20)
    {
        RtcpTwcc report; report.baseSequence = static_cast<std::uint16_t>(base); report.referenceTime = 100;
        report.feedbackCount = static_cast<std::uint8_t>(base / 20);
        const int count = base < 40 ? 20 : 11;
        report.deltas.resize(count, 40); report.deltas[0] = base * 40;
        auto feedback = adapter.ProcessTransportFeedback(report, t + 250ms + base * 10ms);
        CHECK(feedback && feedback->SortedByReceiveTime().size() == static_cast<std::size_t>(count));
        e.IncomingPacketFeedbackVector(feedback->SortedByReceiveTime());
        if (base < 40) CHECK(!e.bitrate());
        // Previously confirmed packets aren't counted twice.
        CHECK(!adapter.ProcessTransportFeedback(report, t + 251ms + base * 10ms));
    }
    CHECK(e.bitrate() == 902400); // 50 * (1000 + 28 + 100) * 8 / 0.5 seconds
    CHECK(adapter.GetOutstandingData() == 0);

    TransportPacketsFeedback mixed;
    mixed.packet_feedbacks = {packet(Time(0ms), Time(1s)), packet(Time(500ms), Time(1500ms))};
    PacketResult lost; lost.sent_packet.send_time = Time(200ms); lost.sent_packet.size = 9999;
    mixed.packet_feedbacks.insert(mixed.packet_feedbacks.begin() + 1, lost);
    PacketResult unsent; unsent.receive_time = Time(1100ms); mixed.packet_feedbacks.push_back(unsent);
    AcknowledgedBitrateEstimator filtered;
    filtered.IncomingPacketFeedbackVector(mixed.SortedByReceiveTime());
    CHECK(filtered.bitrate() == 16000); // lost/unsent packets do not become acknowledged bytes
}
} // namespace
int main()
{
    try
    {
        forwardingAndAlr(); inputValidation(); realEstimator(); transportFeedbackPipeline();
        std::cout << "PASS: acknowledged feedback, byte accounting, ALR transitions and TWCC pipeline\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
