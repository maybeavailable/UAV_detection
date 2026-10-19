#include"webrtc/transport_adapter.h"

#include<iostream>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = TransportTime;
void add(TransportAdapter& adapter, std::uint16_t seq, Time created, std::size_t size = 1000,
    std::size_t overhead = 0, std::optional<std::uint16_t> rtpSeq = {})
{ CHECK(adapter.AddPacket({seq, size, 42, rtpSeq.value_or(seq), false, false}, {}, overhead, created)); }
SentPacket send(TransportAdapter& adapter, std::uint16_t seq, Time at)
{ const auto result = adapter.ProcessSentPacket({seq, at, {true, true, 0}}); CHECK(result); return *result; }
RtcpTwcc report(std::uint16_t base, std::uint32_t reference, std::vector<std::optional<std::int32_t>> deltas)
{ RtcpTwcc f; f.baseSequence = base; f.referenceTime = reference; f.deltas = std::move(deltas); return f; }
void packetsAndIntervals()
{
    const auto t = Time(100s); TransportAdapter adapter; TransportFeedbackDeltaCalculator calculator;
    for (unsigned i = 0; i < 3; ++i) add(adapter, 10 + i, t, 1000, 28);
    CHECK(adapter.GetOutstandingData() == 0 && adapter.Snapshot().history_size == 3);
    for (unsigned i = 0; i < 3; ++i)
    { const auto packet = send(adapter, 10 + i, t + 1ms + i * 10ms); CHECK(packet.size == 1028 && packet.data_in_flight == (i + 1) * 1028); }
    const auto f = report(10, 100, {4, std::nullopt, 64});
    auto result = adapter.ProcessTransportFeedback(f, t + 100ms); CHECK(result);
    CHECK(result->packet_feedbacks.size() == 3 && result->ReceivedWithSendInfo().size() == 2 && result->LostWithSendInfo().size() == 1);
    CHECK(result->PacketsWithFeedback().size() == 3 && result->feedback_time == t + 100ms && result->data_in_flight == 0);
    CHECK(result->packet_feedbacks[0].receive_time == t + 101ms);
    CHECK(result->packet_feedbacks[2].remote_arrival_us == 6417000 && result->packet_feedbacks[1].reported_lost_for_the_first_time);
    auto deltas = calculator.Calculate(*result); CHECK(deltas.size() == 1);
    CHECK(deltas[0].previous_sequence_number == 10 && deltas[0].sequence_number == 12);
    CHECK(deltas[0].send_delta == 20ms && deltas[0].receive_delta == 16ms && deltas[0].delay_delta == -4ms);
    CHECK(adapter.Snapshot().history_size == 1 && adapter.GetOutstandingData() == 0);
    result = adapter.ProcessTransportFeedback(f, t + 101ms); CHECK(result && result->packet_feedbacks.size() == 1);
    CHECK(!result->packet_feedbacks[0].reported_lost_for_the_first_time && calculator.Calculate(*result).empty());
    result = adapter.ProcessTransportFeedback(report(11, 100, {1}), t + 102ms);
    CHECK(result && result->packet_feedbacks[0].reported_recovered_for_the_first_time);
    CHECK(calculator.Calculate(*result).empty() && adapter.Snapshot().history_size == 0);
    CHECK(!adapter.ProcessTransportFeedback(f, t + 103ms));
    add(adapter, 13, t + 30ms); send(adapter, 13, t + 31ms);
    result = adapter.ProcessTransportFeedback(report(13, 101, {8}), t + 110ms); CHECK(result);
    deltas = calculator.Calculate(*result); CHECK(deltas.size() == 1 && deltas[0].send_delta == 10ms && deltas[0].receive_delta == 49ms);
    CHECK(deltas[0].delay_delta == 39ms); // carries the previous report's last received packet
    adapter.Reset(1); add(adapter, 0, t + 120ms); send(adapter, 0, t + 121ms);
    result = adapter.ProcessTransportFeedback(report(0, 1, {4}), t + 130ms); CHECK(result);
    CHECK(calculator.Calculate(*result).empty()); // a new association starts a new interval baseline
}
void wrapReorderAndUnknown()
{
    const auto t = Time(100s); TransportAdapter adapter;
    add(adapter, 65535, t); send(adapter, 65535, t);
    const auto first = adapter.ProcessTransportFeedback(report(65535, 0xffffff, {1}), t + 50ms); CHECK(first);
    adapter.Clear();
    add(adapter, 0, t + 1ms); CHECK(send(adapter, 0, t + 1ms).sequence_number == 65536);
    const auto second = adapter.ProcessTransportFeedback(report(0, 0, {1}), t + 60ms); CHECK(second);
    CHECK(*second->packet_feedbacks[0].remote_arrival_us - *first->packet_feedbacks[0].remote_arrival_us == 64000);
    CHECK(*second->packet_feedbacks[0].receive_time - *first->packet_feedbacks[0].receive_time == 64ms);
    adapter.Reset(2);
    for (unsigned i = 0; i < 3; ++i) { add(adapter, i, t); send(adapter, i, t + i * 10ms); }
    auto result = adapter.ProcessTransportFeedback(report(0, 1, {4, -2, 8}), t + 100ms); CHECK(result);
    const auto sorted = result->SortedByReceiveTime();
    CHECK(sorted[0].sent_packet.sequence_number == 1 && sorted[1].sent_packet.sequence_number == 0 && sorted[2].sent_packet.sequence_number == 2);
    TransportFeedbackDeltaCalculator calculator; const auto deltas = calculator.Calculate(*result);
    CHECK(deltas.size() == 1 && deltas[0].previous_sequence_number == 1 && deltas[0].sequence_number == 2 && deltas[0].receive_delta == 2ms);
    adapter.Reset(3); add(adapter, 0, t); send(adapter, 0, t); add(adapter, 2, t); send(adapter, 2, t + 1ms);
    result = adapter.ProcessTransportFeedback(report(0, 1, {4, 4, 4}), t + 100ms); CHECK(result);
    CHECK(result->packet_feedbacks.size() == 2 && result->packet_feedbacks[1].remote_arrival_us == 67000);
    CHECK(adapter.Snapshot().failed_lookups == 1);
    adapter.Reset(4); add(adapter, 0, t); send(adapter, 0, t + 2ms); add(adapter, 1, t); send(adapter, 1, t + 1ms);
    result = adapter.ProcessTransportFeedback(report(0, 1, {1, 0}), t + 100ms); CHECK(result);
    CHECK(result->SortedByReceiveTime()[0].sent_packet.sequence_number == 1); // same arrival: order by send time before sequence
}
void retransmitAndUntracked()
{
    const auto t = Time(100s); TransportAdapter adapter;
    CHECK(!adapter.ProcessSentPacket({-1, t, {false, true, 77}}));
    add(adapter, 1, t); const auto first = send(adapter, 1, t + 1ms);
    CHECK(first.prior_unacked_data == 77 && first.data_in_flight == 1000);
    CHECK(!adapter.ProcessSentPacket({1, t + 2ms, {true, true, 0}}));
    CHECK(adapter.GetOutstandingData() == 1000 && adapter.Snapshot().retransmissions == 1);
    auto result = adapter.ProcessTransportFeedback(report(1, 1, {4}), t + 100ms); CHECK(result);
    CHECK(result->packet_feedbacks[0].ambiguous_receive_time && result->packet_feedbacks[0].sent_packet.send_time == t + 2ms);
    TransportFeedbackDeltaCalculator calculator; CHECK(calculator.Calculate(*result).empty());
    add(adapter, 2, t, 1000, 0, 10); send(adapter, 2, t + 10ms);
    add(adapter, 3, t, 1000, 0, 10); send(adapter, 3, t + 11ms);
    result = adapter.ProcessTransportFeedback(report(2, 1, {4, 4}), t + 100ms); CHECK(result);
    CHECK(result->packet_feedbacks[0].ambiguous_receive_time && result->packet_feedbacks[1].ambiguous_receive_time);
    CHECK(calculator.Calculate(*result).empty());
    RtpPacketSendInfo rtx{4, 1000, 77, 10, false, true}; rtx.original_ssrc = 42;
    CHECK(adapter.AddPacket(rtx, {}, 0, t + 12ms)); send(adapter, 4, t + 13ms);
    result = adapter.ProcessTransportFeedback(report(4, 1, {4}), t + 100ms); CHECK(result);
    CHECK(!result->packet_feedbacks[0].ambiguous_receive_time && result->packet_feedbacks[0].rtp_packet_info->is_retransmission);
    CHECK(!adapter.ProcessSentPacket({65536, t, {true, true, 0}}));
}
void routesLimitsAndFailures()
{
    const auto t = Time(100s); TransportAdapter adapter;
    add(adapter, 1, t); send(adapter, 1, t);
    TransportNetworkRoute route; route.remote_network_id = 1; adapter.SetNetworkRoute(route);
    CHECK(adapter.GetOutstandingData() == 0); add(adapter, 2, t); send(adapter, 2, t); CHECK(adapter.GetOutstandingData() == 1000);
    const auto result = adapter.ProcessTransportFeedback(report(1, 1, {1, 1}), t + 100ms); CHECK(result);
    CHECK(result->packet_feedbacks.size() == 1 && result->packet_feedbacks[0].sent_packet.sequence_number == 2);
    adapter.SetNetworkRoute({}); CHECK(adapter.GetOutstandingData() == 0 && adapter.Snapshot().ignored_route_packets == 1);
    TransportAdapter limited({10ms, 2});
    for (unsigned i = 0; i < 3; ++i) { add(limited, i, t + i * 1ms); send(limited, i, t + i * 1ms); }
    CHECK(limited.Snapshot().history_size == 2 && limited.GetOutstandingData() == 2000 && limited.Snapshot().expired_packets == 1);
    CHECK(!limited.AddPacket({2, 1000, 42, 2, false, false}, {}, 0, t + 3ms));
    CHECK(limited.RemovePacket(1) && !limited.RemovePacket(1) && limited.GetOutstandingData() == 1000);
    limited.Prune(t + 20ms); CHECK(limited.Snapshot().history_size == 0 && limited.GetOutstandingData() == 0);
    CHECK(limited.Snapshot().failed_sends == 1 && limited.Snapshot().expired_packets == 2);
    limited.Reset(7); add(limited, 0, t); CHECK(send(limited, 0, t).sequence_number == 0);
    CHECK(limited.Snapshot().generation == 7 && limited.Snapshot().failed_sends == 0);
    limited.Clear(); CHECK(limited.GetOutstandingData() == 0 && limited.Snapshot().history_size == 0);
}
void wireAndEarlyFeedback()
{
    const auto t = Time(100s); TransportAdapter adapter; add(adapter, 0, t);
    auto f = report(0, 1, {4}); CHECK(!adapter.ProcessTransportFeedback(f, t + 1ms)); // registered but not yet sent
    CHECK(send(adapter, 0, t + 2ms).data_in_flight == 0);
    std::vector<TransportPacketsFeedback> out(1);
    auto malformed = buildRtcpTwcc(f); malformed.insert(malformed.end(), {0, 0, 0, 0});
    CHECK(!adapter.ProcessRtcp(malformed, t + 3ms, out) && out.size() == 1 && adapter.Snapshot().history_size == 1);
    CHECK(adapter.ProcessRtcp(buildRtcpTwcc(f), t + 4ms, out) && out.size() == 1 && out[0].packet_feedbacks[0].sent_packet.send_time == t + 2ms);
    CHECK(adapter.ProcessRtcp(buildRtcpTwcc(f), t + 5ms, out) && out.empty());
    f.referenceTime = 0x1000000; CHECK(!adapter.ProcessTransportFeedback(f, t + 5ms));
    bool threw = false; try { TransportAdapter invalid({0ms, 1}); } catch (const std::invalid_argument&) { threw = true; } CHECK(threw);
}
void lateCompletionAfterWrap()
{
    const auto t = Time(100s); TransportAdapter adapter({60s, 2});
    for (unsigned i = 0; i <= 65536; ++i)
    {
        const auto seq = static_cast<std::uint16_t>(i); add(adapter, seq, t + i * 1us);
        CHECK(send(adapter, seq, t + i * 1us).sequence_number == i);
    }
    CHECK(adapter.GetOutstandingData() == 2000);
    CHECK(!adapter.RemovePacket(0) && !adapter.MarkRetransmitted(0, t + 1s)); // old full id, not the new wire id 0
    CHECK(adapter.GetOutstandingData() == 2000);
    const auto feedback = adapter.ProcessTransportFeedback(report(0, 1, {4}), t + 1s); CHECK(feedback);
    const auto& packet = feedback->packet_feedbacks[0];
    CHECK(packet.sent_packet.sequence_number == 65536 && packet.sent_packet.send_time == t + 65536us);
    CHECK(!packet.ambiguous_receive_time);
}
}
int main()
{
    try
    {
        packetsAndIntervals(); wrapReorderAndUnknown(); retransmitAndUntracked(); routesLimitsAndFailures(); wireAndEarlyFeedback(); lateCompletionAfterWrap();
        std::cout << "PASS: transport feedback registration/send, intervals, rollover, reorder, routes and bounds\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
