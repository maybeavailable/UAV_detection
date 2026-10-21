/*
 * Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/trendline_estimator.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/trendline_estimator.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<limits>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace
{
constexpr double kSmoothingCoeff = 0.9, kThresholdGain = 4.0;
constexpr double kMaxAdaptOffsetMs = 15.0, kOverUsingTimeThreshold = 10.0;
constexpr int kMinNumDeltas = 60, kDeltaCounterMax = 1000;

TrendlineEstimatorSettings normalize(TrendlineEstimatorSettings config)
{
    if (config.window_size < 10 || config.window_size > 200)
    {
        UAV_LOG_WARN << "[WebRTC][TrendlineEstimator] invalid window size, using 20";
        config.window_size = TrendlineEstimatorSettings::kDefaultTrendlineWindowSize;
    }
    if (config.enable_cap)
    {
        if (config.beginning_packets < 1 || config.end_packets < 1 ||
            config.beginning_packets > config.window_size || config.end_packets > config.window_size ||
            config.beginning_packets + config.end_packets > config.window_size)
        {
            UAV_LOG_WARN << "[WebRTC][TrendlineEstimator] invalid cap packet counts, disabling cap";
            config.enable_cap = false;
            config.beginning_packets = config.end_packets = 0;
            config.cap_uncertainty = 0;
        }
        if (!std::isfinite(config.cap_uncertainty) || config.cap_uncertainty < 0 || config.cap_uncertainty > 0.025)
        {
            UAV_LOG_WARN << "[WebRTC][TrendlineEstimator] invalid cap uncertainty, using 0";
            config.cap_uncertainty = 0;
        }
    }
    return config;
}
std::optional<double> linearFitSlope(const std::deque<TrendlineEstimator::PacketTiming>& packets)
{
    double sum_x = 0, sum_y = 0;
    for (const auto& packet : packets) { sum_x += packet.arrival_time_ms; sum_y += packet.smoothed_delay_ms; }
    const auto x_avg = sum_x / packets.size(), y_avg = sum_y / packets.size();
    double numerator = 0, denominator = 0;
    for (const auto& packet : packets)
    {
        numerator += (packet.arrival_time_ms - x_avg) * (packet.smoothed_delay_ms - y_avg);
        denominator += (packet.arrival_time_ms - x_avg) * (packet.arrival_time_ms - x_avg);
    }
    if (denominator == 0) return std::nullopt;
    return numerator / denominator;
}
std::optional<double> computeSlopeCap(const std::deque<TrendlineEstimator::PacketTiming>& packets,
                                     const TrendlineEstimatorSettings& config)
{
    auto early = packets.front();
    for (std::size_t i = 1; i < config.beginning_packets; ++i)
        if (packets[i].raw_delay_ms < early.raw_delay_ms) early = packets[i];
    const auto late_start = packets.size() - config.end_packets;
    auto late = packets[late_start];
    for (auto i = late_start + 1; i < packets.size(); ++i)
        if (packets[i].raw_delay_ms < late.raw_delay_ms) late = packets[i];
    if (late.arrival_time_ms - early.arrival_time_ms < 1) return std::nullopt;
    return (late.raw_delay_ms - early.raw_delay_ms) / (late.arrival_time_ms - early.arrival_time_ms) + config.cap_uncertainty;
}
} // namespace

TrendlineEstimator::TrendlineEstimator() : TrendlineEstimator(Config{}) {}
TrendlineEstimator::TrendlineEstimator(Config config, NetworkStatePredictor* predictor)
    : settings_(normalize(config)), network_state_predictor_(predictor)
{
    UAV_LOG_INFO << "[WebRTC][TrendlineEstimator] window=" << settings_.window_size << " sort=" << settings_.enable_sort
        << " cap=" << settings_.enable_cap << " predictor=" << (predictor != nullptr);
}
TrendlineEstimator::~TrendlineEstimator() = default;

void TrendlineEstimator::UpdateTrendline(double recv_delta_ms, double send_delta_ms, std::int64_t send_time_ms,
                                         std::int64_t arrival_time_ms, std::size_t /*packet_size*/)
{
    const auto delta_ms = recv_delta_ms - send_delta_ms;
    const auto accumulated = accumulated_delay_ + delta_ms;
    const auto smoothed = kSmoothingCoeff * smoothed_delay_ + (1 - kSmoothingCoeff) * accumulated;
    if (send_time_ms < 0 || arrival_time_ms < 0 || !std::isfinite(recv_delta_ms) || !std::isfinite(send_delta_ms) ||
        !std::isfinite(accumulated) || !std::isfinite(smoothed))
    {
        UAV_LOG_WARN << "[WebRTC][TrendlineEstimator][" << this << "] invalid sample send_ms=" << send_time_ms
            << " arrival_ms=" << arrival_time_ms << " send_delta_ms=" << send_delta_ms << " receive_delta_ms=" << recv_delta_ms;
        throw std::invalid_argument("invalid trendline sample or timestamp");
    }
    num_of_deltas_ = std::min(num_of_deltas_ + 1, kDeltaCounterMax);
    if (first_arrival_time_ms_ == -1) first_arrival_time_ms_ = arrival_time_ms;
    accumulated_delay_ = accumulated;
    smoothed_delay_ = smoothed;
    delay_hist_.push_back({static_cast<double>(arrival_time_ms - first_arrival_time_ms_), smoothed_delay_, accumulated_delay_});
    if (settings_.enable_sort)
    {
        for (auto i = delay_hist_.size() - 1; i > 0 && delay_hist_[i].arrival_time_ms < delay_hist_[i - 1].arrival_time_ms; --i)
            std::swap(delay_hist_[i], delay_hist_[i - 1]);
    }
    if (delay_hist_.size() > settings_.window_size) delay_hist_.pop_front();

    auto trend = prev_trend_;
    if (delay_hist_.size() == settings_.window_size)
    {
        trend = linearFitSlope(delay_hist_).value_or(trend);
        if (settings_.enable_cap)
        {
            const auto cap = computeSlopeCap(delay_hist_, settings_);
            if (trend >= 0 && cap && trend > *cap) trend = *cap;
        }
    }
    Detect(trend, send_delta_ms, arrival_time_ms);
    UAV_LOG_IF(LogLevel::TRACE, LogManager::getInstance().shouldLog(LogLevel::TRACE)) << "[WebRTC][TrendlineEstimator][" << this << "] receive_delta_ms=" << recv_delta_ms
        << " send_delta_ms=" << send_delta_ms << " accumulated_delay_ms=" << accumulated_delay_ << " smoothed_delay_ms=" << smoothed_delay_
        << " samples=" << delay_hist_.size() << " trend=" << trend << " threshold=" << threshold_
        << " hypothesis=" << static_cast<int>(hypothesis_);
}
void TrendlineEstimator::Update(double recv_delta_ms, double send_delta_ms, std::int64_t send_time_ms,
                                std::int64_t arrival_time_ms, std::size_t packet_size, bool calculated_deltas)
{
    if (send_time_ms < 0 || arrival_time_ms < 0)
    {
        UAV_LOG_WARN << "[WebRTC][TrendlineEstimator][" << this << "] negative send_ms=" << send_time_ms << " arrival_ms=" << arrival_time_ms;
        throw std::invalid_argument("negative trendline timestamp");
    }
    if (calculated_deltas) UpdateTrendline(recv_delta_ms, send_delta_ms, send_time_ms, arrival_time_ms, packet_size);
    if (network_state_predictor_)
    {
        const auto predicted = network_state_predictor_->Update(send_time_ms, arrival_time_ms, hypothesis_);
        if (predicted != hypothesis_predicted_)
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][TrendlineEstimator] predicted state=" << static_cast<int>(predicted);
        hypothesis_predicted_ = predicted;
    }
}
BandwidthUsage TrendlineEstimator::State() const { return network_state_predictor_ ? hypothesis_predicted_ : hypothesis_; }

