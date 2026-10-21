// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/network_state_estimate.h"
#include"webrtc/congestion_control/probe_cluster_config.h"
#include<optional>
#include<vector>

namespace webrtc
{
struct ProbeControllerConfig
{
    double first_exponential_probe_scale{3.0};
    std::optional<double> second_exponential_probe_scale{6.0};
    double further_exponential_probe_scale{2.0}, further_probe_threshold{0.7};
    bool abort_further_probe_if_max_lower_than_current{false};
    std::chrono::microseconds repeated_initial_probing_time_period{std::chrono::seconds(5)};
    std::chrono::microseconds initial_probe_duration{std::chrono::milliseconds(100)};
    std::chrono::microseconds initial_min_probe_delta{std::chrono::milliseconds(20)};
    std::chrono::microseconds alr_probing_interval{std::chrono::seconds(5)};
    double alr_probe_scale{2.0};
    // nullopt replaces upstream +infinity (periodic probing disabled).
    std::optional<std::chrono::microseconds> network_state_estimate_probing_interval;
    double probe_if_estimate_lower_than_network_state_estimate_ratio{0.0};
    std::optional<std::chrono::microseconds> estimate_lower_than_network_state_estimate_probing_interval{std::chrono::seconds(3)};
    double network_state_probe_scale{1.0};
    std::chrono::microseconds network_state_probe_duration{std::chrono::milliseconds(15)};
    std::chrono::microseconds network_state_min_probe_delta{std::chrono::milliseconds(20)};
    bool probe_on_max_allocated_bitrate_change{true};
    bool probe_on_max_allocated_bitrate_change_without_alr{true};
    std::optional<double> first_allocation_probe_scale{1.0}, second_allocation_probe_scale{2.0};
    double allocation_probe_limit_by_current_scale{2.0};
    std::int32_t min_probe_packets_sent{5};
    std::chrono::microseconds min_probe_duration{std::chrono::milliseconds(15)};
    std::chrono::microseconds min_probe_delta{std::chrono::milliseconds(2)};
    double loss_limited_probe_scale{1.5};
    double skip_if_estimate_larger_than_fraction_of_max{0.0}, skip_probe_max_allocated_scale{1.0};
    bool rapid_recovery{false}; // WebRTC-BweRapidRecoveryExperiment
};

enum class BandwidthLimitedCause : int
{
    kLossLimitedBweIncreasing = 0,
    kLossLimitedBwe = 1,
    kDelayBasedLimited = 2,
    kDelayBasedLimitedDelayIncreased = 3,
    kRttBasedBackOffHighRtt = 4
};
struct NetworkAvailability
{
    std::chrono::steady_clock::time_point at_time{};
    bool network_available{false};
};

// Serialized, synchronous initiation policy, matching libwebrtc goog_cc.
// Produces requests; the owner schedules actual probes and supplies estimates.
class ProbeController
{
public:
    using Time = std::chrono::steady_clock::time_point;
    using Delta = std::chrono::microseconds;
    using Config = ProbeControllerConfig;
    enum class State { kInit, kWaitingForProbingResult, kProbingComplete };
    ProbeController();
    explicit ProbeController(Config config);
    ProbeController(const ProbeController&) = delete;
    ProbeController& operator=(const ProbeController&) = delete;

    [[nodiscard]] std::vector<ProbeClusterConfig> SetBitrates(std::uint64_t min_bitrate,
        std::uint64_t start_bitrate, std::optional<std::uint64_t> max_bitrate, Time at_time);
    [[nodiscard]] std::vector<ProbeClusterConfig> OnMaxTotalAllocatedBitrate(std::uint64_t bitrate, Time at_time);
    [[nodiscard]] std::vector<ProbeClusterConfig> OnNetworkAvailability(NetworkAvailability msg);
    [[nodiscard]] std::vector<ProbeClusterConfig> SetEstimatedBitrate(std::uint64_t bitrate, BandwidthLimitedCause cause, Time at_time);
    void EnablePeriodicAlrProbing(bool enable);
    void EnableRepeatedInitialProbing(bool enable);
    void SetAlrStartTime(std::optional<Time> alr_start_time);
    void SetAlrEndedTime(Time alr_end_time);
    [[nodiscard]] std::vector<ProbeClusterConfig> RequestProbe(Time at_time);
    void SetNetworkStateEstimate(NetworkStateEstimate estimate);
    void Reset(Time at_time);
    [[nodiscard]] std::vector<ProbeClusterConfig> Process(Time at_time);

    struct Stats
    {
        State state{State::kInit};
        bool network_available{false}, periodic_alr_probing{false}, repeated_initial_probing{false};
        std::uint64_t estimated_bitrate{0}, start_bitrate{0}, max_bitrate{0}, max_total_allocated_bitrate{0};
        std::optional<std::uint64_t> min_bitrate_to_probe_further;
        Time time_last_probing_initiated{};
        std::optional<Time> last_allowed_repeated_initial_probe;
        std::int64_t next_probe_cluster_id{1};
        BandwidthLimitedCause bandwidth_limited_cause{BandwidthLimitedCause::kDelayBasedLimited};
    };
    Stats Snapshot() const;

private:
    Delta ObserveTime(Time at_time);
    void UpdateState(State state);
    std::vector<ProbeClusterConfig> InitiateExponentialProbing(Delta at_time);
    std::vector<ProbeClusterConfig> InitiateProbing(Delta at_time, const std::vector<std::uint64_t>& rates, bool probe_further);
    ProbeClusterConfig CreateProbeClusterConfig(Delta at_time, std::uint64_t bitrate);
    bool TimeForAlrProbe(Delta at_time) const;
    bool TimeForNetworkStateProbe(Delta at_time) const;
    bool TimeForNextRepeatedInitialProbe(Delta at_time) const;

    const Config config_;
    bool network_available_{false}, repeated_initial_probing_enabled_{false}, enable_periodic_alr_probing_{false};
    std::optional<Delta> last_allowed_repeated_initial_probe_, last_event_time_;
    BandwidthLimitedCause bandwidth_limited_cause_{BandwidthLimitedCause::kDelayBasedLimited};
    State state_{State::kInit};
    std::optional<std::uint64_t> min_bitrate_to_probe_further_;
    Delta time_last_probing_initiated_{0}, last_bwe_drop_probing_time_{0}, time_of_last_large_drop_{0};
    std::uint64_t estimated_bitrate_{0}, start_bitrate_{0}, max_bitrate_{5000000};
    std::optional<NetworkStateEstimate> network_estimate_;
    std::optional<Delta> alr_start_time_, alr_end_time_;
    std::uint64_t bitrate_before_last_large_drop_{0}, max_total_allocated_bitrate_{0};
    std::int64_t next_probe_cluster_id_{1};
};
} // namespace webrtc
