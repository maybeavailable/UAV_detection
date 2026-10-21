// Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once
#include"webrtc/congestion_control/loss_based_bwe_v2.h"
#include"webrtc/congestion_control/bandwidth_usage.h"
#include<deque>
#include<utility>

namespace webrtc
{
// Arbitration of delay/loss/receiver limits, legacy RR fallback and RTT backoff.
// The owner serializes access. Configured rates are limited to 1 Gbit/s, matching
// the default source ceiling and this project's ALR/IntervalBudget rate domain.
class SendSideBandwidthEstimation
{
public:
    using Time = TransportTime;
    using Delta = std::chrono::microseconds;
    struct Config
    {
        LossBasedBweV2::Config loss;
        bool rtt_backoff_enabled{true};
        Delta rtt_limit{std::chrono::seconds(3)}, rtt_drop_interval{std::chrono::seconds(1)};
        double rtt_drop_fraction{0.8};
        std::uint64_t rtt_bandwidth_floor{5000};
        bool receiver_limit_caps_only{true};
        float low_loss_threshold{0.02f}, high_loss_threshold{0.1f};
        std::uint64_t bitrate_threshold{0};
    };
    SendSideBandwidthEstimation();
    explicit SendSideBandwidthEstimation(Config config);
    void OnRouteChange();
    std::uint64_t target_rate() const;
    LossBasedState loss_based_state() const { return loss_state_; }
    bool IsRttAboveLimit() const;
    std::uint8_t fraction_loss() const { return fraction_loss_; }
    Delta round_trip_time() const { return rtt_; }
    std::uint64_t GetMinBitrate() const { return min_bitrate_; }
    void UpdateEstimate(Time at_time);
    void OnSentPacket(const SentPacket& packet);
    void UpdatePropagationRtt(Time at_time, Delta rtt);
    void UpdateReceiverEstimate(Time at_time, std::optional<std::uint64_t> bandwidth);
    void UpdateDelayBasedEstimate(Time at_time, std::uint64_t bitrate);
    void UpdatePacketsLost(std::int64_t lost, std::uint64_t total, Time at_time);
    void UpdateRtt(Delta rtt, Time at_time);
    void SetBitrates(std::optional<std::uint64_t> start, std::uint64_t min,
        std::optional<std::uint64_t> max, Time at_time);
    void SetSendBitrate(std::uint64_t bitrate, Time at_time);
    void SetMinMaxBitrate(std::uint64_t min, std::optional<std::uint64_t> max);
    void SetAcknowledgedRate(std::optional<std::uint64_t> rate, Time at_time);
    void UpdateLossBasedEstimator(const TransportPacketsFeedback&, BandwidthUsage,
        std::optional<std::uint64_t> probe_bitrate, bool in_alr);
    static void ValidateRate(std::uint64_t rate);
private:
    bool InStartPhase(Time at_time) const;
    void UpdateTarget(std::uint64_t bitrate, Time at_time);
    void UpdateMinHistory(Time at_time);
    Config config_;
    std::unique_ptr<LossBasedBweV2> loss_;
    std::uint64_t target_{0}, min_bitrate_{5000}, max_bitrate_{1000000000};
    std::optional<std::uint64_t> receiver_limit_, delay_limit_;
    std::deque<std::pair<Time, std::uint64_t>> min_history_;
    std::int64_t accumulated_lost_{0};
    std::uint64_t accumulated_expected_{0};
    std::uint8_t fraction_loss_{0};
    bool decreased_since_loss_{false};
    Delta rtt_{0}, propagation_rtt_{0};
    std::optional<Time> last_propagation_update_, last_packet_sent_, last_decrease_, first_report_, last_loss_report_;
    LossBasedState loss_state_{LossBasedState::kDelayBasedEstimate};
};
} // namespace webrtc
