/*
 * Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/alr_detector.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/alr_detector.h"
#include"log/log.h"

#include<cmath>
#include<stdexcept>
#include<utility>

namespace webrtc
{
AlrDetector::AlrDetector() : AlrDetector(Config{}) {}
AlrDetector::AlrDetector(Config config)
    : AlrDetector(config, [] { return std::chrono::steady_clock::now(); }) {}
AlrDetector::AlrDetector(Config config, Clock clock) : config_(config), clock_(std::move(clock))
{
    if (!clock_ || !std::isfinite(config_.bandwidth_usage_ratio) || config_.bandwidth_usage_ratio <= 0 ||
        config_.bandwidth_usage_ratio > 1 || !std::isfinite(config_.start_budget_level_ratio) ||
        !std::isfinite(config_.stop_budget_level_ratio) || config_.start_budget_level_ratio > 1 ||
        config_.stop_budget_level_ratio < -1 || config_.stop_budget_level_ratio >= config_.start_budget_level_ratio)
        throw std::invalid_argument("invalid ALR detector configuration or clock");
}
AlrDetector::~AlrDetector() = default;

void AlrDetector::OnBytesSent(std::size_t bytes_sent, Time send)
{
    const auto send_time = std::chrono::duration_cast<std::chrono::microseconds>(send.time_since_epoch());
    if (!last_send_time_)
    {
        last_send_time_ = send_time;
        return; // the first packet's sending duration is unknown
    }
    if (send_time < *last_send_time_)
        throw std::invalid_argument("ALR send time moved backwards");
    const auto elapsed_us = (send_time - *last_send_time_).count();
    // Match TimeDelta::ms(): round the duration, not the absolute timestamps.
    const auto elapsed_ms = elapsed_us / 1000 + (elapsed_us % 1000 >= 500);
    last_send_time_ = send_time;

    // Ordering matters after large sends: cap debt first, then earn credit.
    alr_budget_.useBudget(bytes_sent);
    alr_budget_.increaseBudget(std::chrono::milliseconds(elapsed_ms));
    const auto ratio = alr_budget_.budgetRatio();
    if (ratio > config_.start_budget_level_ratio && !alr_started_time_)
    {
        const auto now = clock_();
        alr_started_time_ = Time(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()));
        UAV_LOG_INFO << "[WebRTC][AlrDetector] entered budget_ratio=" << ratio;
    }
    else if (ratio < config_.stop_budget_level_ratio && alr_started_time_)
    {
        alr_started_time_.reset();
        UAV_LOG_INFO << "[WebRTC][AlrDetector] exited budget_ratio=" << ratio;
    }
}

void AlrDetector::SetEstimatedBitrate(std::uint64_t bitrate_bps)
{
    // DataRate * double rounds to integer bps, then kbps() rounds to nearest.
    // Only the ALR target is quantized; callers continue to use bit/s.
    const auto scaled_bps = std::round(static_cast<double>(bitrate_bps) * config_.bandwidth_usage_ratio);
    if (!bitrate_bps || !std::isfinite(scaled_bps) || scaled_bps > IntervalBudget::MaxRateBps + 499.0)
        throw std::invalid_argument("invalid ALR estimated bitrate");
    const auto target_bps = ((static_cast<std::uint64_t>(scaled_bps) + 500) / 1000) * 1000;
    alr_budget_.setTargetRate(target_bps);
}
std::optional<AlrDetector::Time> AlrDetector::GetApplicationLimitedRegionStartTime() const { return alr_started_time_; }
} // namespace webrtc