void TrendlineEstimator::Detect(double trend, double ts_delta, std::int64_t now_ms)
{
    if (num_of_deltas_ < 2) { hypothesis_ = BandwidthUsage::kBwNormal; return; }
    const auto modified_trend = std::min(num_of_deltas_, kMinNumDeltas) * trend * kThresholdGain;
    prev_modified_trend_ = modified_trend;
    const auto previous = hypothesis_;
    if (modified_trend > threshold_)
    {
        if (time_over_using_ == -1) time_over_using_ = ts_delta / 2;
        else time_over_using_ += ts_delta;
        // Saturation only affects inputs beyond the upstream counter's int range.
        if (overuse_counter_ < std::numeric_limits<int>::max()) ++overuse_counter_;
        if (time_over_using_ > kOverUsingTimeThreshold && overuse_counter_ > 1 && trend >= prev_trend_)
        {
            time_over_using_ = 0;
            overuse_counter_ = 0;
            hypothesis_ = BandwidthUsage::kBwOverusing;
        }
    }
    else if (modified_trend < -threshold_)
    {
        time_over_using_ = -1;
        overuse_counter_ = 0;
        hypothesis_ = BandwidthUsage::kBwUnderusing;
    }
    else
    {
        time_over_using_ = -1;
        overuse_counter_ = 0;
        hypothesis_ = BandwidthUsage::kBwNormal;
    }
    if (hypothesis_ != previous)
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][TrendlineEstimator] hypothesis=" << static_cast<int>(hypothesis_)
            << " modified_trend=" << modified_trend << " threshold=" << threshold_;
    prev_trend_ = trend;
    UpdateThreshold(modified_trend, now_ms);
}
void TrendlineEstimator::UpdateThreshold(double modified_trend, std::int64_t now_ms)
{
    if (last_update_ms_ == -1) last_update_ms_ = now_ms;
    if (std::abs(modified_trend) > threshold_ + kMaxAdaptOffsetMs) { last_update_ms_ = now_ms; return; }
    const auto k = std::abs(modified_trend) < threshold_ ? 0.039 : 0.0087;
    const auto time_delta_ms = std::min<std::int64_t>(now_ms - last_update_ms_, 100);
    threshold_ += k * (std::abs(modified_trend) - threshold_) * time_delta_ms;
    threshold_ = std::clamp(threshold_, 6.0, 600.0);
    last_update_ms_ = now_ms;
}
TrendlineEstimator::Stats TrendlineEstimator::Snapshot() const
{
    return {delay_hist_.size(), num_of_deltas_, accumulated_delay_, smoothed_delay_, prev_trend_, prev_modified_trend_,
        threshold_, time_over_using_, overuse_counter_, hypothesis_, State()};
}
} // namespace webrtc
