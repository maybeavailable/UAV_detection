// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// Adapted regression scenarios; see third_party/webrtc/LICENSE and NOTICE.
#include"webrtc/congestion_control/probe_controller.h"
#include"webrtc/congestion_control/alr_detector.h"
#include"webrtc/transport_adapter.h"
#include"reference/probe_controller_replay.h"
#include<cmath>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = ProbeController::Time;
using State = ProbeController::State;
using Cause = BandwidthLimitedCause;
void completeInitial(ProbeController& e, Time now, std::uint64_t start = 300, std::uint64_t max = 10000)
{
    CHECK(e.OnNetworkAvailability({now, true}).empty());
    CHECK(!e.SetBitrates(100, start, max, now).empty());
    CHECK(e.Process(now + 1s + 1us).empty());
    CHECK(e.Snapshot().state == State::kProbingComplete);
}
void initialAndExponential()
{
    ProbeController e; const Time now(100s);
    CHECK(e.SetBitrates(100, 300, 10000, now).empty());
    CHECK(e.Process(now).empty());
    auto probes = e.OnNetworkAvailability({now, true});
    CHECK(probes.size() == 2 && probes[0].target_data_rate == 900 && probes[1].target_data_rate == 1800);
    CHECK(probes[0].at_time == now && probes[0].target_duration == 15ms && probes[0].min_probe_delta == 2ms);
    CHECK(probes[0].target_probe_count == 5 && probes[0].id == 1 && probes[1].id == 2);
    CHECK(e.Snapshot().state == State::kWaitingForProbingResult && e.Snapshot().min_bitrate_to_probe_further == 1260);
    CHECK(e.SetEstimatedBitrate(1260, Cause::kDelayBasedLimited, now + 1ms).empty());
    probes = e.SetEstimatedBitrate(1261, Cause::kDelayBasedLimited, now + 2ms);
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 2522 && probes[0].id == 3);
    CHECK(e.Snapshot().min_bitrate_to_probe_further == 1765);
    probes = e.SetEstimatedBitrate(6000, Cause::kDelayBasedLimited, now + 3ms);
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 10000 && e.Snapshot().state == State::kProbingComplete);
    CHECK(e.SetEstimatedBitrate(10000, Cause::kDelayBasedLimited, now + 4ms).empty());
    probes = e.SetBitrates(100, 0, 20000, now + 5ms);
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 20000 && probes[0].id == 5);

    ProbeController fallback;
    CHECK(fallback.OnNetworkAvailability({Time(0s), true}).empty());
    probes = fallback.SetBitrates(100, 0, std::nullopt, Time(0s));
    CHECK(probes.size() == 2 && probes[0].target_data_rate == 300 && probes[1].target_data_rate == 600);
    CHECK(fallback.Snapshot().start_bitrate == 100 && fallback.Snapshot().estimated_bitrate == 0 && fallback.Snapshot().max_bitrate == 5000000);
    ProbeController limit; CHECK(limit.OnNetworkAvailability({Time(0s), true}).empty());
    probes = limit.SetBitrates(100, 1000000, std::nullopt, Time(0s));
    CHECK(probes.size() == 2 && probes[0].target_data_rate == 3000000 && probes[1].target_data_rate == 5000000);
    CHECK(limit.Snapshot().state == State::kProbingComplete);
    for (const auto second : {std::optional<double>{}, std::optional<double>{0.0}})
    {
        ProbeController::Config config; config.first_exponential_probe_scale = 2.5; config.second_exponential_probe_scale = second;
        config.min_probe_duration = 123ms; config.min_probe_delta = 5ms; config.min_probe_packets_sent = 7;
        ProbeController one(config); CHECK(one.OnNetworkAvailability({Time(0s), true}).empty());
        probes = one.SetBitrates(100, 300, 10000, Time(0s));
        CHECK(probes.size() == 1 && probes[0].target_data_rate == 750 && probes[0].target_duration == 123ms);
        CHECK(probes[0].min_probe_delta == 5ms && probes[0].target_probe_count == 7);
    }
}
void allocationAndReset()
{
    ProbeController e; e.EnablePeriodicAlrProbing(true); e.SetAlrStartTime(Time(0s));
    completeInitial(e, Time(0s));
    e.EnableRepeatedInitialProbing(true);
    auto probes = e.OnMaxTotalAllocatedBitrate(500, Time(2s));
    CHECK(probes.size() == 2 && probes[0].target_data_rate == 500 && probes[1].target_data_rate == 600);
    CHECK(e.Snapshot().state == State::kWaitingForProbingResult);
    probes = e.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(2s));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 1000 && e.Snapshot().state == State::kProbingComplete);
    CHECK(e.OnMaxTotalAllocatedBitrate(400, Time(2s)).empty());
    const auto next_id = e.Snapshot().next_probe_cluster_id;
    e.Reset(Time(3s));
    auto snapshot = e.Snapshot();
    CHECK(snapshot.state == State::kInit && snapshot.network_available && snapshot.periodic_alr_probing && snapshot.repeated_initial_probing);
    CHECK(snapshot.estimated_bitrate == 0 && snapshot.start_bitrate == 0 && snapshot.max_bitrate == 5000000);
    CHECK(snapshot.max_total_allocated_bitrate == 400 && snapshot.next_probe_cluster_id == next_id);
    CHECK(e.Process(Time(3s)).empty());
    probes = e.SetBitrates(100, 300, 10000, Time(3s));
    CHECK(probes.size() == 2 && probes[0].target_data_rate == 800 && probes[1].target_data_rate == 800);
    CHECK(probes[0].id == next_id && probes[1].id == next_id + 1); // Capped duplicate requests are retained.

    ProbeController::Config config; config.probe_on_max_allocated_bitrate_change_without_alr = false;
    ProbeController alr_only(config); completeInitial(alr_only, Time(0s));
    CHECK(alr_only.OnMaxTotalAllocatedBitrate(500, Time(2s)).empty());
    alr_only.SetAlrStartTime(Time(1s)); probes = alr_only.OnMaxTotalAllocatedBitrate(750, Time(2s));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 600);
    config.first_allocation_probe_scale.reset();
    ProbeController no_allocation(config); completeInitial(no_allocation, Time(0s)); no_allocation.SetAlrStartTime(Time(0s));
    CHECK(no_allocation.OnMaxTotalAllocatedBitrate(1000, Time(2s)).empty());
    CHECK(no_allocation.Snapshot().max_total_allocated_bitrate == 1000);
}
void timersAndRepeatedStartup()
{
    ProbeController e; e.EnableRepeatedInitialProbing(true);
    CHECK(e.OnNetworkAvailability({Time(0s), true}).empty());
    auto probes = e.SetBitrates(100, 300, 10000, Time(0s));
    CHECK(probes.size() == 2 && probes[0].target_duration == 100ms && probes[0].min_probe_delta == 20ms);
    CHECK(e.Process(Time(1s)).empty() && e.Snapshot().state == State::kWaitingForProbingResult);
    probes = e.Process(Time(1s + 1us));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 900 && probes[0].target_duration == 100ms);
    probes = e.Process(Time(2s + 2us)); CHECK(probes.size() == 1 && probes[0].min_probe_delta == 20ms);
    CHECK(e.Process(Time(5s)).empty() && e.Snapshot().state == State::kProbingComplete);

    ProbeController stopped; stopped.EnableRepeatedInitialProbing(true);
    CHECK(stopped.OnNetworkAvailability({Time(0s), true}).empty()); CHECK(stopped.SetBitrates(100, 300, 10000, Time(0s)).size() == 2);
    CHECK(stopped.OnMaxTotalAllocatedBitrate(100, Time(500ms)).empty());
    CHECK(stopped.Process(Time(1100ms)).empty());

    ProbeController periodic; periodic.EnablePeriodicAlrProbing(true); periodic.SetAlrStartTime(Time(100s));
    CHECK(periodic.OnNetworkAvailability({Time(100s), true}).empty());
    CHECK(periodic.SetBitrates(100, 300, 10000, Time(100s)).size() == 2);
    CHECK(periodic.Process(Time(105s - 1us)).empty());
    probes = periodic.Process(Time(105s)); CHECK(probes.size() == 1 && probes[0].target_data_rate == 600);
    probes = periodic.Process(Time(110s)); CHECK(probes.size() == 1 && probes[0].target_data_rate == 600);
    periodic.SetAlrStartTime(std::nullopt); CHECK(periodic.Process(Time(115s)).empty());
    periodic.Reset(Time(115s)); CHECK(periodic.OnNetworkAvailability({Time(115s), false}).empty());
    CHECK(periodic.SetBitrates(100, 300, 10000, Time(115s)).empty()); CHECK(periodic.Process(Time(125s)).empty());
}
void networkStateAndSkip()
{
    ProbeController::Config config; config.network_state_estimate_probing_interval = 5s;
    config.probe_if_estimate_lower_than_network_state_estimate_ratio = .7;
    config.network_state_probe_duration = 40ms; config.network_state_min_probe_delta = 7ms;
    ProbeController e(config); CHECK(e.OnNetworkAvailability({Time(100s), true}).empty());
    CHECK(e.SetBitrates(100, 300, 10000, Time(100s)).size() == 2);
    e.SetNetworkStateEstimate({std::nullopt, 1000}); CHECK(e.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(100s)).empty());
    CHECK(e.Process(Time(103s - 1us)).empty());
    auto probes = e.Process(Time(103s));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 1000 && probes[0].target_duration == 40ms && probes[0].min_probe_delta == 7ms);
    CHECK(e.SetEstimatedBitrate(800, Cause::kDelayBasedLimited, Time(103s)).empty());
    CHECK(e.Process(Time(106s)).empty()); CHECK(e.Process(Time(108s)).size() == 1);
    e.SetNetworkStateEstimate({std::nullopt, 0}); e.SetAlrStartTime(Time(108s)); e.EnablePeriodicAlrProbing(true);
    CHECK(e.Process(Time(113s)).empty());

    ProbeController further(config); CHECK(further.OnNetworkAvailability({Time(0s), true}).empty());
    CHECK(further.SetBitrates(100, 300, 10000, Time(0s)).size() == 2);
    further.SetNetworkStateEstimate({std::nullopt, 2000});
    CHECK(further.SetEstimatedBitrate(1500, Cause::kDelayBasedLimited, Time(0s)).empty());
    probes = further.SetEstimatedBitrate(1300, Cause::kDelayBasedLimited, Time(1ms));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 2000);

    config.network_state_estimate_probing_interval.reset();
    ProbeController ratio_only(config); CHECK(ratio_only.OnNetworkAvailability({Time(0s), true}).empty());
    CHECK(ratio_only.SetBitrates(100, 300, 10000, Time(0s)).size() == 2);
    ratio_only.SetNetworkStateEstimate({std::nullopt, 1200}); CHECK(ratio_only.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(0s)).empty());
    probes = ratio_only.Process(Time(3s));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 1000 && probes[0].target_duration == 15ms && probes[0].min_probe_delta == 2ms);

    config = {}; config.skip_if_estimate_larger_than_fraction_of_max = .9;
    ProbeController skip(config); CHECK(skip.OnNetworkAvailability({Time(0s), true}).empty());
    CHECK(skip.SetBitrates(100, 9001, 10000, Time(0s)).empty()); CHECK(skip.Snapshot().state == State::kProbingComplete);
    ProbeController boundary(config); CHECK(boundary.OnNetworkAvailability({Time(0s), true}).empty());
    CHECK(boundary.SetBitrates(100, 9000, 10000, Time(0s)).size() == 2);
    ProbeController allocation_skip(config); allocation_skip.EnablePeriodicAlrProbing(true); allocation_skip.SetAlrStartTime(Time(0s));
    CHECK(allocation_skip.OnNetworkAvailability({Time(0s), true}).empty()); CHECK(allocation_skip.SetBitrates(100, 950, 10000, Time(0s)).size() == 2);
    CHECK(allocation_skip.OnMaxTotalAllocatedBitrate(1000, Time(500ms)).empty()); CHECK(allocation_skip.Process(Time(5s)).empty());
    allocation_skip.SetNetworkStateEstimate({std::nullopt, 800}); probes = allocation_skip.Process(Time(5s));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 1900);
}
void causesAndRecovery()
{
    for (int value = 0; value <= 4; ++value)
    {
        ProbeController e; completeInitial(e, Time(0s)); e.EnablePeriodicAlrProbing(true); e.SetAlrStartTime(Time(0s));
        CHECK(e.SetEstimatedBitrate(300, static_cast<Cause>(value), Time(2s)).empty());
        auto probes = e.Process(Time(5s));
        if (value == 0) CHECK(probes.size() == 1 && probes[0].target_data_rate == 450);
        else if (value == 2) CHECK(probes.size() == 1 && probes[0].target_data_rate == 600);
        else CHECK(probes.empty());
    }
    ProbeController e; completeInitial(e, Time(100s), 1000); e.SetAlrStartTime(Time(101s));
    CHECK(e.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(107s)).empty());
    auto probes = e.RequestProbe(Time(107s)); CHECK(probes.size() == 1 && probes[0].target_data_rate == 850);
    CHECK(e.SetEstimatedBitrate(1000, Cause::kDelayBasedLimited, Time(108s)).empty());
    CHECK(e.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(112s)).empty());
    CHECK(e.RequestProbe(Time(112s)).empty()); CHECK(e.RequestProbe(Time(112s + 1us)).size() == 1);

    for (bool rapid : {false, true})
    {
        ProbeController::Config config; config.rapid_recovery = rapid;
        ProbeController recovery(config); completeInitial(recovery, Time(100s), 1000);
        CHECK(recovery.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(107s)).empty());
        CHECK(recovery.RequestProbe(Time(107s)).empty() == !rapid);
    }
    ProbeController expired; completeInitial(expired, Time(100s), 1000); expired.SetAlrStartTime(Time(100s));
    CHECK(expired.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(107s)).empty());
    CHECK(expired.RequestProbe(Time(112s)).empty()); // Exactly 5 seconds since the drop.
    ProbeController ended; completeInitial(ended, Time(100s), 1000); ended.SetAlrEndedTime(Time(104s));
    CHECK(ended.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(106s)).empty());
    CHECK(ended.RequestProbe(Time(107s)).empty()); // Exactly 3 seconds after ALR ended.
    ProbeController recent; completeInitial(recent, Time(100s), 1000); recent.SetAlrEndedTime(Time(104s));
    CHECK(recent.SetEstimatedBitrate(500, Cause::kDelayBasedLimited, Time(106s)).empty());
    CHECK(recent.RequestProbe(Time(107s - 1us)).size() == 1);
}
void inputAndNumericBoundaries()
{
    for (int i = 0; i < 8; ++i)
    {
        ProbeController::Config c;
        if (i == 0) c.first_exponential_probe_scale = 0;
        if (i == 1) c.second_exponential_probe_scale = std::numeric_limits<double>::quiet_NaN();
        if (i == 2) c.further_exponential_probe_scale = std::numeric_limits<double>::infinity();
        if (i == 3) c.min_probe_packets_sent = 0;
        if (i == 4) c.min_probe_delta = 0us;
        if (i == 5) c.repeated_initial_probing_time_period = -1us;
        if (i == 6) c.network_state_estimate_probing_interval = -1us;
        if (i == 7) c.first_allocation_probe_scale = -1;
        bool caught = false; try { ProbeController invalid(c); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught);
    }
    ProbeController e; CHECK(e.OnNetworkAvailability({Time(1s), true}).empty()); CHECK(e.SetBitrates(0, 0, 10000, Time(1s)).empty());
    bool caught = false; try { (void)e.SetBitrates(100, 300, 0, Time(2s)); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught);
    CHECK(e.SetBitrates(100, 300, 10000, Time(1s)).size() == 2); // Invalid max did not consume time.
    const auto before = e.Snapshot(); caught = false;
    try { (void)e.SetEstimatedBitrate(500, static_cast<Cause>(99), Time(2s)); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught);
    CHECK(e.Snapshot().estimated_bitrate == before.estimated_bitrate);
    caught = false; try { (void)e.Process(Time(999ms)); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught);
    caught = false; try { e.SetAlrStartTime(Time(-1ns)); } catch (const std::invalid_argument&) { caught = true; } CHECK(caught);

    ProbeController wide; CHECK(wide.OnNetworkAvailability({Time(0s), true}).empty());
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto probes = wide.SetBitrates(100, maximum, maximum, Time(0s));
    CHECK(probes.size() == 2 && probes[0].target_data_rate == maximum && probes[1].target_data_rate == maximum);
    ProbeController large; CHECK(large.OnNetworkAvailability({Time(0s), true}).empty());
    CHECK(large.SetBitrates(100, 10000000, 100000000, Time(0s)).size() == 2);
    probes = large.SetEstimatedBitrate(60000000, Cause::kDelayBasedLimited, Time(0s));
    CHECK(probes.size() == 1 && probes[0].target_data_rate == 100000000);
    CHECK(large.SetEstimatedBitrate(100000000, Cause::kDelayBasedLimited, Time(0s)).empty());
    ProbeController::Config c; c.first_exponential_probe_scale = .1; c.second_exponential_probe_scale.reset();
    ProbeController tiny(c); CHECK(tiny.OnNetworkAvailability({Time(0s), true}).empty()); CHECK(tiny.SetBitrates(1, 1, 10, Time(0s)).empty());
    c = {}; c.repeated_initial_probing_time_period = std::chrono::microseconds::max();
    ProbeController timers(c); timers.EnableRepeatedInitialProbing(true); CHECK(timers.OnNetworkAvailability({Time(1s), true}).empty());
    CHECK(timers.SetBitrates(100, 300, 10000, Time(1s)).size() == 2);
    CHECK(timers.Snapshot().last_allowed_repeated_initial_probe == Time::max());
    CHECK(timers.Process(Time::max()).size() == 1);
}
void upstreamReplay()
{
    constexpr std::uint64_t golden[] = {8828995621977630777ULL, 3078169456247659333ULL, 18024461804112217325ULL, 18139818698315518949ULL,
        13471420074831913704ULL, 10579508075575052246ULL, 2098624714135267025ULL, 2699238435273900772ULL};
    constexpr int cluster_counts[] = {87, 86, 99, 78, 100, 76, 92, 69};
    constexpr int state_counts[8][3] = {{30, 328, 1142}, {30, 339, 1131}, {30, 447, 1023}, {30, 340, 1130},
        {30, 408, 1062}, {30, 229, 1241}, {30, 330, 1140}, {30, 282, 1188}};
    for (int s = 0; s < 8; ++s)
    {
        ProbeController::Config config; config.rapid_recovery = probe_replay::rapidRecovery(s);
        probe_replay::configure(s, config, [](std::int64_t us) { return std::chrono::microseconds(us); }, [] { return std::nullopt; });
        ProbeController e(config); probe_replay::Input input(s); probe_replay::Hash hash; int states[3]{}, clusters = 0;
        for (int i = 0; i < 1500; ++i)
        {
            const auto ev = input.event(i, s); const Time now(std::chrono::microseconds(ev.at_us));
            std::vector<ProbeClusterConfig> probes; using K = probe_replay::Kind;
            switch (ev.kind)
            {
            case K::Reset: e.Reset(now); break;
            case K::Availability: probes = e.OnNetworkAvailability({now, ev.flag}); break;
            case K::Bitrates: probes = e.SetBitrates(10000, ev.rate, ev.maximum ? std::optional<std::uint64_t>(ev.maximum) : std::nullopt, now); break;
            case K::Allocation: probes = e.OnMaxTotalAllocatedBitrate(ev.rate, now); break;
            case K::Estimate: probes = e.SetEstimatedBitrate(ev.rate, static_cast<Cause>(ev.cause), now); break;
            case K::Periodic: e.EnablePeriodicAlrProbing(ev.flag); break;
            case K::Repeated: e.EnableRepeatedInitialProbing(ev.flag); break;
            case K::AlrStart: e.SetAlrStartTime(ev.flag ? std::optional<Time>(now - 500ms) : std::nullopt); break;
            case K::AlrEnd: e.SetAlrEndedTime(now); break;
            case K::Request: probes = e.RequestProbe(now); break;
            case K::NetworkEstimate: e.SetNetworkStateEstimate({std::nullopt, ev.upper}); break;
            case K::Process: probes = e.Process(now); break;
            }
            hash.mix(probes.size()); clusters += probes.size();
            for (const auto& p : probes)
            {
                hash.mix(std::chrono::duration_cast<std::chrono::microseconds>(p.at_time.time_since_epoch()).count());
                hash.mix(p.target_data_rate); hash.mix(p.target_duration.count()); hash.mix(p.min_probe_delta.count()); hash.mix(p.target_probe_count); hash.mix(p.id);
            }
            const auto stats = e.Snapshot(); ++states[static_cast<int>(stats.state)];
            hash.mix(static_cast<int>(stats.state)); hash.mix(stats.network_available); hash.mix(stats.periodic_alr_probing); hash.mix(stats.repeated_initial_probing);
            hash.mix(stats.estimated_bitrate); hash.mix(stats.start_bitrate); hash.mix(stats.max_bitrate); hash.mix(stats.max_total_allocated_bitrate);
            hash.optional(stats.min_bitrate_to_probe_further);
            hash.mix(std::chrono::duration_cast<std::chrono::microseconds>(stats.time_last_probing_initiated.time_since_epoch()).count());
            std::optional<std::uint64_t> last_allowed;
            if (stats.last_allowed_repeated_initial_probe) last_allowed = std::chrono::duration_cast<std::chrono::microseconds>(stats.last_allowed_repeated_initial_probe->time_since_epoch()).count();
            hash.optional(last_allowed); hash.mix(stats.next_probe_cluster_id); hash.mix(static_cast<int>(stats.bandwidth_limited_cause));
        }
        if (hash.value != golden[s]) std::cerr << "scenario " << s << " actual=" << hash.value << " expected=" << golden[s] << '\n';
        CHECK(hash.value == golden[s] && clusters == cluster_counts[s]);
        for (int i = 0; i < 3; ++i) CHECK(states[i] == state_counts[s][i]);
    }
}
void alrAndTransportMetadata()
{
    Time now(100s); AlrDetector alr({}, [&now] { return now; }); alr.SetEstimatedBitrate(300000);
    ProbeController e; e.EnablePeriodicAlrProbing(true);
    CHECK(e.OnNetworkAvailability({now, true}).empty());
    CHECK(e.SetBitrates(30000, 300000, 2000000, now).size() == 2);
    alr.OnBytesSent(0, now); now += 1s; alr.OnBytesSent(0, now);
    CHECK(alr.GetApplicationLimitedRegionStartTime() == now); e.SetAlrStartTime(alr.GetApplicationLimitedRegionStartTime());
    now += 5s; const auto probes = e.Process(now); CHECK(probes.size() == 1 && probes[0].target_data_rate == 600000);
    const auto& cluster = probes[0];
    // Mirrors the cluster fields carried by the future probing/pacing owner.
    // Source DataRate * duration rounds to nearest byte: 600k * 15ms / 8 = 1125.
    PacedPacketInfo info; info.probe_cluster_id = cluster.id; info.probe_cluster_min_probes = cluster.target_probe_count;
    info.probe_cluster_min_bytes = 1125; info.send_bitrate_bps = cluster.target_data_rate;
    TransportFeedbackAdapter adapter;
    CHECK(adapter.AddPacket({7, 1200, 42, 123, false, false}, info, 28, now));
    CHECK(adapter.ProcessSentPacket({7, now, {true, true, 0}}));
    RtcpTwcc twcc; twcc.baseSequence = 7; twcc.referenceTime = 100; twcc.deltas = {0};
    std::vector<TransportPacketsFeedback> feedback;
    CHECK(adapter.ProcessRtcp(buildRtcpTwcc(twcc), now + 100ms, feedback) && feedback.size() == 1);
    const auto& recorded = feedback[0].packet_feedbacks[0].sent_packet.pacing_info;
    CHECK(recorded.probe_cluster_id == cluster.id && recorded.probe_cluster_min_probes == 5 && recorded.probe_cluster_min_bytes == 1125);
    CHECK(recorded.send_bitrate_bps == 600000 && adapter.GetOutstandingData() == 0);
    CHECK(e.SetEstimatedBitrate(150000, Cause::kLossLimitedBwe, now + 1s).empty());
    CHECK(e.Process(now + 6s).empty());
    CHECK(e.SetEstimatedBitrate(200000, Cause::kLossLimitedBweIncreasing, now + 6s).empty());
    const auto limited = e.Process(now + 6s); CHECK(limited.size() == 1 && limited[0].target_data_rate == 300000);
}
} // namespace
int main()
{
    try
    {
        initialAndExponential(); allocationAndReset(); timersAndRepeatedStartup(); networkStateAndSkip(); causesAndRecovery();
        inputAndNumericBoundaries(); upstreamReplay(); alrAndTransportMetadata();
        std::cout << "PASS: initial/exponential/allocation/ALR/recovery probes, limits, 12000 upstream events and ALR/TWCC metadata\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
