// Copyright 2021 The WebRTC project authors. All rights reserved.
// Adapted regression scenarios; see third_party/webrtc/LICENSE and NOTICE.
#include"webrtc/congestion_control/loss_based_bwe_v2.h"
#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"
#include"webrtc/congestion_control/delay_based_bwe.h"
#include"reference/loss_bwe_v2_replay.h"
#include<algorithm>
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
PacketResult packet(std::int64_t sequence, Time sent, std::size_t size, bool received)
{
    PacketResult p; p.sent_packet.sequence_number = sequence; p.sent_packet.send_time = sent; p.sent_packet.size = size;
    if (received) p.receive_time = sent + 100ms;
    return p;
}
std::vector<PacketResult> pair(std::int64_t sequence, Time first, int loss_percent)
{
    return {packet(sequence, first, 15000, loss_percent != 100), packet(sequence + 1, first + 250ms, 15000, loss_percent == 0)};
}
LossBasedBweV2::Config shortConfig()
{
    LossBasedBweV2::Config c; c.min_num_observations = 1; c.observation_window_size = 2; return c;
}
void readinessAndFallback()
{
    LossBasedBweV2 e;
    CHECK(e.IsEnabled() && !e.IsReady() && e.UseInStartPhase() && !e.ReadyToUseInStartPhase());
    CHECK(!e.GetLossBasedResult().bandwidth_estimate);
    e.SetBandwidthEstimate(600000);
    CHECK(!e.IsReady() && !e.GetLossBasedResult().bandwidth_estimate);
    e.UpdateBandwidthEstimate({}, 700000, false);
    CHECK(!e.IsReady() && e.GetLossBasedResult().bandwidth_estimate == 700000);
    for (int i = 0; i < 3; ++i)
    {
        e.UpdateBandwidthEstimate(pair(i * 2, Time(i * 500ms), 0), 700000, false);
        CHECK(e.IsReady() == (i == 2));
        CHECK(e.Snapshot().observations == std::uint64_t(i + 1));
    }
    CHECK(e.ReadyToUseInStartPhase() && e.GetLossBasedResult().bandwidth_estimate);
    e.SetBandwidthEstimate(600000); CHECK(e.GetLossBasedResult().bandwidth_estimate == 600000);
    LossBasedBweV2 no_initial(shortConfig());
    no_initial.UpdateBandwidthEstimate(pair(0, Time(0s), 0), std::nullopt, false);
    CHECK(!no_initial.IsReady() && no_initial.Snapshot().observations == 1);
    no_initial.UpdateBandwidthEstimate(pair(2, Time(500ms), 0), 500000, false);
    CHECK(no_initial.IsReady() && no_initial.GetLossBasedResult().bandwidth_estimate);

    auto config = shortConfig(); config.enabled = false;
    LossBasedBweV2 disabled(config);
    CHECK(!disabled.IsEnabled() && !disabled.IsReady() && !disabled.UseInStartPhase() && !disabled.ReadyToUseInStartPhase());
    disabled.SetAcknowledgedBitrate(200000); disabled.SetMinMaxBitrate(5000, std::nullopt);
    disabled.SetBandwidthEstimate(600000); disabled.UpdateBandwidthEstimate({}, 400000, false);
    CHECK(disabled.GetLossBasedResult().bandwidth_estimate == 400000);
    CHECK(disabled.GetLossBasedResult().state == LossBasedState::kDelayBasedEstimate);
    disabled.UpdateBandwidthEstimate({}, std::nullopt, false); CHECK(!disabled.GetLossBasedResult().bandwidth_estimate);
}
void observationBoundariesAndRecovery()
{
    LossBasedBweV2 e(shortConfig()); e.SetBandwidthEstimate(2500000);
    e.UpdateBandwidthEstimate({packet(1, Time(0s), 15000, true), packet(2, Time(249999us), 15000, false)}, 2500000, false);
    CHECK(e.Snapshot().observations == 0 && e.Snapshot().partial_packets == 2 && e.Snapshot().pending_lost_packets == 1);
    // Recovery while the observation is still open erases the pending loss;
    // the feedback entry still contributes to its packet/byte totals upstream.
    e.UpdateBandwidthEstimate({packet(2, Time(249999us), 15000, true), packet(3, Time(250000us), 15000, true)}, 2500000, false);
    CHECK(e.IsReady() && e.Snapshot().observations == 1 && e.Snapshot().pending_lost_packets == 0);
    CHECK(e.Snapshot().average_loss_ratio == 0 && e.Snapshot().sending_rate == 1920000);
    CHECK(e.GetLossBasedResult().bandwidth_estimate == 2500000);
    const auto before = e.GetLossBasedResult();
    e.UpdateBandwidthEstimate({packet(4, Time(250000us), 1000, true)}, 2500000, false);
    CHECK(e.Snapshot().observations == 1 && e.Snapshot().partial_packets == 1);
    CHECK(e.GetLossBasedResult().bandwidth_estimate == before.bandwidth_estimate);
}
void byteLossAndSpikeFiltering()
{
    LossBasedBweV2 bytes(shortConfig()); bytes.SetBandwidthEstimate(500000);
    auto packet_config = shortConfig(); packet_config.use_byte_loss_rate = false;
    LossBasedBweV2 packets(packet_config); packets.SetBandwidthEstimate(500000);
    std::vector<PacketResult> samples;
    for (int i = 0; i < 10; ++i) samples.push_back(packet(i, Time(i * 250ms), i == 9 ? 300000 : 15000, i != 9));
    bytes.UpdateBandwidthEstimate(samples, std::nullopt, false); packets.UpdateBandwidthEstimate(samples, std::nullopt, false);
    CHECK(bytes.Snapshot().average_loss_ratio > .68 && std::abs(packets.Snapshot().average_loss_ratio - .1) < 1e-12);
    CHECK(bytes.GetLossBasedResult().state == LossBasedState::kDecreasing);
    CHECK(*bytes.GetLossBasedResult().bandwidth_estimate < 160000);
    CHECK(*packets.GetLossBasedResult().bandwidth_estimate > *bytes.GetLossBasedResult().bandwidth_estimate);

    LossBasedBweV2::Config c; c.observation_window_size = 5;
    LossBasedBweV2 spike(c); spike.SetBandwidthEstimate(500000);
    for (int i = 0; i < 5; ++i) spike.UpdateBandwidthEstimate(pair(i * 2, Time(i * 250ms), 0), 500000, false);
    spike.UpdateBandwidthEstimate(pair(10, Time(1250ms), 100), 500000, false);
    spike.UpdateBandwidthEstimate(pair(12, Time(1500ms), 0), 500000, false);
    CHECK(spike.GetLossBasedResult().bandwidth_estimate == 500000 && spike.GetLossBasedResult().state == LossBasedState::kDelayBasedEstimate);
    spike.UpdateBandwidthEstimate(pair(14, Time(1750ms), 100), 500000, false);
    CHECK(spike.GetLossBasedResult().state == LossBasedState::kDecreasing && *spike.GetLossBasedResult().bandwidth_estimate < 500000);

    LossBasedBweV2 burst(c); burst.SetBandwidthEstimate(500000);
    for (int i = 0; i < 5; ++i) burst.UpdateBandwidthEstimate(pair(i * 2, Time(i * 250ms), 0), 500000, false);
    samples.clear();
    for (int i = 0; i < 5; ++i) samples.push_back(packet(10 + i, Time(1250ms + (i == 4 ? 250ms : i * 10ms)), 15000, false));
    burst.UpdateBandwidthEstimate(samples, 500000, false);
    CHECK(burst.GetLossBasedResult().state == LossBasedState::kDecreasing);
}
void holdPaddingAndBounds()
{
    LossBasedBweV2 e(shortConfig()); e.SetBandwidthEstimate(600000); e.SetAcknowledgedBitrate(100000);
    e.UpdateBandwidthEstimate(pair(0, Time(0s), 100), 5000000, false);
    const auto loss = e.GetLossBasedResult();
    CHECK(loss.state == LossBasedState::kDecreasing && e.Snapshot().hold_duration == 600ms);
    bool padded = false;
    for (int i = 2; i < 30; ++i)
    {
        e.UpdateBandwidthEstimate(pair(i * 2, Time(i * 250ms), 0), 5000000, false);
        if (e.GetLossBasedResult().state == LossBasedState::kIncreaseUsingPadding)
        {
            CHECK(e.GetLossBasedResult().bandwidth_estimate == *loss.bandwidth_estimate * 12 / 10);
            padded = true; break;
        }
    }
    CHECK(padded);
    auto config = shortConfig(); config.padding_duration = 0us; config.hold_duration_factor = 0;
    LossBasedBweV2 no_padding(config); no_padding.SetBandwidthEstimate(500000);
    no_padding.UpdateBandwidthEstimate(pair(0, Time(0s), 100), std::nullopt, false);
    bool increased = false;
    for (int i = 1; i < 30; ++i)
    {
        no_padding.UpdateBandwidthEstimate(pair(i * 2, Time(i * 250ms), 0), std::nullopt, false);
        if (no_padding.GetLossBasedResult().state == LossBasedState::kIncreasing) { increased = true; break; }
    }
    CHECK(increased);
    e.SetMinMaxBitrate(30000, 400000); e.SetAcknowledgedBitrate(350000);
    e.UpdateBandwidthEstimate(pair(100, Time(10s), 100), 5000000, false);
    CHECK(*e.GetLossBasedResult().bandwidth_estimate >= 350000 && *e.GetLossBasedResult().bandwidth_estimate <= 400000);

    LossBasedBweV2 maximum(shortConfig()); maximum.SetBandwidthEstimate(600000);
    maximum.SetMinMaxBitrate(0, 100000);
    maximum.UpdateBandwidthEstimate(pair(0, Time(0s), 0), 1000000, false);
    CHECK(maximum.GetLossBasedResult().bandwidth_estimate == 100000);
    maximum.SetMinMaxBitrate(0, std::nullopt); maximum.SetBandwidthEstimate(600000);
    maximum.UpdateBandwidthEstimate(pair(2, Time(500ms), 0), 1000000, false);
    CHECK(*maximum.GetLossBasedResult().bandwidth_estimate > 100000 && !maximum.Snapshot().instant_upper_bound);
}
void invalidConfigAndInputs()
{
    for (int i = 0; i < 12; ++i)
    {
        LossBasedBweV2::Config c;
        if (i == 0) c.bandwidth_rampup_upper_bound_factor = 1;
        if (i == 1) c.candidate_factors = {1, -1};
        if (i == 2) { c.candidate_factors = {1}; c.append_acknowledged_rate_candidate = c.append_delay_based_estimate_candidate = false; }
        if (i == 3) c.newton_iterations = 0;
        if (i == 4) c.observation_window_size = 1;
        if (i == 5) c.instant_upper_bound_loss_offset = 1;
        if (i == 6) c.temporal_weight_factor = 0;
        if (i == 7) c.newton_step_size = std::numeric_limits<double>::quiet_NaN();
        if (i == 8) c.candidate_factors.push_back(std::numeric_limits<double>::infinity());
        if (i == 9) c.padding_duration = -1us;
        if (i == 10) c.inherent_loss_upper_bound_bandwidth_balance = std::numeric_limits<std::uint64_t>::max();
        if (i == 11) c.bandwidth_backoff_lower_bound_factor = -1;
        LossBasedBweV2 invalid(c);
        CHECK(!invalid.IsEnabled() && !invalid.IsReady() && !invalid.UseInStartPhase());
        invalid.SetAcknowledgedBitrate(100000); // Disabled state must not dereference an absent config.
    }
    LossBasedBweV2 e(shortConfig()); e.SetBandwidthEstimate(600000);
    for (int i = 0; i < 5; ++i)
    {
        auto samples = pair(0, Time(0s), 0);
        if (i == 0) samples.back().sent_packet.send_time.reset();
        if (i == 1) samples.back().sent_packet.sequence_number = -1;
        if (i == 2) samples.back().sent_packet.send_time = Time(-1ns);
        if (i == 3) samples.back().receive_time = Time(-1ns);
        if (i == 4) samples.back().sent_packet.size = std::numeric_limits<std::size_t>::max();
        bool caught = false;
        try { e.UpdateBandwidthEstimate(samples, 400000, false); } catch (const std::exception&) { caught = true; }
        CHECK(caught && e.Snapshot().observations == 0 && e.Snapshot().partial_packets == 0);
        CHECK(!e.GetLossBasedResult().bandwidth_estimate); // Invalid batch did not install its delay estimate.
    }
    bool caught = false;
    try { e.SetAcknowledgedBitrate(std::numeric_limits<std::uint64_t>::max()); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught);
    caught = false; try { e.SetMinMaxBitrate(200000, 100000); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught);
    e.SetMinMaxBitrate(0, std::nullopt); e.SetBandwidthEstimate(0);
    e.UpdateBandwidthEstimate({packet(0, Time(0s), 0, true), packet(1, Time(250ms), 0, false)}, 0, false);
    CHECK(e.IsReady() && e.Snapshot().average_loss_ratio == 0 && std::isfinite(e.Snapshot().inherent_loss));
    CHECK(e.GetLossBasedResult().bandwidth_estimate == 0); // Known zero is not unknown.
}
void upstreamReplay()
{
    constexpr std::uint64_t golden[] = {3113203604747807615ULL, 2677361131633951847ULL, 1544339977367727757ULL,
        11303872703812612633ULL, 15723771714285187135ULL, 632435164438942138ULL, 14238664985854672899ULL, 16585573159975137404ULL};
    constexpr int expected_counts[8][4] = {{0, 12, 342, 246}, {314, 0, 158, 128}, {0, 62, 201, 337}, {0, 114, 357, 129},
        {0, 60, 531, 9}, {0, 41, 271, 288}, {0, 175, 128, 297}, {0, 40, 295, 265}};
    for (int s = 0; s < 8; ++s)
    {
        LossBasedBweV2::Config config;
        loss_bwe_replay::configure(s, config, [](std::uint64_t v) { return v; }, [](std::int64_t v) { return std::chrono::microseconds(v); });
        LossBasedBweV2 e(config); if (s != 0 && s != 7) e.SetBandwidthEstimate(600000);
        loss_bwe_replay::Input input(s); loss_bwe_replay::Hash hash; int counts[4]{};
        for (int i = 0; i < 600; ++i)
        {
            std::vector<PacketResult> samples;
            for (const auto& p : input.packets(i)) samples.push_back(packet(p.sequence, Time(std::chrono::microseconds(p.send_us)), p.size, p.received));
            const auto ack = input.ack(i); if (i % 7 != 0) e.SetAcknowledgedBitrate(ack);
            if (i % 79 == 0) e.SetMinMaxBitrate(i % 2 ? 20000 : 5000, i % 3 ? 1600000 : 900000);
            if (i % 137 == 136) e.SetBandwidthEstimate(400000 + ack % 70000);
            e.UpdateBandwidthEstimate(samples, input.delay(i, s), (i / 40) % 2);
            const auto result = e.GetLossBasedResult(); const auto stats = e.Snapshot(); ++counts[static_cast<int>(result.state)];
            hash.rate(result.bandwidth_estimate); hash.mix(static_cast<int>(result.state)); hash.mix(e.IsReady());
            hash.mix(e.UseInStartPhase()); hash.mix(e.ReadyToUseInStartPhase());
            hash.mix(stats.observations); hash.mix(stats.partial_packets); hash.mix(stats.pending_lost_packets); hash.rate(stats.model_bandwidth);
            hash.rate(stats.sending_rate); hash.rate(stats.instant_lower_bound); hash.rate(stats.instant_upper_bound); hash.mix(stats.hold_duration.count());
            hash.mix(std::llround(stats.average_loss_ratio * 1e9)); hash.mix(std::llround(stats.inherent_loss * 1e9));
        }
        if (hash.value != golden[s]) std::cerr << "scenario " << s << " actual hash=" << hash.value << " expected=" << golden[s] << '\n';
        CHECK(hash.value == golden[s]);
        for (int i = 0; i < 4; ++i) CHECK(counts[i] == expected_counts[s][i]);
    }
}
void realTwccPipeline()
{
    for (bool lossy : {false, true})
    {
        TransportFeedbackAdapter adapter; AcknowledgedBitrateEstimator ack; DelayBasedBwe delay;
        LossBasedBweV2 loss(shortConfig()); delay.SetStartBitrate(600000); loss.SetBandwidthEstimate(600000);
        const Time start(100s);
        for (int base = 0; base < 120; base += 20)
        {
            for (int i = base; i < base + 20; ++i)
            {
                const auto sent = start + i * 20ms;
                CHECK(adapter.AddPacket({static_cast<std::uint16_t>(i), 1200, 42, static_cast<std::uint16_t>(i), false, false}, {}, 28, sent));
                CHECK(adapter.ProcessSentPacket({i, sent, {true, true, 0}}));
            }
            RtcpTwcc twcc; twcc.baseSequence = static_cast<std::uint16_t>(base); twcc.referenceTime = 100;
            twcc.feedbackCount = base / 20;
            int last_received = -1;
            for (int i = 0; i < 20; ++i)
            {
                if (lossy && i % 2) twcc.deltas.push_back(std::nullopt);
                else { twcc.deltas.push_back(last_received < 0 ? (base + i) * 80 : (i - last_received) * 80); last_received = i; }
            }
            std::vector<TransportPacketsFeedback> messages;
            CHECK(adapter.ProcessRtcp(buildRtcpTwcc(twcc), start + 500ms + base * 20ms, messages) && messages.size() == 1);
            auto& msg = messages[0]; const auto received = msg.SortedByReceiveTime();
            ack.IncomingPacketFeedbackVector(received);
            if (const auto rate = ack.bitrate()) loss.SetAcknowledgedBitrate(*rate);
            const auto delayed = delay.IncomingPacketFeedbackVector(msg, ack.bitrate(), std::nullopt, std::nullopt, false);
            const auto delay_rate = delayed.updated ? std::optional<std::uint64_t>(delayed.target_bitrate) : delay.LatestEstimate();
            loss.UpdateBandwidthEstimate(msg.packet_feedbacks, delay_rate, false);
            CHECK(msg.packet_feedbacks.size() == 20 && received.size() == (lossy ? 10 : 20));
        }
        CHECK(loss.IsReady() && loss.GetLossBasedResult().bandwidth_estimate && ack.bitrate());
        if (lossy) CHECK(loss.Snapshot().average_loss_ratio > .49 && loss.GetLossBasedResult().state == LossBasedState::kDecreasing);
        else CHECK(loss.Snapshot().average_loss_ratio == 0 && loss.GetLossBasedResult().state == LossBasedState::kDelayBasedEstimate);
    }
}
} // namespace
int main()
{
    try
    {
        readinessAndFallback(); observationBoundariesAndRecovery(); byteLossAndSpikeFiltering(); holdPaddingAndBounds();
        invalidConfigAndInputs(); upstreamReplay(); realTwccPipeline();
        std::cout << "PASS: loss BWE readiness, byte/packet models, recovery, HOLD/padding, bounds, 4800 upstream reports and TWCC pipeline\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
