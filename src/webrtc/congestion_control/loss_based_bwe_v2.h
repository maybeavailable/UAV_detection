// Copyright 2021 The WebRTC project authors. All rights reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/transport_adapter.h"

namespace webrtc
{
enum class LossBasedState
{
    kIncreasing = 0,
    kIncreaseUsingPadding = 1,
    kDecreasing = 2,
    kDelayBasedEstimate = 3
};

// Serialized sender-side loss model from libwebrtc goog_cc. All public rates
// use bit/s; nullopt represents an unknown/unbounded rate, distinct from zero.
class LossBasedBweV2
{
public:
    struct Config
    {
        // Defaults are from CreateConfig(), not upstream's zero-filled struct.
        bool enabled{true};
        double bandwidth_rampup_upper_bound_factor{1.5};
        double bandwidth_rampup_upper_bound_factor_in_hold{1.2};
        double bandwidth_rampup_hold_threshold{1.3};
        double rampup_acceleration_max_factor{0.0};
        std::chrono::microseconds rampup_acceleration_maxout_time{std::chrono::seconds(60)};
        std::vector<double> candidate_factors{1.02, 1.0, 0.95};
        double higher_bandwidth_bias_factor{0.0002}, higher_log_bandwidth_bias_factor{0.02};
        double inherent_loss_lower_bound{0.001};
        double loss_threshold_of_high_bandwidth_preference{0.2};
        double bandwidth_preference_smoothing_factor{0.002};
        std::uint64_t inherent_loss_upper_bound_bandwidth_balance{100000};
        double inherent_loss_upper_bound_offset{0.05}, initial_inherent_loss_estimate{0.01};
        int newton_iterations{1};
        double newton_step_size{0.75};
        bool append_acknowledged_rate_candidate{true}, append_delay_based_estimate_candidate{true};
        bool append_upper_bound_candidate_in_alr{false};
        std::chrono::microseconds observation_duration_lower_bound{std::chrono::milliseconds(250)};
        int observation_window_size{15};
        double sending_rate_smoothing_factor{0.0};
        double instant_upper_bound_temporal_weight_factor{0.9};
        std::uint64_t instant_upper_bound_bandwidth_balance{100000};
        double instant_upper_bound_loss_offset{0.05}, temporal_weight_factor{0.9};
        double bandwidth_backoff_lower_bound_factor{1.0}, max_increase_factor{1.3};
        std::chrono::microseconds delayed_increase_window{std::chrono::milliseconds(300)};
        bool not_increase_if_inherent_loss_less_than_average_loss{true};
        bool not_use_acked_rate_in_alr{true}, use_in_start_phase{true};
        int min_num_observations{3};
        double lower_bound_by_acked_rate_factor{1.0}, hold_duration_factor{2.0};
        bool use_byte_loss_rate{true};
        std::chrono::microseconds padding_duration{std::chrono::seconds(2)};
        bool bound_best_candidate{true};
        double median_sending_rate_factor{2.0};
    };
    struct Result
    {
        std::optional<std::uint64_t> bandwidth_estimate;
        LossBasedState state{LossBasedState::kDelayBasedEstimate};
    };

    LossBasedBweV2();
    // Disabled or invalid configurations produce a disabled estimator.
    explicit LossBasedBweV2(Config config);
    ~LossBasedBweV2();
    LossBasedBweV2(const LossBasedBweV2&) = delete;
    LossBasedBweV2& operator=(const LossBasedBweV2&) = delete;

    bool IsEnabled() const;
    bool IsReady() const;
    bool ReadyToUseInStartPhase() const;
    bool UseInStartPhase() const;
    Result GetLossBasedResult() const;
    void SetAcknowledgedBitrate(std::uint64_t acknowledged_bitrate_bps);
    void SetMinMaxBitrate(std::uint64_t min_bitrate_bps, std::optional<std::uint64_t> max_bitrate_bps);
    void SetBandwidthEstimate(std::uint64_t bandwidth_estimate_bps);
    // Pass ALL feedback records with send history, including losses. Passing
    // SortedByReceiveTime() would discard losses and invalidate this model.
    // Rebuild on route/generation changes. No automatic rate or padding changes.
    void UpdateBandwidthEstimate(const std::vector<PacketResult>& packet_results,
        std::optional<std::uint64_t> delay_based_estimate_bps, bool in_alr);

    struct Stats
    {
        std::uint64_t observations{0}, partial_packets{0}, pending_lost_packets{0};
        double average_loss_ratio{0.0}, inherent_loss{0.0};
        std::optional<std::uint64_t> model_bandwidth, sending_rate, instant_lower_bound, instant_upper_bound;
        std::chrono::microseconds hold_duration{0};
    };
    Stats Snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace webrtc
