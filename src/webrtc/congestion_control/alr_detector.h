// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/interval_budget.h"
#include<functional>
#include<optional>

namespace webrtc
{
// Detects application-limited sending from elapsed send time and bytes sent.
// Synchronous: the owning transport/controller must serialize access.
class AlrDetector
{
public:
    using Time = std::chrono::steady_clock::time_point;
    using Clock = std::function<Time()>;
    struct Config
    {
        // Effective upstream defaults with the default-on screenshare ALR trial.
        // The base values without that trial are 0.65, 0.80, 0.50 respectively.
        double bandwidth_usage_ratio{0.80};
        double start_budget_level_ratio{0.40};
        double stop_budget_level_ratio{-0.60};
    };

    AlrDetector();
    explicit AlrDetector(Config config);
    AlrDetector(Config config, Clock clock); // entry timestamp uses this local processing clock
    AlrDetector(const AlrDetector&) = delete;
    AlrDetector& operator=(const AlrDetector&) = delete;
    ~AlrDetector();

    // First packet only establishes a baseline. Times must not move backwards;
    // invalid input throws invalid_argument without consuming the packet.
    void OnBytesSent(std::size_t bytes_sent, Time send_time);
    // Positive bit/s; scaled/rounded budget rate must not exceed MaxRateBps.
    // An update preserves budget/ALR state; transitions happen on OnBytesSent.
    void SetEstimatedBitrate(std::uint64_t bitrate_bps);
    std::optional<Time> GetApplicationLimitedRegionStartTime() const;

private:
    const Config config_;
    const Clock clock_;
    std::optional<std::chrono::microseconds> last_send_time_;
    IntervalBudget alr_budget_{0, true, IntervalBudget::Rounding::TruncateBytes};
    std::optional<Time> alr_started_time_;
};
} // namespace webrtc
