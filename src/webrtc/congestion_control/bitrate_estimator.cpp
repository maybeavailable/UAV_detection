/*
 * Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/bitrate_estimator.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/bitrate_estimator.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
std::uint64_t boundedRate(long double bps)
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return bps >= static_cast<long double>(max) ? max : static_cast<std::uint64_t>(bps);
}
bool validScale(double value)
{
    return std::isfinite(value) && value >= 0 && value <= std::numeric_limits<float>::max();
}
} // namespace

BitrateEstimator::BitrateEstimator() : BitrateEstimator(Config{}) {}
BitrateEstimator::BitrateEstimator(Config config) : config_(config)
{
    using Ms = std::chrono::milliseconds;
    if (config_.initial_window < Ms(150) || config_.initial_window > Ms(1000) ||
        config_.window < Ms(150) || config_.window > Ms(1000) ||
        !validScale(config_.uncertainty_scale) || !validScale(config_.uncertainty_scale_in_alr) ||
        !validScale(config_.small_sample_uncertainty_scale))
    {
        UAV_LOG_WARN << "[WebRTC][BitrateEstimator] invalid configuration initial_window_ms=" << config_.initial_window.count()
            << " window_ms=" << config_.window.count();
        throw std::invalid_argument("invalid bitrate estimator configuration");
    }
    UAV_LOG_INFO << "[WebRTC][BitrateEstimator][" << this << "] initialized initial_window_ms=" << config_.initial_window.count()
        << " window_ms=" << config_.window.count() << " floor_bps=" << config_.estimate_floor_bps;
}
BitrateEstimator::~BitrateEstimator() = default;

void BitrateEstimator::Update(Time at_time, std::size_t amount, bool in_alr)
{
    const auto window_ms = (bitrate_estimate_kbps_ < 0 ? config_.initial_window : config_.window).count();
    bool is_small_sample = false;
    // Timestamp::ms() rounds to nearest; duration_cast<milliseconds> truncates.
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(at_time.time_since_epoch()).count();
    const auto ms = us / 1000 + (us % 1000 >= 500) - (us % 1000 <= -500);
    const float sample = UpdateWindow(ms, amount, window_ms, &is_small_sample);
    UAV_LOG_IF(LogLevel::TRACE, LogManager::getInstance().shouldLog(LogLevel::TRACE)) << "[WebRTC][BitrateEstimator][" << this << "] receive_ms=" << ms << " bytes=" << amount
        << " accumulated_bytes=" << sum_ << " window_ms=" << current_window_ms_ << " in_alr=" << in_alr;
    if (sample < 0) return;
    if (bitrate_estimate_kbps_ < 0)
    {
        // The boundary packet belongs to the next window, not this first sample.
        bitrate_estimate_kbps_ = sample;
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateEstimator] initialized acknowledged_bps=" << *bitrate();
        return;
    }
    float scale = static_cast<float>(config_.uncertainty_scale);
    if (is_small_sample && sample < bitrate_estimate_kbps_)
        scale = static_cast<float>(config_.small_sample_uncertainty_scale);
    else if (in_alr && sample < bitrate_estimate_kbps_)
        scale = static_cast<float>(config_.uncertainty_scale_in_alr);

    const float denominator = bitrate_estimate_kbps_ + std::min(sample,
        static_cast<float>(config_.uncertainty_symmetry_cap_bps) * 0.001f);
    // A zero-byte initial sample is legal here. Avoid upstream's 0/0 and allow
    // recovery from zero throughput when positive samples resume.
    const float uncertainty = denominator > 0
        ? scale * std::abs(bitrate_estimate_kbps_ - sample) / denominator
        : (sample == bitrate_estimate_kbps_ ? 0.0f : scale);
    const float sample_var = uncertainty * uncertainty;
    const float predicted_var = bitrate_estimate_var_ + 5.0f;
    if (std::isfinite(sample_var + predicted_var) && std::isfinite(sample_var * predicted_var) &&
        std::isfinite(sample_var * bitrate_estimate_kbps_ + predicted_var * sample))
    {
        // Preserve upstream's float filter and process-noise/variance constants.
        bitrate_estimate_kbps_ = (sample_var * bitrate_estimate_kbps_ + predicted_var * sample) /
                               (sample_var + predicted_var);
        bitrate_estimate_var_ = sample_var * predicted_var / (sample_var + predicted_var);
    }
    else
    {
        // Wide intermediates only for values outside upstream's int-byte range.
        const double wide_uncertainty = denominator > 0
            ? double(scale) * std::abs(double(bitrate_estimate_kbps_) - sample) / denominator
            : scale;
        const double variance = wide_uncertainty * wide_uncertainty;
        const double gain = predicted_var / (variance + predicted_var);
        bitrate_estimate_kbps_ = static_cast<float>((1.0 - gain) * bitrate_estimate_kbps_ + gain * sample);
        bitrate_estimate_var_ = static_cast<float>((1.0 - gain) * predicted_var);
    }
    bitrate_estimate_kbps_ = std::max(bitrate_estimate_kbps_,
        static_cast<float>(config_.estimate_floor_bps) * 0.001f);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateEstimator][" << this << "] sample_kbps=" << sample << " acknowledged_bps=" << *bitrate()
        << " variance=" << bitrate_estimate_var_ << " small_sample=" << is_small_sample << " in_alr=" << in_alr;
}

float BitrateEstimator::UpdateWindow(std::int64_t now_ms, std::size_t bytes,
                                   std::int64_t rate_window_ms, bool* is_small_sample)
{
    if (prev_time_ms_ && now_ms < *prev_time_ms_)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateEstimator][" << this << "] backwards receive clock now_ms=" << now_ms
            << " previous_ms=" << *prev_time_ms_ << ", resetting sample window";
        prev_time_ms_.reset(); sum_ = 0; current_window_ms_ = 0;
    }
    if (prev_time_ms_)
    {
        const auto gap = now_ms - *prev_time_ms_;
        current_window_ms_ += gap;
        if (gap > rate_window_ms)
        {
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateEstimator][" << this << "] sample window gap_ms=" << gap
                << " window_ms=" << rate_window_ms << ", discarding stale bytes=" << sum_;
            sum_ = 0;
            current_window_ms_ %= rate_window_ms;
        }
    }
    prev_time_ms_ = now_ms;
    float sample = -1.0f;
    if (current_window_ms_ >= rate_window_ms)
    {
        *is_small_sample = sum_ < config_.small_sample_threshold;
        sample = 8.0f * static_cast<float>(sum_) / static_cast<float>(rate_window_ms);
        current_window_ms_ -= rate_window_ms;
        sum_ = 0;
    }
    // Unlike upstream's int sum, byte accounting cannot wrap on large inputs.
    sum_ += std::min(bytes, std::numeric_limits<std::size_t>::max() - sum_);
    return sample;
}

std::optional<std::uint64_t> BitrateEstimator::bitrate() const
{
    if (bitrate_estimate_kbps_ < 0) return std::nullopt;
    // DataRate::KilobitsPerSec(float) truncates the float product to integer bps.
    return boundedRate(bitrate_estimate_kbps_ * 1000.0f);
}
std::optional<std::uint64_t> BitrateEstimator::PeekRate() const
{
    if (current_window_ms_ <= 0) return std::nullopt;
    // Exact integer bps, avoiding overflow in sum * 8000. The residual window
    // is bounded by the configured (at most 1000 ms) sampling windows.
    const auto ms = static_cast<std::uint64_t>(current_window_ms_);
    const auto whole = sum_ / ms;
    const auto fractional = (sum_ % ms) * 8000 / ms;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (whole > (max - fractional) / 8000) return max;
    return whole * 8000 + fractional;
}
void BitrateEstimator::ExpectFastRateChange()
{
    bitrate_estimate_var_ += 200.0f;
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateEstimator] expecting fast rate change";
}
} // namespace webrtc
