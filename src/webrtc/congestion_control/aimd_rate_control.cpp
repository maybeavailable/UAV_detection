/*
 * Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/remote_bitrate_estimator/aimd_rate_control.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/aimd_rate_control.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
using Delta = AimdRateControl::Delta;
using Time = AimdRateControl::Time;
Delta micros(Time time) { return std::chrono::duration_cast<Delta>(time.time_since_epoch()); }
std::uint64_t addRate(std::uint64_t a, std::uint64_t b)
{
    return a + std::min(b, std::numeric_limits<std::uint64_t>::max() - a);
}
std::uint64_t rateFromDouble(double bps, bool round)
{
    // DataRate * double rounds; DataRate::BitsPerSec(double) truncates.
    // Saturate before conversion, including outside upstream's int64 range.
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (round) bps = std::round(bps);
    if (static_cast<long double>(bps) >= static_cast<long double>(max)) return max;
    return bps > 0 ? static_cast<std::uint64_t>(bps) : 0;
}
std::uint64_t scaleRate(std::uint64_t bps, double scale) { return rateFromDouble(bps * scale, true); }
AimdRateControl::Config checkedConfig(AimdRateControl::Config config)
{
    if (!std::isfinite(config.backoff_factor) || config.backoff_factor <= 0 || config.backoff_factor >= 1)
    {
        UAV_LOG_WARN << "[WebRTC][AimdRateControl] invalid backoff factor, using 0.85";
        config.backoff_factor = 0.85;
    }
    return config;
}
} // namespace

AimdRateControl::AimdRateControl() : AimdRateControl(Config{}) {}
AimdRateControl::AimdRateControl(Config config) : config_(checkedConfig(config))
{
    UAV_LOG_INFO << "[WebRTC][AimdRateControl] initialized beta=" << config_.backoff_factor
        << " send_side=" << config_.send_side << " hold_in_alr=" << config_.no_bitrate_increase_in_alr;
}
bool AimdRateControl::ValidEstimate() const { return initialized_; }
void AimdRateControl::SetStartBitrate(std::uint64_t bitrate)
{
    current_bitrate_ = latest_throughput_ = bitrate;
    initialized_ = true; // Like upstream, this setter does not clamp or reset history.
}
void AimdRateControl::SetMinBitrate(std::uint64_t bitrate)
{
    min_bitrate_ = bitrate;
    current_bitrate_ = std::max(bitrate, current_bitrate_);
}
AimdRateControl::Delta AimdRateControl::GetFeedbackInterval() const
{
    const auto rtcp_bps = scaleRate(current_bitrate_, 0.05);
    // 80 bytes / 5% of target rate; DataSize / DataRate truncates to us.
    if (rtcp_bps == 0) return std::chrono::seconds(1);
    return std::clamp(Delta(640000000 / rtcp_bps), Delta(200000), Delta(1000000));
}
void AimdRateControl::ValidateTime(Time at_time) const
{
    if (at_time.time_since_epoch() < Time::duration::zero() ||
        (last_update_time_ && micros(at_time) < *last_update_time_))
        throw std::invalid_argument("AIMD requires nonnegative, nondecreasing control timestamps");
}
bool AimdRateControl::TimeToReduceFurther(Time at_time, std::uint64_t throughput) const
{
    ValidateTime(at_time);
    const auto interval = std::clamp(rtt_, Delta(10000), Delta(200000));
    if (!last_bitrate_change_ || micros(at_time) - *last_bitrate_change_ >= interval) return true;
    return initialized_ && throughput < scaleRate(current_bitrate_, 0.5);
}
bool AimdRateControl::InitialTimeToReduceFurther(Time at_time) const
{
    // Upstream passes current / 2 - 1, which satisfies the half-rate test even
    // within an RTT. Preserve this behavior rather than its outdated comment.
    if (!initialized_) return false;
    if (current_bitrate_ < 2) { ValidateTime(at_time); return true; }
    return TimeToReduceFurther(at_time, current_bitrate_ / 2 - 1);
}
std::uint64_t AimdRateControl::LatestEstimate() const { return current_bitrate_; }
void AimdRateControl::SetRtt(Delta rtt)
{
    if (rtt < Delta::zero() || rtt.count() > (std::numeric_limits<std::int64_t>::max() / 2 - 100000))
        throw std::invalid_argument("invalid AIMD RTT");
    rtt_ = rtt;
}
std::uint64_t AimdRateControl::Update(const RateControlInput& input, Time at_time)
{
    ValidateTime(at_time);
    if (input.bw_state != BandwidthUsage::kBwNormal && input.bw_state != BandwidthUsage::kBwUnderusing &&
        input.bw_state != BandwidthUsage::kBwOverusing)
        throw std::invalid_argument("invalid AIMD bandwidth usage state");
    const auto now = micros(at_time);
    if (!initialized_)
    {
        if (!first_throughput_time_)
        {
            if (input.estimated_throughput) first_throughput_time_ = now;
        }
        else if (now - *first_throughput_time_ > std::chrono::seconds(5) && input.estimated_throughput)
        {
            current_bitrate_ = *input.estimated_throughput;
            initialized_ = true;
        }
    }
    ChangeBitrate(input, now);
    last_update_time_ = now;
    return current_bitrate_;
}
void AimdRateControl::SetInApplicationLimitedRegion(bool in_alr) { in_alr_ = in_alr; }
void AimdRateControl::SetEstimate(std::uint64_t bitrate, Time at_time)
{
    ValidateTime(at_time);
    initialized_ = true;
    current_bitrate_ = ClampBitrate(bitrate);
    last_bitrate_change_ = last_update_time_ = micros(at_time);
}
void AimdRateControl::SetNetworkStateEstimate(std::optional<NetworkStateEstimate> estimate) { network_estimate_ = estimate; }
double AimdRateControl::GetNearMaxIncreaseRateBpsPerSecond() const
{
    // Source: one 1/30 s frame (33333 us), nearest whole bytes. Split the
    // product so even the complete uint64_t input domain cannot overflow.
    const auto frame_bytes = (current_bitrate_ / 8000000) * 33333 +
        ((current_bitrate_ % 8000000) * 33333 + 4000000) / 8000000;
    if (!frame_bytes) return 4000; // Defined recovery for zero/sub-byte rates.
    const auto packets = std::ceil(static_cast<double>(frame_bytes) / 1200);
    const auto average_bytes = static_cast<std::uint64_t>(std::llround(frame_bytes / packets));
    const auto response_us = (rtt_.count() + 100000) * 2;
    const auto increase_bps = average_bytes * 8000000 / static_cast<std::uint64_t>(response_us);
    return std::max(4000.0, static_cast<double>(increase_bps));
}
void AimdRateControl::ChangeBitrate(const RateControlInput& input, Delta now)
{
    const auto throughput = input.estimated_throughput.value_or(latest_throughput_);
    if (input.estimated_throughput) latest_throughput_ = throughput;
    if (!initialized_ && input.bw_state != BandwidthUsage::kBwOverusing) return;

    switch (input.bw_state)
    {
    case BandwidthUsage::kBwNormal:
        if (state_ == State::Hold) { last_bitrate_change_ = now; state_ = State::Increase; }
        break;
    case BandwidthUsage::kBwUnderusing: state_ = State::Hold; break;
    case BandwidthUsage::kBwOverusing: state_ = State::Decrease; break;
    default: break; // Validated by Update before state mutation.
    }
    auto next = current_bitrate_;
    if (state_ == State::Increase)
    {
        const auto upper = link_capacity_.UpperBound();
        if (upper && throughput > *upper) link_capacity_.Reset();
        auto limit = addRate(scaleRate(throughput, 1.5), 10000);
        if (config_.send_side && in_alr_ && config_.no_bitrate_increase_in_alr) limit = current_bitrate_;
        if (current_bitrate_ < limit)
        {
            std::uint64_t increase;
            if (link_capacity_.has_estimate())
            {
                const double seconds = (now - *last_bitrate_change_).count() * 1e-6;
                increase = rateFromDouble(GetNearMaxIncreaseRateBpsPerSecond() * seconds, false);
            }
            else
            {
                const double seconds = last_bitrate_change_ ? (now - *last_bitrate_change_).count() * 1e-6 : 1.0;
                const double alpha = std::pow(1.08, std::min(seconds, 1.0));
                increase = std::max(scaleRate(current_bitrate_, alpha - 1.0), std::uint64_t(1000));
            }
            next = std::min(addRate(current_bitrate_, increase), limit);
        }
        last_bitrate_change_ = now;
    }
    else if (state_ == State::Decrease)
    {
        auto decreased = scaleRate(throughput, config_.backoff_factor);
        if (decreased > 5000) decreased -= 5000;
        if (decreased > current_bitrate_ && link_capacity_.has_estimate())
            decreased = scaleRate(*link_capacity_.estimate(), config_.backoff_factor);
        if (decreased < current_bitrate_) next = decreased;
        if (throughput < link_capacity_.LowerBound()) link_capacity_.Reset();
        initialized_ = true;
        link_capacity_.OnOveruseDetected(throughput);
        state_ = State::Hold;
        last_bitrate_change_ = now;
    }
    const auto previous = current_bitrate_;
    current_bitrate_ = ClampBitrate(next);
    if (current_bitrate_ < previous)
        UAV_LOG_DEBUG << "[WebRTC][AimdRateControl] decreased target_bps=" << current_bitrate_
            << " previous_bps=" << previous << " acknowledged_bps=" << throughput;
}
std::uint64_t AimdRateControl::ClampBitrate(std::uint64_t bitrate) const
{
    if (!config_.disable_estimate_bounded_increase && network_estimate_ && network_estimate_->link_capacity_upper)
    {
        const auto upper = config_.use_current_estimate_as_min_upper_bound
            ? std::max(*network_estimate_->link_capacity_upper, current_bitrate_) : *network_estimate_->link_capacity_upper;
        bitrate = std::min(upper, bitrate);
    }
    if (network_estimate_ && network_estimate_->link_capacity_lower && bitrate < current_bitrate_)
        bitrate = std::min(current_bitrate_, std::max(bitrate, scaleRate(*network_estimate_->link_capacity_lower, config_.backoff_factor)));
    return std::max(bitrate, min_bitrate_);
}
} // namespace webrtc
