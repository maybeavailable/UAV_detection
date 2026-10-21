// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// Adapted regression scenarios; see third_party/webrtc/LICENSE and NOTICE.
#include"webrtc/congestion_control/probe_bitrate_estimator.h"
#include"webrtc/congestion_control/probe_controller.h"
#include"webrtc/congestion_control/delay_based_bwe.h"
#include"reference/probe_bitrate_estimator_replay.h"
#include<array>
#include<cmath>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = TransportTime;
PacketResult packet(int id, std::size_t size, std::int64_t send_us, std::int64_t receive_us, int min_probes = 5, int min_bytes = 5000)
{
    PacketResult p;
    p.sent_packet.send_time = Time(1000s + std::chrono::microseconds(send_us));
    p.receive_time = Time(1000s + std::chrono::microseconds(receive_us));
    p.sent_packet.size = size; p.sent_packet.pacing_info = {id, min_probes, min_bytes, 0};
    return p;
}
void upstreamScenarios()
{
    struct Case { std::array<std::int64_t, 4> send, receive; std::optional<std::uint64_t> expected; };
    const Case cases[] = {
        {{{0, 10000, 20000, 30000}}, {{10000, 20000, 30000, 40000}}, 800000},
        {{{0, 10000, 20000, 30000}}, {{15000, 30000, 35000, 40000}}, 800000},
        {{{0, 10000, 20000, 40000}}, {{19000, 22000, 25000, 27000}}, std::nullopt},
        {{{0, 10000, 20000, 30000}}, {{10000, 40000, 70000, 85000}}, 304000},
        {{{0, 10000, 20000, 40000}}, {{50000, 50000, 50000, 50000}}, std::nullopt},
        {{{0, 10000, 20000, 30000}}, {{0, 5000, 10000, 15000}}, 800000}, // Ratio == 2 is valid.
        {{{0, 10000, 20000, 30000}}, {{0, 5000, 10000, 14999}}, std::nullopt}
    };
    for (const auto& c : cases)
    {
        ProbeBitrateEstimator e; std::optional<std::uint64_t> result;
        for (int i = 0; i < 4; ++i) result = e.HandleProbeAndEstimateBitrate(packet(0, 1000, c.send[i], c.receive[i]));
        CHECK(result == c.expected);
    }
    ProbeBitrateEstimator small;
    for (int i = 0; i < 6; ++i) (void)small.HandleProbeAndEstimateBitrate(packet(0, 150, i * 10000, 10000 + i * 10000, 5, 1000));
    CHECK(small.FetchAndResetLastEstimatedBitrate() == 120000);
    ProbeBitrateEstimator large;
    for (int i = 0; i < 25; ++i) (void)large.HandleProbeAndEstimateBitrate(packet(0, 12500, i * 1000, 5000 + i * 1000, 30, 312500));
    CHECK(large.FetchAndResetLastEstimatedBitrate() == 100000000);
    ProbeBitrateEstimator few;
    for (int i = 0; i < 3; ++i) CHECK(!few.HandleProbeAndEstimateBitrate(packet(0, 2000, i * 10000, i * 10000)));
    ProbeBitrateEstimator bytes;
    for (int i = 0; i < 5; ++i) CHECK(!bytes.HandleProbeAndEstimateBitrate(packet(0, 800, i * 10000, i * 10000, 5, 6000)));
}
void roundingAndIntervals()
{
    ProbeBitrateEstimator thresholds;
    CHECK(!thresholds.HandleProbeAndEstimateBitrate(packet(0, 2, 0, 0, 2, 6)));
    CHECK(!thresholds.HandleProbeAndEstimateBitrate(packet(0, 2, 10, 10, 2, 6))); // round(6 * .8) == 5 bytes.
    CHECK(thresholds.HandleProbeAndEstimateBitrate(packet(0, 1, 20, 20, 2, 6)) == 1140000);
    ProbeBitrateEstimator count;
    CHECK(!count.HandleProbeAndEstimateBitrate(packet(0, 1, 0, 0, 3, 1)));
    CHECK(count.HandleProbeAndEstimateBitrate(packet(0, 1, 1, 1, 3, 1)) == 8000000); // floor(3 * .8) == 2.
    ProbeBitrateEstimator high;
    for (int i = 0; i < 4; ++i) (void)high.HandleProbeAndEstimateBitrate(packet(0, 12500, i * 1000, i * 1250));
    CHECK(high.FetchAndResetLastEstimatedBitrate() == 75999999); // Original 0.95f, not double 0.95.
    ProbeBitrateEstimator exact_threshold;
    CHECK(!exact_threshold.HandleProbeAndEstimateBitrate(packet(0, 10, 0, 0, 2, 20)));
    CHECK(exact_threshold.HandleProbeAndEstimateBitrate(packet(0, 9, 1000000, 1000000, 2, 20)) == 72); // R == round(0.9f * S).
    ProbeBitrateEstimator slower;
    CHECK(!slower.HandleProbeAndEstimateBitrate(packet(0, 10, 0, 0, 2, 20)));
    CHECK(slower.HandleProbeAndEstimateBitrate(packet(0, 8, 1000000, 1000000, 2, 20)) == 61);
    ProbeBitrateEstimator too_long;
    CHECK(!too_long.HandleProbeAndEstimateBitrate(packet(0, 10, 0, 0, 2, 20)));
    CHECK(!too_long.HandleProbeAndEstimateBitrate(packet(0, 9, 1000001, 1000000, 2, 20)));
    ProbeBitrateEstimator metadata;
    for (int i = 0; i < 3; ++i) CHECK(!metadata.HandleProbeAndEstimateBitrate(packet(0, 1000, i * 10000, i * 10000, 100, 5000)));
    CHECK(metadata.HandleProbeAndEstimateBitrate(packet(0, 1000, 30000, 30000)) == 800000); // Gates use this packet's metadata.
}
void extremaAndCache()
{
    ProbeBitrateEstimator e; CHECK(!e.FetchAndResetLastEstimatedBitrate());
    for (int i = 0; i < 4; ++i) (void)e.HandleProbeAndEstimateBitrate(packet(0, 1000, i * 10000, 10000 + i * 10000));
    CHECK(e.FetchAndResetLastEstimatedBitrate() == 800000 && !e.FetchAndResetLastEstimatedBitrate());
    CHECK(e.Snapshot().clusters == 1); // Fetch only clears the cached rate.
    CHECK(e.HandleProbeAndEstimateBitrate(packet(0, 1500, 40000, 50000)) == 800000);
    CHECK(!e.HandleProbeAndEstimateBitrate(packet(0, 1000, 2000000, 50000))); // Invalid duration preserves the previous success.
    CHECK(e.FetchAndResetLastEstimatedBitrate() == 800000);

    ProbeBitrateEstimator first;
    (void)first.HandleProbeAndEstimateBitrate(packet(0, 1500, 0, 10000));
    for (int i = 1; i < 4; ++i) (void)first.HandleProbeAndEstimateBitrate(packet(0, 1000, i * 10000, 10000 + i * 10000));
    CHECK(first.FetchAndResetLastEstimatedBitrate() == 760000);
    ProbeBitrateEstimator ties;
    (void)ties.HandleProbeAndEstimateBitrate(packet(0, 1000, 0, 10000));
    (void)ties.HandleProbeAndEstimateBitrate(packet(0, 2000, 10000, 10000));
    (void)ties.HandleProbeAndEstimateBitrate(packet(0, 1000, 30000, 30000));
    CHECK(ties.HandleProbeAndEstimateBitrate(packet(0, 2000, 30000, 40000)) == 1333333);
    ProbeBitrateEstimator reordered;
    const int sizes[] = {1000, 1500, 500, 1000};
    for (int i : {3, 0, 2, 1}) (void)reordered.HandleProbeAndEstimateBitrate(packet(0, sizes[i], i * 10000, 10000 + i * 10000));
    CHECK(reordered.FetchAndResetLastEstimatedBitrate() == 800000);
    // Last successful estimate wins across clusters, even when it is smaller.
    for (int i = 0; i < 4; ++i) (void)reordered.HandleProbeAndEstimateBitrate(packet(1, 1000, i * 20000, 10000 + i * 20000));
    CHECK(reordered.FetchAndResetLastEstimatedBitrate() == 400000);

    ProbeBitrateEstimator age;
    for (int i = 0; i < 3; ++i) (void)age.HandleProbeAndEstimateBitrate(packet(0, 1000, i * 10000, i * 10000));
    (void)age.HandleProbeAndEstimateBitrate(packet(1, 1000, 1000000, 1020000, 100, 5000));
    CHECK(age.Snapshot().clusters == 2); // Exactly 1 second after cluster 0's last receive.
    (void)age.HandleProbeAndEstimateBitrate(packet(1, 1000, 1000001, 1020001, 100, 5000));
    CHECK(age.Snapshot().clusters == 1);
    for (int i = 0; i < 4; ++i) (void)age.HandleProbeAndEstimateBitrate(packet(2, 1000, 1100000 + i * 10000, 1100000 + i * 10000));
    (void)age.HandleProbeAndEstimateBitrate(packet(3, 1000, 3000000, 3000000));
    CHECK(age.FetchAndResetLastEstimatedBitrate() == 800000); // History pruning does not clear the result.
}
void invalidInputAndNumericLimits()
{
    ProbeBitrateEstimator e;
    for (int i = 0; i < 4; ++i)
    {
        auto p = packet(0, 1000, i * 10000, i * 10000);
        p.sent_packet.prior_unacked_data = 100000; p.wire_size = 50000; p.ambiguous_receive_time = true;
        (void)e.HandleProbeAndEstimateBitrate(p);
    }
    CHECK(e.Snapshot().last_estimated_bitrate == 800000);
    auto non_probe = packet(PacedPacketInfo::kNotAProbe, 1000, 6000000, 6000000);
    CHECK(!e.HandleProbeAndEstimateBitrate(non_probe));
    auto lost = packet(1, 1000, 6000000, 6000000); lost.receive_time.reset(); CHECK(!e.HandleProbeAndEstimateBitrate(lost));
    auto unknown = packet(1, 1000, 6000000, 6000000); unknown.sent_packet.send_time.reset(); CHECK(!e.HandleProbeAndEstimateBitrate(unknown));
    CHECK(e.Snapshot().clusters == 1 && e.Snapshot().last_estimated_bitrate == 800000);
    for (int i = 0; i < 4; ++i)
    {
        auto p = packet(1, 1000, 6000000, 6000000);
        if (i == 0) p.sent_packet.pacing_info.probe_cluster_min_probes = 0;
        if (i == 1) p.sent_packet.pacing_info.probe_cluster_min_bytes = 0;
        if (i == 2) p.sent_packet.send_time = Time(-1ns);
        if (i == 3) p.receive_time = Time(-1ns);
        bool caught = false;
        try { (void)e.HandleProbeAndEstimateBitrate(p); } catch (const std::invalid_argument&) { caught = true; }
        CHECK(caught && e.Snapshot().clusters == 1 && e.Snapshot().last_estimated_bitrate == 800000);
    }
    ProbeBitrateEstimator zero;
    for (int i = 0; i < 4; ++i) CHECK(!zero.HandleProbeAndEstimateBitrate(packet(0, 0, i * 1000, i * 1000)));
    CHECK(!zero.HandleProbeAndEstimateBitrate(packet(0, 1000, 4000, 4000, 5, 1000))); // Empty send measurement.
    const auto maximum = std::numeric_limits<std::size_t>::max();
    ProbeBitrateEstimator overflow;
    CHECK(!overflow.HandleProbeAndEstimateBitrate(packet(0, maximum, 0, 0)));
    bool caught = false;
    try { (void)overflow.HandleProbeAndEstimateBitrate(packet(0, 1, 1, 1)); } catch (const std::overflow_error&) { caught = true; }
    CHECK(caught && overflow.Snapshot().clusters == 1);
    ProbeBitrateEstimator saturated;
    for (int i = 0; i < 4; ++i) (void)saturated.HandleProbeAndEstimateBitrate(packet(0, maximum / 8, i, i));
    CHECK(saturated.FetchAndResetLastEstimatedBitrate() == std::numeric_limits<std::uint64_t>::max());
}
void upstreamReplay()
{
    constexpr std::uint64_t golden[] = {7051392418720493801ULL, 17356404290788060270ULL, 21228492111449351ULL,
        14849201854695396060ULL, 13803019247750384729ULL, 17526069811486394852ULL};
    constexpr int successes[] = {2800, 1854, 2778, 400, 3600, 1400};
    constexpr int fetched[] = {400, 368, 400, 145, 462, 200};
    for (int s = 0; s < 6; ++s)
    {
        ProbeBitrateEstimator e; probe_bitrate_replay::Input input(s); probe_bitrate_replay::Hash hash;
        int success_count = 0, fetched_count = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const auto sample = input.sample(i, s);
            auto p = packet(sample.cluster_id, sample.size, sample.send_us, sample.receive_us, sample.min_probes, sample.min_bytes);
            const auto result = e.HandleProbeAndEstimateBitrate(p); hash.rate(result); success_count += result.has_value();
            if (sample.fetch)
            {
                const auto last = e.FetchAndResetLastEstimatedBitrate(); hash.rate(last); fetched_count += last.has_value();
                hash.rate(e.FetchAndResetLastEstimatedBitrate());
            }
            const auto stats = e.Snapshot(); hash.mix(stats.clusters); hash.rate(stats.last_estimated_bitrate);
        }
        if (hash.value != golden[s]) std::cerr << "scenario " << s << " actual=" << hash.value << " expected=" << golden[s] << '\n';
        CHECK(hash.value == golden[s] && success_count == successes[s] && fetched_count == fetched[s]);
    }
}
void controllerTwccDelayPipeline()
{
    ProbeController controller; ProbeBitrateEstimator estimator; TransportFeedbackAdapter adapter; DelayBasedBwe delay;
    const Time start(100s); delay.SetStartBitrate(300000);
    CHECK(controller.OnNetworkAvailability({start, true}).empty());
    const auto clusters = controller.SetBitrates(30000, 300000, 5000000, start); CHECK(clusters.size() == 2);
    for (int index = 0; index < 2; ++index)
    {
        const auto& cluster = clusters[index];
        PacedPacketInfo pacing; pacing.probe_cluster_id = cluster.id; pacing.probe_cluster_min_probes = cluster.target_probe_count;
        pacing.probe_cluster_min_bytes = static_cast<int>(std::llround(cluster.target_data_rate * double(cluster.target_duration.count()) / 8000000));
        pacing.send_bitrate_bps = cluster.target_data_rate;
        const auto spacing = index == 0 ? 10ms : 5ms;
        for (int i = 0; i < 5; ++i)
        {
            const auto seq = index * 5 + i; const auto sent = start + index * 100ms + i * spacing;
            CHECK(adapter.AddPacket({static_cast<std::uint16_t>(seq), 1200, 42, static_cast<std::uint16_t>(seq), false, false}, pacing, 0, sent));
            CHECK(adapter.ProcessSentPacket({seq, sent, {true, true, 0}}));
        }
        RtcpTwcc twcc; twcc.baseSequence = index * 5; twcc.referenceTime = 100; twcc.feedbackCount = index;
        twcc.deltas.resize(5, index == 0 ? 40 : 20); twcc.deltas[0] = index * 400;
        const auto now = start + 200ms + index * 100ms;
        const auto wire = buildRtcpTwcc(twcc); std::vector<TransportPacketsFeedback> messages;
        CHECK(adapter.ProcessRtcp(wire, now, messages) && messages.size() == 1);
        for (const auto& p : messages[0].SortedByReceiveTime()) (void)estimator.HandleProbeAndEstimateBitrate(p);
        const auto probe = estimator.FetchAndResetLastEstimatedBitrate(); CHECK(probe == (index == 0 ? 960000 : 1920000));
        const auto result = delay.IncomingPacketFeedbackVector(messages[0], std::nullopt, probe, std::nullopt, false);
        CHECK(result.updated && result.probe && result.target_bitrate == *probe);
        const auto further = controller.SetEstimatedBitrate(result.target_bitrate, BandwidthLimitedCause::kDelayBasedLimited, now);
        if (index == 0) CHECK(further.empty());
        else CHECK(further.size() == 1 && further[0].id > cluster.id && further[0].target_data_rate == 3840000);
        messages.clear(); CHECK(adapter.ProcessRtcp(wire, now + 1ms, messages) && messages.empty());
        CHECK(!estimator.FetchAndResetLastEstimatedBitrate());
    }
    CHECK(adapter.GetOutstandingData() == 0);
}
} // namespace
int main()
{
    try
    {
        upstreamScenarios(); roundingAndIntervals(); extremaAndCache(); invalidInputAndNumericLimits(); upstreamReplay(); controllerTwccDelayPipeline();
        std::cout << "PASS: probe bitrate thresholds, rates, float rounding, expiry, cache, 24000 upstream packets and controller/TWCC/delay pipeline\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
