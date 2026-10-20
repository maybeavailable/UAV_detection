#include"webrtc/congestion_control/delay_based_bwe.h"
#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"
#include<algorithm>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = DelayBasedBwe::Time;
using Usage = BandwidthUsage;
struct Predictor : NetworkStatePredictor
{
    Usage state{Usage::kBwNormal};
    int calls{0}; std::int64_t send_ms{0}, receive_ms{0};
    Usage Update(std::int64_t send, std::int64_t receive, Usage) override
    { ++calls; send_ms = send; receive_ms = receive; return state; }
};
TransportPacketsFeedback feedback(Time at, Time send, Time receive, bool audio = false)
{
    TransportPacketsFeedback msg; msg.feedback_time = at;
    PacketResult p; p.sent_packet.send_time = send; p.receive_time = receive;
    p.sent_packet.size = 1200; p.sent_packet.audio = audio;
    msg.packet_feedbacks.push_back(p); return msg;
}
DelayBasedBwe::Result consume(DelayBasedBwe& e, const TransportPacketsFeedback& f,
    std::optional<std::uint64_t> ack = 200000, std::optional<std::uint64_t> probe = std::nullopt)
{ return e.IncomingPacketFeedbackVector(f, ack, probe, std::nullopt, false); }
void resultAndOveruseBranches()
{
    Predictor predictor; DelayBasedBwe e({}, &predictor);
    CHECK(!e.LatestEstimate() && e.last_estimate() == 0 && e.last_state() == Usage::kBwNormal);
    std::vector<std::uint32_t> ssrcs{123}; std::uint64_t rate = 42;
    CHECK(!e.LatestEstimate(&ssrcs, &rate) && ssrcs == std::vector<std::uint32_t>{123} && rate == 42);
    auto f = feedback(Time(1s), Time(900ms), Time(2s));
    auto r = consume(e, f);
    CHECK(!r.updated && !r.probe && r.target_bitrate == 30000000 && !r.recovered_from_overuse);
    e.SetStartBitrate(300000); CHECK(e.LatestEstimate() == 300000 && e.last_estimate() == 0);
    r = consume(e, f); CHECK(r.updated && r.target_bitrate == 301000);
    CHECK(e.LatestEstimate(&ssrcs, &rate) && ssrcs == std::vector<std::uint32_t>{0} && rate == 301000);
    e.OnRttUpdate(80ms);
    predictor.state = Usage::kBwOverusing;
    f.feedback_time = Time(1100ms);
    r = consume(e, f, 150000, 1000000);
    CHECK(r.updated && !r.probe && r.target_bitrate == 122500 && r.delay_detector_state == Usage::kBwOverusing);
    f.feedback_time = Time(1101ms); r = consume(e, f, 150000);
    CHECK(!r.updated && !r.probe && r.target_bitrate == 0 && e.last_estimate() == 122500);
    r = consume(e, f, 40000); CHECK(r.updated && r.target_bitrate == 29000); // Half-rate escape before RTT.
    f.feedback_time = Time(1180ms); CHECK(!consume(e, f, 20000).updated);
    f.feedback_time = Time(1181ms); CHECK(consume(e, f, 20000).updated);

    predictor.state = Usage::kBwUnderusing;
    f.feedback_time = Time(1200ms); r = consume(e, f);
    CHECK(r.updated && !r.recovered_from_overuse);
    predictor.state = Usage::kBwNormal;
    f.feedback_time = Time(1220ms); r = consume(e, f);
    CHECK(r.updated && r.recovered_from_overuse);
    predictor.state = Usage::kBwUnderusing; consume(e, f);
    predictor.state = Usage::kBwNormal; r = consume(e, f, 200000, 500000);
    CHECK(r.updated && r.probe && r.target_bitrate == 500000 && !r.recovered_from_overuse);
    predictor.state = Usage::kBwOverusing; consume(e, f);
    predictor.state = Usage::kBwNormal; CHECK(!consume(e, f).recovered_from_overuse);

    Predictor over; over.state = Usage::kBwOverusing; DelayBasedBwe no_ack({}, &over);
    CHECK(!consume(no_ack, f, std::nullopt).updated && !no_ack.LatestEstimate());
    no_ack.SetStartBitrate(600000);
    CHECK(consume(no_ack, f, std::nullopt).target_bitrate == 300000);
    CHECK(consume(no_ack, f, std::nullopt).target_bitrate == 150000); // Same control time: source behavior.
    for (int i = 0; i < 10; ++i) consume(no_ack, f, std::nullopt);
    CHECK(no_ack.LatestEstimate() == 5000);
    const auto logged_rate = e.last_estimate();
    CHECK(e.TriggerOveruse(Time(2s), 100000) == 80000);
    CHECK(e.last_estimate() == logged_rate); // TriggerOveruse does not publish a Result or change last_state.
}
void audioSwitchAndTimeout()
{
    DelayBasedBwe::Config config; config.separate_audio.enabled = true;
    config.separate_audio.packet_threshold = 3; config.separate_audio.time_threshold = 100ms;
    Predictor predictor; DelayBasedBwe e(config, &predictor); e.SetStartBitrate(600000);
    consume(e, feedback(Time(1s), Time(1s), Time(2s)));
    predictor.state = Usage::kBwOverusing;
    for (int i = 1; i <= 4; ++i)
    {
        auto r = consume(e, feedback(Time(1s + i * 20ms), Time(1s + i * 20ms), Time(2100ms + i * 1ms), true));
        CHECK(r.delay_detector_state == (i > 3 ? Usage::kBwOverusing : Usage::kBwNormal));
    }
    predictor.state = Usage::kBwNormal;
    CHECK(consume(e, feedback(Time(1100ms), Time(1100ms), Time(2200ms))).delay_detector_state == Usage::kBwNormal);
    predictor.state = Usage::kBwOverusing;
    for (int i = 1; i <= 4; ++i)
        CHECK(consume(e, feedback(Time(1100ms + i * 20ms), Time(1100ms + i * 20ms), Time(2200ms + i * 1ms), true)).delay_detector_state == Usage::kBwNormal);
    // Enough audio packets but not enough receiver-clock time since last video.
    CHECK(consume(e, feedback(Time(1300ms), Time(1300ms), Time(2300ms), true)).delay_detector_state == Usage::kBwNormal);
    CHECK(consume(e, feedback(Time(1320ms), Time(1320ms), Time(2300ms + 1us), true)).delay_detector_state == Usage::kBwOverusing);

    DelayBasedBwe timeout; timeout.SetStartBitrate(600000);
    for (int i = 0; i < 80; ++i)
        consume(timeout, feedback(Time(1s + i * 30ms), Time(1s + i * 20ms), Time(2s + i * 23ms)));
    CHECK(timeout.last_state() == Usage::kBwOverusing);
    // Old send timestamps cannot update the existing group; exactly 2 s retains it.
    CHECK(consume(timeout, feedback(Time(5370ms), Time(0s), Time(0s))).delay_detector_state == Usage::kBwOverusing);
    CHECK(consume(timeout, feedback(Time(7370ms + 1us), Time(0s), Time(0s))).delay_detector_state == Usage::kBwNormal);
    CHECK(timeout.LatestEstimate().has_value()); // Timeout resets delay filters, not AIMD.
}
void filteringPrecisionAndValidation()
{
    Predictor predictor; DelayBasedBwe e({}, &predictor);
    auto msg = feedback(Time(1s), Time(500ms + 499us), Time(1500ms + 500us));
    auto lost = msg.packet_feedbacks[0]; lost.receive_time.reset();
    auto unknown = msg.packet_feedbacks[0]; unknown.sent_packet.send_time.reset();
    msg.packet_feedbacks.push_back(lost); msg.packet_feedbacks.push_back(unknown);
    consume(e, msg); CHECK(predictor.calls == 1 && predictor.send_ms == 500 && predictor.receive_ms == 1501);
    msg.packet_feedbacks = {lost, unknown};
    auto r = consume(e, msg, 200000, 900000);
    CHECK(!r.updated && !r.probe && r.target_bitrate == 0 && predictor.calls == 1);
    CHECK(!e.LatestEstimate()); // An empty report cannot install a probe result.
    bool caught = false;
    msg = feedback(Time(1s), Time(800ms), Time(2s));
    auto invalid = msg.packet_feedbacks.front(); invalid.receive_time = Time(-1ns); msg.packet_feedbacks.push_back(invalid);
    try { consume(e, msg); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught && predictor.calls == 1);
    caught = false; msg.packet_feedbacks.pop_back(); msg.feedback_time = Time(999ms);
    try { consume(e, msg); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught && predictor.calls == 1);
    caught = false;
    try { e.LatestEstimate(nullptr, &r.target_bitrate); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught);

    DelayBasedBwe precision; precision.SetStartBitrate(300000); std::uint64_t last = 300000;
    for (int i = 0; i < 2000; ++i)
    {
        const Time receive(1s + (i / 2) * 20ms + (i % 2 ? 10000us : 500us));
        const Time send(1s + (i / 2) * 20ms + (i % 2 ? 9750us : 0us));
        r = consume(precision, feedback(receive + 100ms, send, receive), 1000000);
        CHECK(r.updated && r.target_bitrate >= last && r.delay_detector_state != Usage::kBwOverusing);
        last = r.target_bitrate;
    }
}
void upstreamReplay()
{
    // Original DelayBasedBwe + AIMD + InterArrivalDelta + Trendline + LinkCapacity
    // using original WebRTC integer units, fixed revision in NOTICE.
    constexpr std::uint64_t golden[] = {821294530892416631ULL, 11580957190917312589ULL, 6237881192464225111ULL, 1797104477311743401ULL};
    constexpr int expected_counts[4][3] = {{445, 62, 93}, {445, 62, 93}, {470, 44, 86}, {510, 50, 40}};
    for (int s = 0; s < 4; ++s)
    {
        DelayBasedBwe::Config config; config.aimd.no_bitrate_increase_in_alr = s == 3;
        if (s == 2) { config.separate_audio.enabled = true; config.separate_audio.packet_threshold = 3; config.separate_audio.time_threshold = 100ms; }
        Predictor predictor; DelayBasedBwe e(config, s == 3 ? &predictor : nullptr);
        if (s != 0) e.SetStartBitrate(600000);
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        std::int64_t recv = 2000000, offset = 0; int counts[3]{};
        for (int i = 0; i < 600; ++i)
        {
            if (i == 400) offset += 2000001;
            std::int64_t recv_delta = i < 80 ? 20000 : (i < 180 ? 23000 : (i < 260 ? 17000 : 20000));
            if (i >= 450) recv_delta += (i % 3 - 1) * 250;
            recv += recv_delta; TransportPacketsFeedback msg;
            msg.feedback_time = Time(std::chrono::microseconds(1000000 + i * 30000LL + offset));
            for (int j = 0; j < (s == 1 ? 4 : 1); ++j)
            {
                PacketResult p;
                p.sent_packet.send_time = Time(std::chrono::microseconds(1000000 + i * 20000LL + j * 2000 + offset + (i % 2 ? 275 : 0)));
                p.receive_time = Time(std::chrono::microseconds(recv + j * 1800 + offset));
                p.sent_packet.size = j % 2 ? 100 : 1200; p.sent_packet.sequence_number = i * 4 + j;
                p.sent_packet.audio = (i >= 100 && i < 180) || i % 7 == 0; msg.packet_feedbacks.push_back(p);
            }
            if (s == 1) std::reverse(msg.packet_feedbacks.begin(), msg.packet_feedbacks.end());
            std::optional<std::uint64_t> ack, probe;
            if (i >= 45 && i % 13 != 0) ack = i < 180 ? 450000 : 650000;
            if (i == 300 || i == 420) probe = 900000;
            predictor.state = i < 80 ? Usage::kBwNormal : (i < 120 ? Usage::kBwOverusing : (i < 170 ? Usage::kBwUnderusing : Usage::kBwNormal));
            std::optional<NetworkStateEstimate> network;
            if (i >= 320 && i < 360) network = NetworkStateEstimate{200000, 800000};
            const auto r = e.IncomingPacketFeedbackVector(msg, ack, probe, network, i >= 280 && i < 380);
            ++counts[static_cast<int>(r.delay_detector_state)];
            mix(r.updated); mix(r.probe); mix(r.target_bitrate); mix(r.recovered_from_overuse); mix(static_cast<int>(r.delay_detector_state));
            mix(e.last_estimate()); mix(static_cast<int>(e.last_state()));
            const auto rate = e.LatestEstimate(); mix(rate.has_value()); if (rate) mix(*rate);
        }
        CHECK(hash == golden[s]);
        for (int i = 0; i < 3; ++i) CHECK(counts[i] == expected_counts[s][i]);
    }
}
void twccPipeline()
{
    for (int receive_units : {80, 92, 68})
    {
        TransportFeedbackAdapter adapter; AcknowledgedBitrateEstimator ack; DelayBasedBwe bwe;
        bwe.SetStartBitrate(600000); const Time t(100s);
        for (int base = 0; base < 100; base += 20)
        {
            for (int i = base; i < base + 20; ++i)
            {
                const auto sent = t + i * 20ms;
                CHECK(adapter.AddPacket({static_cast<std::uint16_t>(i), 1200, 42, static_cast<std::uint16_t>(i), false, false}, {}, 28, sent));
                CHECK(adapter.ProcessSentPacket({i, sent, {true, true, 0}}));
            }
            RtcpTwcc report; report.baseSequence = static_cast<std::uint16_t>(base); report.referenceTime = 100;
            report.feedbackCount = static_cast<std::uint8_t>(base / 20); report.deltas.resize(20, receive_units);
            report.deltas[0] = base * receive_units;
            std::vector<TransportPacketsFeedback> messages;
            CHECK(adapter.ProcessRtcp(buildRtcpTwcc(report), t + 200ms + (base + 20) * 25ms, messages));
            CHECK(messages.size() == 1);
            ack.IncomingPacketFeedbackVector(messages[0].SortedByReceiveTime());
            const auto result = consume(bwe, messages[0], ack.bitrate());
            CHECK(result.updated || result.delay_detector_state == Usage::kBwOverusing);
        }
        const auto expected = receive_units == 80 ? Usage::kBwNormal : (receive_units > 80 ? Usage::kBwOverusing : Usage::kBwUnderusing);
        CHECK(bwe.last_state() == expected && ack.bitrate() && adapter.GetOutstandingData() == 0);
        CHECK(bwe.LatestEstimate() && (receive_units != 92 || *bwe.LatestEstimate() < 600000));
    }
}
} // namespace
int main()
{
    try
    {
        resultAndOveruseBranches(); audioSwitchAndTimeout(); filteringPrecisionAndValidation(); upstreamReplay(); twccPipeline();
        std::cout << "PASS: delay BWE branches, audio switching, timeout, microseconds, 2400 upstream reports and real TWCC/ACK pipeline\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
