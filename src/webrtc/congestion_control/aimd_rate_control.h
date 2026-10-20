// Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/bandwidth_usage.h"
#include"webrtc/congestion_control/link_capacity_estimator.h"
#include"webrtc/congestion_control/network_state_estimate.h"
#include<chrono>

namespace webrtc
{
struct RateControlInput
{
    BandwidthUsage bw_state{BandwidthUsage::kBwNormal};
    std::optional<std::uint64_t> estimated_throughput;
};

// Serialized, synchronous libwebrtc AIMD controller. All rates use bit/s.
class AimdRateControl
{
public:
    using Time = std::chrono::steady_clock::time_point;
    using Delta = std::chrono::microseconds;
    struct Config
    {
        double backoff_factor{0.85};
        bool send_side{false};
        bool no_bitrate_increase_in_alr{false};
        bool disable_estimate_bounded_increase{false};
        bool use_current_estimate_as_min_upper_bound{true};
    };
    AimdRateControl();
    explicit AimdRateControl(Config config);
    AimdRateControl(const AimdRateControl&) = delete;
    AimdRateControl& operator=(const AimdRateControl&) = delete;

    bool ValidEstimate() const;
    void SetStartBitrate(std::uint64_t start_bitrate_bps);
    void SetMinBitrate(std::uint64_t min_bitrate_bps);
    Delta GetFeedbackInterval() const;
    bool TimeToReduceFurther(Time at_time, std::uint64_t estimated_throughput_bps) const;
    bool InitialTimeToReduceFurther(Time at_time) const;
    std::uint64_t LatestEstimate() const;
    void SetRtt(Delta rtt);
    std::uint64_t Update(const RateControlInput& input, Time at_time);
    void SetInApplicationLimitedRegion(bool in_alr);
    void SetEstimate(std::uint64_t bitrate_bps, Time at_time);
    void SetNetworkStateEstimate(std::optional<NetworkStateEstimate> estimate);
    double GetNearMaxIncreaseRateBpsPerSecond() const;

private:
    enum class State { Hold, Increase, Decrease };
    void ValidateTime(Time at_time) const;
    void ChangeBitrate(const RateControlInput& input, Delta at_time);
    std::uint64_t ClampBitrate(std::uint64_t bitrate_bps) const;

    const Config config_;
    std::uint64_t min_bitrate_{5000};
    // The upstream 30 Mbit/s value is an initial placeholder, not a ceiling.
    std::uint64_t current_bitrate_{30000000}, latest_throughput_{30000000};
    LinkCapacityEstimator link_capacity_;
    std::optional<NetworkStateEstimate> network_estimate_;
    State state_{State::Hold};
    std::optional<Delta> last_bitrate_change_, first_throughput_time_, last_update_time_;
    bool initialized_{false}, in_alr_{false};
    Delta rtt_{std::chrono::milliseconds(200)};
};
} // namespace webrtc
