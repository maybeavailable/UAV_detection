// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// Adapted regression scenarios; see third_party/webrtc/LICENSE and NOTICE.
#include"webrtc/congestion_control/goog_cc_network_control.h"
#include"webrtc/congestion_control/pacer.h"
#include"reference/goog_cc_network_control_replay.h"
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = TransportTime;
const Time start{1000s};
NetworkControllerConfig config()
{
    NetworkControllerConfig c; c.constraints.at_time = start; c.constraints.starting_rate = 300000;
    c.constraints.max_data_rate = 5000000; return c;
}
NetworkControlUpdate initialize(GoogCcNetworkController& c)
{ (void)c.OnNetworkAvailability({start, true}); return c.OnProcessInterval({start, {}}); }
PacketResult packet(std::int64_t seq, Time sent, std::optional<Time> received)
{
    PacketResult p; p.sent_packet.send_time = sent; p.sent_packet.size = 1200;
    p.sent_packet.sequence_number = seq; p.receive_time = received; return p;
}
template<class F> void invalid(F f)
{ bool threw = false; try { f(); } catch (const std::invalid_argument&) { threw = true; } CHECK(threw); }
void startupAndRemoteLimits()
{
    GoogCcNetworkController c(config()); auto u = initialize(c);
    CHECK(u.target_rate && u.target_rate->target_rate == 300000);
    CHECK(u.pacer_config && u.pacer_config->data_rate() == 750000);
    CHECK(u.probe_cluster_configs.size() == 2 && u.probe_cluster_configs[0].target_data_rate == 900000 &&
        u.probe_cluster_configs[1].target_data_rate == 1800000);
    CHECK(!c.OnProcessInterval({start + 25ms, {}}).has_updates());
    CHECK(!c.OnRemoteBitrateReport({start + 25ms, 600000}).has_updates());
    u = c.OnProcessInterval({start + 50ms, {}}); CHECK(u.target_rate && u.target_rate->target_rate == 600000);
    (void)c.OnRemoteBitrateReport({start + 50ms, 200000});
    u = c.OnProcessInterval({start + 75ms, {}}); CHECK(u.target_rate && u.target_rate->target_rate == 200000);
    (void)c.OnRoundTripTimeUpdate({start + 75ms, 100ms, true});
    CHECK(!c.OnProcessInterval({start + 100ms, {}}).target_rate);
    (void)c.OnRoundTripTimeUpdate({start + 100ms, 100ms, false});
    u = c.OnProcessInterval({start + 125ms, {}}); CHECK(u.target_rate->network_estimate.round_trip_time == 100ms);
    StreamsConfig streams; streams.at_time = start + 125ms; streams.pacing_factor = 4; streams.max_padding_rate = 100000;
    u = c.OnStreamsConfig(streams); CHECK(u.pacer_config->data_rate() == 800000 && u.pacer_config->pad_rate() == 100000);
    CHECK(!c.SupportsEcnAdaptation()); CHECK(c.GetProcessInterval() == 25ms);
}
void constraintsAndRoutes()
{
    GoogCcNetworkController c(config()); (void)initialize(c);
    TargetRateConstraints constraints; constraints.at_time = start + 1s; constraints.min_data_rate = 100000;
    constraints.max_data_rate = 50000; constraints.starting_rate = 1000;
    auto u = c.OnTargetRateConstraints(constraints); CHECK(u.target_rate->target_rate == 100000);
    NetworkRouteChange route; route.at_time = start + 2s; route.constraints.at_time = route.at_time;
    route.constraints.starting_rate = 200000; route.generation = 2;
    u = c.OnNetworkRouteChange(route); CHECK(u.target_rate->target_rate == 200000 && u.probe_cluster_configs.size() == 2);
    TransportPacketsFeedback stale; stale.feedback_time = start + 3s; stale.generation = 1;
    stale.packet_feedbacks.push_back(packet(0, start + 2s, start + 2s));
    CHECK(!c.OnTransportPacketsFeedback(stale).has_updates());
    stale.generation = 2; u = c.OnTransportPacketsFeedback(stale); CHECK(u.target_rate && u.pacer_config);
    CHECK(u.pacer_config->data_rate() >= u.target_rate->target_rate);
    // A route event before the first tick must not be overwritten by old startup constraints.
    GoogCcNetworkController early(config()); (void)early.OnNetworkAvailability({start, true});
    route.at_time = start; route.constraints.at_time = start; route.constraints.starting_rate = 700000;
    u = early.OnNetworkRouteChange(route); CHECK(u.target_rate->target_rate == 700000);
    (void)early.OnProcessInterval({start + 25ms, {}}); CHECK(early.GetNetworkState(start + 25ms).target_rate->target_rate == 700000);
    // Delayed first timer uses the current event clock for initialization.
    GoogCcNetworkController late(config()); (void)late.OnNetworkAvailability({start + 1s, true});
    u = late.OnProcessInterval({start + 2s, {}}); CHECK(u.probe_cluster_configs.size() == 2);
}
void cwndAndRateWindows()
{
    CongestionWindowPushbackController p; p.SetDataWindow(10000); p.UpdateOutstandingData(15000);
    CHECK(p.UpdateTargetBitrate(1000000) == 950000);
    p.UpdateOutstandingData(16000); CHECK(p.UpdateTargetBitrate(1000000) == 855000);
    for (int i = 0; i < 100; ++i) (void)p.UpdateTargetBitrate(1000000);
    CHECK(p.UpdateTargetBitrate(1000000) == 30000); CHECK(p.UpdateTargetBitrate(10000) == 10000);
    p.UpdateOutstandingData(0); CHECK(p.UpdateTargetBitrate(1000000) == 1000000);
    const auto zero = PacerConfig::Create(start, 123457, 8001, 0us);
    CHECK(zero.time_window == 0us && zero.data_window == 15432 && zero.data_rate() == 123456 && zero.pad_rate() == 8000);
    const auto short_window = PacerConfig::Create(start, 5000, 0, 40ms);
    CHECK(short_window.data_window == 25 && short_window.data_rate() == 5000);
    CHECK(PacerConfig::Create(start, 99, 0, 40ms).data_window == 0);
    CHECK(PacerConfig::Create(start, 100, 0, 40ms).data_window == 1); // DataRate * TimeDelta rounds to nearest byte.
    for (const bool drop_frames : {true, false})
    {
        GoogCcConfig g; g.drop_frame_only = drop_frames; g.pushback.initial_data_window = 10000;
        GoogCcNetworkController c(config(), std::move(g)); (void)initialize(c);
        SentPacket sent; sent.send_time = start + 1ms; sent.size = 1200; sent.data_in_flight = 20000;
        const auto u = c.OnSentPacket(sent); CHECK(u.target_rate && u.pacer_config);
        CHECK(u.pacer_config->data_rate() == 750000);
        if (drop_frames) CHECK(u.target_rate->target_rate == 300000 && u.target_rate->cwnd_reduce_ratio > 0);
        else CHECK(u.target_rate->target_rate < 300000 && u.target_rate->cwnd_reduce_ratio == 0);
        // The snapshot deliberately exposes the pushback target, as upstream does.
        CHECK(c.GetNetworkState(start + 1ms).target_rate->target_rate < 300000);
    }
}
void legacyLossAndRttBackoff()
{
    SendSideBandwidthEstimation::Config cfg; cfg.loss.enabled = false;
    SendSideBandwidthEstimation b(cfg); b.SetBitrates(100000, 5000, 1000000, start);
    b.UpdatePacketsLost(0, 19, start); CHECK(b.fraction_loss() == 0 && b.target_rate() == 100000);
    b.UpdatePacketsLost(0, 1, start); CHECK(b.target_rate() == 109000);
    b.UpdatePacketsLost(20, 100, start + 1s); CHECK(b.fraction_loss() == 51);
    const auto reduced = b.target_rate(); CHECK(reduced < 109000);
    b.UpdateEstimate(start + 2s); CHECK(b.target_rate() == reduced);
    b.UpdatePropagationRtt(start + 2s, 4s);
    b.UpdateEstimate(start + 3s); CHECK(b.target_rate() < reduced && b.IsRttAboveLimit());
    const auto after = b.target_rate(); b.UpdateEstimate(start + 3500ms); CHECK(b.target_rate() == after);
    b.UpdatePropagationRtt(start + 4s, 0us); CHECK(!b.IsRttAboveLimit());
    invalid([&] { b.SetMinMaxBitrate(1000000001, {}); });
}
void twccProbeAndPacingPipeline()
{
    GoogCcNetworkController c(config()); auto update = initialize(c);
    const auto clusters = update.probe_cluster_configs;
    Pacer pacer; CHECK(pacer.setRates(update.pacer_config->data_rate(), update.pacer_config->pad_rate(), start));
    TransportFeedbackAdapter adapter; adapter.Reset(1);
    for (int index = 0; index < 2; ++index)
    {
        const auto& cluster = clusters[index];
        PacedPacketInfo pacing{cluster.id, 5, 5000, cluster.target_data_rate};
        for (int i = 0; i < 5; ++i)
        {
            const int seq = index * 5 + i;
            const auto sent = start + index * 300ms + i * (index == 0 ? 10ms : 5ms);
            CHECK(adapter.AddPacket({static_cast<std::uint16_t>(seq), 1200, 42, static_cast<std::uint16_t>(seq), false, false}, pacing, 0, sent));
            const auto record = adapter.ProcessSentPacket({seq, sent, {true, true, 0}}); CHECK(record);
            (void)c.OnSentPacket(*record);
        }
        RtcpTwcc twcc; twcc.baseSequence = index * 5; twcc.referenceTime = 100; twcc.feedbackCount = index;
        twcc.deltas.resize(5, index == 0 ? 40 : 20); twcc.deltas[0] = index * 1200;
        const auto now = start + 200ms + index * 300ms;
        std::vector<TransportPacketsFeedback> feedback;
        CHECK(adapter.ProcessRtcp(buildRtcpTwcc(twcc), now, feedback) && feedback.size() == 1);
        update = c.OnTransportPacketsFeedback(feedback[0]); CHECK(update.target_rate && update.pacer_config);
        CHECK(update.target_rate->target_rate == (index == 0 ? 960000 : 1920000));
        CHECK(pacer.setRates(update.pacer_config->data_rate(), update.pacer_config->pad_rate(), now));
        if (index == 1) CHECK(update.probe_cluster_configs.size() == 1 && update.probe_cluster_configs[0].target_data_rate == 3840000);
        CHECK(adapter.ProcessRtcp(buildRtcpTwcc(twcc), now + 1ms, feedback) && feedback.empty());
    }
    // Window is computed and smoothed even though pushback consumes it privately.
    CHECK(c.GetNetworkState(start + 1s).congestion_window.value_or(0) >= 3000);
    CHECK(adapter.GetOutstandingData() == 0);
}
void invalidInputs()
{
    auto missing = config(); missing.constraints.starting_rate.reset(); invalid([&] { GoogCcNetworkController c(missing); });
    GoogCcNetworkController c(config()); (void)initialize(c);
    invalid([&] { (void)c.OnSentPacket({}); });
    invalid([&] { (void)c.OnProcessInterval({Time{}, {}}); });
    StreamsConfig streams; streams.at_time = start; streams.pacing_factor = std::numeric_limits<double>::quiet_NaN();
    invalid([&] { (void)c.OnStreamsConfig(streams); });
    TransportPacketsFeedback bad; bad.feedback_time = start + 1s;
    bad.packet_feedbacks.push_back(packet(0, start, start)); bad.packet_feedbacks.push_back({});
    invalid([&] { (void)c.OnTransportPacketsFeedback(bad); });
    CHECK(c.GetNetworkState(start).pacer_config->data_rate() == 750000);
    CHECK(!c.OnProcessInterval({start + 25ms, {}}).has_updates());
}
struct ExternalEstimate final : NetworkStateEstimator
{
    int feedbacks{0}, routes{0};
    std::optional<NetworkStateEstimate> GetCurrentEstimate() override
    { NetworkStateEstimate e; e.link_capacity_upper = 500000; e.update_time = start; return e; }
    void OnTransportPacketsFeedback(const TransportPacketsFeedback&) override { ++feedbacks; }
    void OnRouteChange(const NetworkRouteChange&) override { ++routes; }
};
struct OverusePredictor final : NetworkStatePredictor
{
    int calls{0};
    BandwidthUsage Update(std::int64_t, std::int64_t, BandwidthUsage) override
    { ++calls; return BandwidthUsage::kBwOverusing; }
};
void optionalEstimatorsAndPredictor()
{
    auto cfg = config(); cfg.stream_based_config.pacing_factor = 4.0;
    GoogCcConfig g; g.limit_pacing_factor_by_upper_link_capacity = true;
    auto external = std::make_unique<ExternalEstimate>(); auto* observer = external.get();
    g.network_state_estimator = std::move(external);
    GoogCcNetworkController c(cfg, std::move(g)); (void)initialize(c);
    NetworkStateEstimate stale; stale.update_time = start - 1s; stale.link_capacity_upper = 5000000;
    (void)c.OnNetworkStateEstimate(stale); // Ignored in favor of the owned estimator; delivery may be delayed.
    TransportPacketsFeedback f; f.feedback_time = start + 1ms; f.packet_feedbacks.push_back(packet(0, start, start));
    auto u = c.OnTransportPacketsFeedback(f); CHECK(observer->feedbacks == 1 && u.pacer_config->data_rate() == 500000);
    NetworkRouteChange route; route.at_time = start + 2ms; route.constraints.at_time = route.at_time; route.constraints.starting_rate = 300000;
    (void)c.OnNetworkRouteChange(route); CHECK(observer->routes == 1);
    GoogCcConfig predicted; auto predictor = std::make_unique<OverusePredictor>(); auto* p = predictor.get();
    predicted.network_state_predictor = std::move(predictor);
    GoogCcNetworkController controlled(config(), std::move(predicted)); (void)initialize(controlled);
    u = controlled.OnTransportPacketsFeedback(f); CHECK(p->calls == 1 && u.target_rate->target_rate == 150000);
}
void upstreamReplay()
{
    // Generated by generate_goog_cc_reference.py using the original controller,
    // send-side arbitration, pushback and all original estimator algorithms.
    const std::uint64_t expected_hash[] = {
        1378587552771988330ULL, 7700459483302467502ULL, 6289193452909856819ULL, 4393053205431158521ULL,
        17729139175660307734ULL, 10501296998114455074ULL, 16832343443478573581ULL, 8793041826380858777ULL
    };
    const std::uint64_t expected_targets[] = {1416, 694, 1416, 1185, 1453, 1391, 1227, 1416};
    const std::uint64_t expected_probes[] = {4, 4, 4, 4, 5, 4, 10, 4};
    for (int scenario = 0; scenario < 8; ++scenario)
    {
        auto cfg = config(); GoogCcConfig goog;
        if (scenario == 1) goog.use_congestion_window = false;
        if (scenario == 2) goog.drop_frame_only = false;
        if (scenario == 3) goog.pushback.add_pacing = true;
        if (scenario == 4)
        { goog.safe_reset_on_route_change = goog.safe_reset_acknowledged_rate = true; cfg.stream_based_config.requests_alr_probing = true; }
        if (scenario == 5)
        { goog.use_min_allocatable_as_lower_bound = false; goog.limit_pacing_factor_by_upper_link_capacity = true; }
        if (scenario == 6) goog.bandwidth.loss.enabled = false;
        if (scenario == 7)
        {
            cfg.default_pacing_time_window = 0us; cfg.stream_based_config.pacing_factor = 4.0;
            cfg.stream_based_config.min_total_allocated_bitrate = 50000; cfg.stream_based_config.max_padding_rate = 100000;
        }
        GoogCcNetworkController controller(cfg, std::move(goog));
        const auto result = goog_cc_replay::run(scenario, controller);
        CHECK(result.hash == expected_hash[scenario]); CHECK(result.targets == expected_targets[scenario]);
        CHECK(result.probes == expected_probes[scenario]); CHECK(result.updates == (scenario == 6 ? 5496 : 5436));
    }
}
}
int main()
{
    try
    {
        startupAndRemoteLimits(); constraintsAndRoutes(); cwndAndRateWindows(); legacyLossAndRttBackoff(); twccProbeAndPacingPipeline(); invalidInputs(); optionalEstimatorsAndPredictor(); upstreamReplay();
        std::cout << "PASS: GoogCC startup, constraints, routes, RTT/RR, cwnd, TWCC probes, optional estimators and 28800 original-source packets\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
