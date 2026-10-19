#include"webrtc/congestion_control/inter_arrival_delta.h"
#include"webrtc/transport_adapter.h"

#include<cstdint>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = InterArrivalDelta::Time;
using Delta = InterArrivalDelta::Delta;
struct Fixture
{
    explicit Fixture(Delta length = 5ms) : inter(length) {}
    InterArrivalDelta inter;
    Delta send_delta{777us}, arrival_delta{888us}; int size_delta{999};
    bool input(Delta send, Delta arrival, std::size_t bytes = 100, std::optional<Delta> system = {})
    {
        return inter.ComputeDeltas(Time(send), Time(arrival), Time(system.value_or(send)), bytes,
            &send_delta, &arrival_delta, &size_delta);
    }
    void check(Delta send, Delta arrival, int bytes) const
    { CHECK(send_delta == send && arrival_delta == arrival && size_delta == bytes); }
};
void groupsAndSizes()
{
    Fixture f;
    CHECK(!f.input(0ms, 100ms, 100));
    CHECK(!f.input(5ms, 105ms, 200)); // exactly group length still belongs to this group
    CHECK(!f.input(6ms, 111ms, 400));
    f.check(777us, 888us, 999); // no completed pair yet
    CHECK(f.input(12ms, 117ms, 500)); f.check(1ms, 6ms, 100);
    CHECK(f.input(18ms, 123ms, 200)); f.check(6ms, 6ms, 100);
    CHECK(f.input(24ms, 129ms, 300)); f.check(6ms, 6ms, -300);

    Fixture custom(20ms);
    CHECK(!custom.input(0ms, 100ms)); CHECK(!custom.input(20ms, 120ms));
    CHECK(!custom.input(21ms, 126ms)); CHECK(custom.input(42ms, 147ms));
    custom.check(1ms, 6ms, -100);
    Fixture zeroLength(0us);
    CHECK(!zeroLength.input(0ms, 0ms, 0)); CHECK(!zeroLength.input(1ms, 10ms, 0));
    CHECK(zeroLength.input(2ms, 20ms, 0)); zeroLength.check(1ms, 10ms, 0);
}
void bursts()
{
    Fixture f;
    CHECK(!f.input(0ms, 0ms, 100));
    CHECK(!f.input(10ms, 5ms, 200)); // 5 ms arrival gap included, although sends are > 5 ms apart
    CHECK(!f.input(20ms, 10001us, 300)); // 5 ms + 1 us arrival gap ends the burst
    CHECK(f.input(30ms, 20001us, 400)); f.check(10ms, 5001us, 0);

    Fixture equalPropagation;
    CHECK(!equalPropagation.input(0ms, 0ms)); CHECK(!equalPropagation.input(5ms, 5ms));
    CHECK(!equalPropagation.input(10ms, 10ms)); // propagation delta == 0 must not merge
    CHECK(equalPropagation.input(20ms, 20ms)); equalPropagation.check(5ms, 5ms, -100);

    Fixture duration;
    CHECK(!duration.input(0ms, 0ms, 100)); CHECK(!duration.input(0ms, 95ms, 200));
    CHECK(!duration.input(10ms, 99999us, 300)); // total burst duration just below 100 ms
    CHECK(!duration.input(20ms, 100ms, 400)); // exactly 100 ms starts a new group
    CHECK(duration.input(30ms, 110ms, 500)); duration.check(10ms, 1us, -200);

    Fixture sameSend;
    CHECK(!sameSend.input(0ms, 0ms, 100)); CHECK(!sameSend.input(0ms, 1s, 200));
    CHECK(!sameSend.input(10ms, 1010ms, 300)); CHECK(sameSend.input(20ms, 1020ms));
    sameSend.check(10ms, 10ms, 0); // identical send time merges even after 100 ms
}
void reorderedPacketsAndGroups()
{
    Fixture f;
    CHECK(!f.input(10ms, 100ms, 100)); CHECK(!f.input(14ms, 104ms, 200));
    CHECK(!f.input(12ms, 106ms, 300)); // send reorder inside the group retains its max send time
    CHECK(!f.input(9ms, 110ms, 9999)); // earlier than first_send_time: ignored completely
    CHECK(!f.input(20ms, 120ms, 400)); CHECK(f.input(30ms, 130ms, 500));
    f.check(6ms, 14ms, -200);

    Fixture reset;
    CHECK(!reset.input(0ms, 100ms)); CHECK(!reset.input(10ms, 110ms));
    CHECK(!reset.input(10ms, 90ms)); // current group's last arrival is now before the previous group's
    CHECK(!reset.input(20ms, 120ms)); CHECK(!reset.input(30ms, 130ms));
    CHECK(!reset.input(40ms, 140ms)); // third detection resets; triggering packet is discarded
    CHECK(!reset.input(50ms, 150ms)); CHECK(!reset.input(60ms, 160ms));
    CHECK(reset.input(70ms, 170ms)); reset.check(10ms, 10ms, 0);

    Fixture recovery;
    CHECK(!recovery.input(0ms, 100ms)); CHECK(!recovery.input(10ms, 110ms));
    CHECK(!recovery.input(10ms, 90ms)); CHECK(!recovery.input(20ms, 120ms));
    CHECK(!recovery.input(30ms, 130ms)); // two detections, no reset
    CHECK(!recovery.input(10ms, 110ms)); CHECK(recovery.input(20ms, 120ms));
    recovery.check(10ms, 10ms, 200); // late same-group packets were accumulated
    CHECK(!recovery.input(20ms, 90ms)); CHECK(!recovery.input(30ms, 130ms));
    CHECK(!recovery.input(40ms, 140ms)); // healthy delta reset the consecutive counter
    CHECK(!recovery.input(20ms, 120ms)); CHECK(recovery.input(30ms, 130ms));
    recovery.check(10ms, 10ms, 0);

    Fixture sameArrival;
    CHECK(!sameArrival.input(0ms, 100ms)); CHECK(!sameArrival.input(10ms, 106ms));
    CHECK(!sameArrival.input(10ms, 100ms)); CHECK(sameArrival.input(20ms, 110ms));
    sameArrival.check(10ms, 0ms, 100); // zero arrival delta is valid
}
void clockOffsets()
{
    static_assert(InterArrivalDelta::kReorderedResetThreshold == 3);
    static_assert(InterArrivalDelta::kArrivalTimeOffsetThreshold == 3s);
    Fixture below;
    CHECK(!below.input(0ms, 100ms)); CHECK(!below.input(10ms, 3109999us));
    CHECK(below.input(20ms, 3120ms)); below.check(10ms, 3009999us, 0);

    Fixture reset;
    CHECK(!reset.input(0ms, 100ms)); CHECK(!reset.input(10ms, 3110ms));
    CHECK(!reset.input(20ms, 3120ms)); // arrival delta - system delta == 3 s
    CHECK(!reset.input(30ms, 3130ms)); CHECK(!reset.input(40ms, 3140ms));
    CHECK(reset.input(50ms, 3150ms)); reset.check(10ms, 10ms, 0);

    Fixture bothClocks;
    CHECK(!bothClocks.input(0ms, 100ms, 100, 500ms));
    CHECK(!bothClocks.input(10ms, 3110ms, 100, 3510ms));
    CHECK(bothClocks.input(20ms, 3120ms, 100, 3520ms));
    bothClocks.check(10ms, 3010ms, 0); // compare clock deltas, not raw arrival gap

    Fixture localBackward;
    CHECK(!localBackward.input(0ms, 100ms, 100, 10s));
    CHECK(!localBackward.input(10ms, 110ms, 100, 7s));
    CHECK(!localBackward.input(20ms, 120ms, 100, 7010ms));
    CHECK(!localBackward.input(30ms, 130ms, 100, 7020ms)); // reset discarded boundary packet
}
void precisionAndValidation()
{
    InterArrivalDelta precise(5ms);
    Delta send{0}, arrival{0}; int size = 0;
    CHECK(!precise.ComputeDeltas(Time(999ns), Time(100ms + 999ns), Time(0ms), 100, &send, &arrival, &size));
    CHECK(!precise.ComputeDeltas(Time(6ms + 100ns), Time(110ms + 100ns), Time(10ms), 200, &send, &arrival, &size));
    CHECK(precise.ComputeDeltas(Time(12ms + 900ns), Time(120ms + 900ns), Time(20ms), 300, &send, &arrival, &size));
    CHECK(send == 6ms && arrival == 10ms && size == 100);
    bool threw = false;
    try { InterArrivalDelta invalid(-1us); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    for (int null = 0; null < 3; ++null)
    {
        threw = false;
        try { precise.ComputeDeltas(Time(0ms), Time(0ms), Time(0ms), 100,
            null == 0 ? nullptr : &send, null == 1 ? nullptr : &arrival, null == 2 ? nullptr : &size); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
    constexpr auto max = static_cast<std::size_t>(std::numeric_limits<int>::max());
    Fixture large; CHECK(!large.input(0ms, 100ms, max));
    threw = false;
    try { large.input(1ms, 101ms, 1); } catch (const std::overflow_error&) { threw = true; }
    CHECK(threw); // rejected packet did not change group size or timestamps
    threw = false;
    try { large.input(1ms, 101ms, max + 1); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    CHECK(!large.input(10ms, 110ms, 0)); CHECK(large.input(20ms, 120ms));
    large.check(10ms, 10ms, -std::numeric_limits<int>::max());

    InterArrivalDelta wide(5ms);
    CHECK(!wide.ComputeDeltas(Time::min(), Time::min(), Time::min(), 100, &send, &arrival, &size));
    const auto middle = Time::max() - 20ms;
    CHECK(!wide.ComputeDeltas(middle, middle, middle, 100, &send, &arrival, &size));
    CHECK(wide.ComputeDeltas(middle + 10ms, middle + 10ms, middle + 10ms, 100, &send, &arrival, &size));
    CHECK(send == arrival && send.count() > 0 && size == 0);
}
void transportFeedbackPipeline()
{
    TransportFeedbackAdapter adapter; InterArrivalDelta inter(5ms);
    const Time t(100s);
    for (int i = 0; i < 6; ++i)
    {
        const auto sent = t + i * 10ms;
        CHECK(adapter.AddPacket({static_cast<std::uint16_t>(i), static_cast<std::size_t>(1000 + i * 100),
            42, static_cast<std::uint16_t>(i), false, false}, {}, 28, sent));
        CHECK(adapter.ProcessSentPacket({i, sent, {true, true, 0}}));
    }
    int calculated = 0;
    for (int base = 0; base < 6; base += 3)
    {
        RtcpTwcc report; report.baseSequence = static_cast<std::uint16_t>(base); report.referenceTime = 100;
        report.feedbackCount = static_cast<std::uint8_t>(base / 3);
        report.deltas = {base * 48, 48, 48}; // receiver: 12 ms, sender: 10 ms
        const auto feedback = adapter.ProcessTransportFeedback(report, t + 50ms + base * 10ms);
        CHECK(feedback);
        for (const auto& packet : feedback->SortedByReceiveTime())
        {
            Delta send{0}, arrival{0}; int bytes = 0;
            if (inter.ComputeDeltas(*packet.sent_packet.send_time, *packet.receive_time,
                feedback->feedback_time, packet.sent_packet.size, &send, &arrival, &bytes))
            {
                CHECK(send == 10ms && arrival == 12ms && arrival - send == 2ms && bytes == 100);
                ++calculated;
            }
        }
    }
    CHECK(calculated == 4); // state survives across feedback batches; the last group remains incomplete
}
void upstreamReplay()
{
    // Counts and hashes generated by the original pinned .h/.cc functions,
    // with only Timestamp/TimeDelta and logging replaced by standalone shims.
    const Delta steps[] = {0us, 1us, 1ms, 4ms, 5ms, 6ms, 10ms, 20ms};
    const Delta lengths[] = {0us, 5ms, 20ms};
    const int expected_counts[] = {265, 167, 94};
    const std::uint64_t expected_hashes[] = {16060637390523881499ULL, 6345560736064221735ULL, 8389188433752972098ULL};
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        InterArrivalDelta inter(lengths[scenario]);
        std::uint32_t random = 0x1234abcdU + scenario;
        const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
        std::uint64_t hash = 14695981039346656037ULL; int calculated = 0;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        Delta send = 1s, arrival = 2s, system = 3s;
        for (int i = 0; i < 600; ++i)
        {
            send += steps[(next() >> 16) % 8]; arrival += steps[(next() >> 16) % 8];
            if (i % 5 == 0) system += 10ms;
            if (i > 0 && i % 71 == 0) arrival -= 10ms;
            if (i > 0 && i % 137 == 0) arrival += 3s;
            if (i > 0 && i % 211 == 0) arrival -= 2s;
            const auto actual_send = send - (i > 0 && i % 97 == 0 ? 30ms : 0ms);
            const auto bytes = 1 + (next() >> 16) % 1400;
            Delta send_delta{0}, arrival_delta{0}; int size_delta = 0;
            const bool valid = inter.ComputeDeltas(Time(actual_send), Time(arrival), Time(system), bytes,
                &send_delta, &arrival_delta, &size_delta);
            mix(valid);
            if (valid)
            {
                ++calculated; mix(send_delta.count()); mix(arrival_delta.count()); mix(size_delta);
            }
        }
        CHECK(calculated == expected_counts[scenario] && hash == expected_hashes[scenario]);
    }
}
} // namespace
int main()
{
    try
    {
        groupsAndSizes(); bursts(); reorderedPacketsAndGroups(); clockOffsets();
        precisionAndValidation(); transportFeedbackPipeline(); upstreamReplay();
        std::cout << "PASS: inter-arrival groups, bursts, reordering, clock offsets and TWCC pipeline\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
