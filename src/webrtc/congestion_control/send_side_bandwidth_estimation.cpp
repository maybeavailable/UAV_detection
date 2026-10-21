// Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
// Adapted from libwebrtc 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
// UMA/event-log plumbing is replaced with the project's log library.
#include"webrtc/congestion_control/send_side_bandwidth_estimation.h"
#include"webrtc/congestion_control/bandwidth_usage.h"
#include"webrtc/congestion_control/rate_math.h"
#include"log/log.h"
#include<algorithm>
#include<limits>
#include<stdexcept>

namespace webrtc
{
SendSideBandwidthEstimation::SendSideBandwidthEstimation() : SendSideBandwidthEstimation(Config{}) {}
SendSideBandwidthEstimation::SendSideBandwidthEstimation(Config config) : config_(config), loss_(std::make_unique<LossBasedBweV2>(config.loss))
{
    if (!std::isfinite(config.rtt_drop_fraction) || config.rtt_drop_fraction <= 0 || config.rtt_drop_fraction >= 1 ||
        config.rtt_limit.count() <= 0 || config.rtt_drop_interval.count() <= 0 || config.rtt_bandwidth_floor == 0 ||
        !std::isfinite(config.low_loss_threshold) || !std::isfinite(config.high_loss_threshold) ||
        config.low_loss_threshold < 0 || config.high_loss_threshold > 1 || config.low_loss_threshold > config.high_loss_threshold)
        throw std::invalid_argument("invalid send-side bandwidth estimation configuration");
    ValidateRate(config.rtt_bandwidth_floor); ValidateRate(config.bitrate_threshold);
    loss_->SetMinMaxBitrate(min_bitrate_, max_bitrate_);
}
void SendSideBandwidthEstimation::ValidateRate(std::uint64_t rate)
{ if (rate > 1000000000) throw std::invalid_argument("GoogCC rate exceeds 1 Gbit/s"); }
void SendSideBandwidthEstimation::OnRouteChange()
{
    accumulated_lost_ = 0; accumulated_expected_ = 0; target_ = 0;
    min_bitrate_ = 5000; max_bitrate_ = 1000000000;
    decreased_since_loss_ = false; last_loss_report_.reset(); fraction_loss_ = 0; rtt_ = Delta::zero();
    receiver_limit_.reset(); delay_limit_.reset(); last_decrease_.reset(); first_report_.reset();
    // Source retains min history, RTT backoff and loss_state_ on route changes.
    // SetSendBitrate clears history when the new route provides a start rate.
    if (loss_->IsEnabled() && loss_->UseInStartPhase()) loss_ = std::make_unique<LossBasedBweV2>(config_.loss);
    UAV_LOG_INFO << "[WebRTC][SendSideBwe][" << this << "] route reset";
}
std::uint64_t SendSideBandwidthEstimation::target_rate() const
{ return std::max(min_bitrate_, config_.receiver_limit_caps_only && receiver_limit_ ? std::min(target_, *receiver_limit_) : target_); }
bool SendSideBandwidthEstimation::IsRttAboveLimit() const
{
    if (!config_.rtt_backoff_enabled || !last_propagation_update_) return false;
    const auto correction = last_packet_sent_ ? std::max(std::chrono::duration_cast<Delta>(*last_packet_sent_ - *last_propagation_update_), Delta::zero()) : Delta::zero();
    // Wide comparison also handles a negative propagation sample and a very
    // large configured limit without overflowing a signed duration.
    return static_cast<long double>(propagation_rtt_.count()) + correction.count() > config_.rtt_limit.count();
}
void SendSideBandwidthEstimation::SetMinMaxBitrate(std::uint64_t min, std::optional<std::uint64_t> max)
{
    ValidateRate(min); if (max) ValidateRate(*max);
    min_bitrate_ = std::max<std::uint64_t>(min, 5000);
    max_bitrate_ = max && *max > 0 ? std::max(min_bitrate_, *max) : 1000000000;
    loss_->SetMinMaxBitrate(min_bitrate_, max_bitrate_);
}
void SendSideBandwidthEstimation::SetBitrates(std::optional<std::uint64_t> start, std::uint64_t min,
    std::optional<std::uint64_t> max, Time at)
{
    if (start) { ValidateRate(*start); if (!*start) throw std::invalid_argument("zero start bitrate"); }
    SetMinMaxBitrate(min, max);
    if (start) SetSendBitrate(*start, at);
}
void SendSideBandwidthEstimation::SetSendBitrate(std::uint64_t bitrate, Time at)
{
    if (!bitrate) throw std::invalid_argument("zero send bitrate");
    delay_limit_.reset(); UpdateTarget(bitrate, at); min_history_.clear();
}
void SendSideBandwidthEstimation::UpdateReceiverEstimate(Time at, std::optional<std::uint64_t> rate)
{
    receiver_limit_ = rate && *rate ? rate : std::nullopt; UpdateTarget(target_, at);
}
void SendSideBandwidthEstimation::UpdateDelayBasedEstimate(Time at, std::uint64_t rate)
{
    delay_limit_ = rate ? std::optional<std::uint64_t>(rate) : std::nullopt; UpdateTarget(target_, at);
}
void SendSideBandwidthEstimation::SetAcknowledgedRate(std::optional<std::uint64_t> rate, Time /*at*/)
{ if (rate && loss_->IsEnabled()) loss_->SetAcknowledgedBitrate(*rate); }
void SendSideBandwidthEstimation::UpdateLossBasedEstimator(const TransportPacketsFeedback& report, BandwidthUsage,
    std::optional<std::uint64_t>, bool in_alr)
{
    if (loss_->IsEnabled())
    {
        loss_->UpdateBandwidthEstimate(report.packet_feedbacks, delay_limit_, in_alr);
        UpdateEstimate(report.feedback_time);
    }
}
void SendSideBandwidthEstimation::UpdatePacketsLost(std::int64_t lost, std::uint64_t total, Time at)
{
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (total > static_cast<std::uint64_t>(max) - accumulated_expected_ ||
        (lost > 0 && accumulated_lost_ > max - lost) ||
        (lost < 0 && accumulated_lost_ < std::numeric_limits<std::int64_t>::min() - lost))
        throw std::overflow_error("loss report counters overflow");
    if (!first_report_) first_report_ = at;
    if (!total) return;
    const auto expected = accumulated_expected_ + total;
    const auto lost_total = accumulated_lost_ + lost;
    if (expected < 20) { accumulated_expected_ = expected; accumulated_lost_ = lost_total; return; }
    decreased_since_loss_ = false;
    const auto positive = static_cast<std::uint64_t>(std::max<std::int64_t>(lost_total, 0));
    // Match floor(256 * lost / expected), saturating to 255 without overflow.
    fraction_loss_ = static_cast<std::uint8_t>(std::min<long double>(255, std::floor(static_cast<long double>(positive) * 256 / expected)));
    accumulated_lost_ = 0; accumulated_expected_ = 0; last_loss_report_ = at;
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][SendSideBwe][" << this
        << "] RR lost=" << lost_total << " expected=" << expected << " fraction_q8=" << unsigned(fraction_loss_);
    UpdateEstimate(at);
}
void SendSideBandwidthEstimation::UpdateRtt(Delta rtt, Time /*at*/)
{ if (rtt.count() < 0) throw std::invalid_argument("negative RTT"); if (rtt > Delta::zero()) rtt_ = rtt; }
void SendSideBandwidthEstimation::UpdatePropagationRtt(Time at, Delta rtt)
{ last_propagation_update_ = at; propagation_rtt_ = rtt; }
void SendSideBandwidthEstimation::OnSentPacket(const SentPacket& packet)
{ if (!packet.send_time) throw std::invalid_argument("missing send timestamp"); last_packet_sent_ = packet.send_time; }
bool SendSideBandwidthEstimation::InStartPhase(Time at) const
{ return !first_report_ || at - *first_report_ < std::chrono::seconds(2); }
void SendSideBandwidthEstimation::UpdateMinHistory(Time at)
{
    while (!min_history_.empty() && at - min_history_.front().first > std::chrono::milliseconds(999)) min_history_.pop_front();
    while (!min_history_.empty() && target_ <= min_history_.back().second) min_history_.pop_back();
    min_history_.emplace_back(at, target_);
}
void SendSideBandwidthEstimation::UpdateTarget(std::uint64_t rate, Time /*at*/)
{
    auto upper = std::min(max_bitrate_, delay_limit_.value_or(max_bitrate_));
    if (!config_.receiver_limit_caps_only && receiver_limit_) upper = std::min(upper, *receiver_limit_);
    const auto next = std::max(min_bitrate_, std::min(rate, upper));
    if (next != target_)
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][SendSideBwe][" << this
            << "] target_bps=" << next << " previous_bps=" << target_ << " upper_bps=" << upper << " loss_state=" << static_cast<int>(loss_state_);
    target_ = next;
}
void SendSideBandwidthEstimation::UpdateEstimate(Time at)
{
    if (IsRttAboveLimit())
    {
        if ((!last_decrease_ || at - *last_decrease_ >= config_.rtt_drop_interval) && target_ > config_.rtt_bandwidth_floor)
        {
            last_decrease_ = at; UpdateTarget(std::max(cc_detail::scale(target_, config_.rtt_drop_fraction), config_.rtt_bandwidth_floor), at);
        }
        else UpdateTarget(target_, at);
        return;
    }
    if (!fraction_loss_ && InStartPhase(at) && !loss_->ReadyToUseInStartPhase())
    {
        auto next = target_;
        if (receiver_limit_) next = std::max(next, *receiver_limit_);
        if (delay_limit_) next = std::max(next, *delay_limit_);
        if (next != target_)
        {
            min_history_.clear(); min_history_.emplace_back(at, target_); UpdateTarget(next, at); return;
        }
    }
    UpdateMinHistory(at);
    if (loss_->IsReady())
    {
        const auto result = loss_->GetLossBasedResult(); loss_state_ = result.state;
        if (result.bandwidth_estimate) UpdateTarget(*result.bandwidth_estimate, at);
        return;
    }
    if (last_loss_report_ && at - *last_loss_report_ < std::chrono::seconds(6))
    {
        const float loss = fraction_loss_ / 256.0f;
        if (target_ < config_.bitrate_threshold || loss <= config_.low_loss_threshold)
        {
            const auto next = cc_detail::bounded(min_history_.front().second * 1.08 + 0.5) + 1000;
            UpdateTarget(next, at); return;
        }
        if (target_ > config_.bitrate_threshold && loss > config_.high_loss_threshold && !decreased_since_loss_ &&
            (!last_decrease_ || std::chrono::duration_cast<Delta>(at - *last_decrease_) - rtt_ >= std::chrono::milliseconds(300)))
        {
            last_decrease_ = at; decreased_since_loss_ = true;
            UpdateTarget(cc_detail::bounded(target_ * static_cast<double>(512 - fraction_loss_) / 512.0), at); return;
        }
    }
    UpdateTarget(target_, at);
}
} // namespace webrtc
