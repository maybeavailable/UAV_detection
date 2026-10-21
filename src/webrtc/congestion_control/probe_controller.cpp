/*
 * Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/probe_controller.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/probe_controller.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<iterator>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
using Delta = ProbeController::Delta;
using Time = ProbeController::Time;
constexpr Delta kMaxWaitingTimeForProbingResult{std::chrono::seconds(1)};
constexpr std::uint64_t kDefaultMaxProbingBitrate = 5000000;
Delta micros(Time time)
{
    if (time.time_since_epoch() < Time::duration::zero())
    {
        UAV_LOG_WARN << "[WebRTC][ProbeController] negative timestamp";
        throw std::invalid_argument("negative probe controller timestamp");
    }
    return std::chrono::duration_cast<Delta>(time.time_since_epoch());
}
Time timeFromMicros(Delta time)
{
    const auto maximum = std::chrono::duration_cast<Delta>(Time::max().time_since_epoch());
    return time > maximum ? Time::max() : Time(time);
}
Delta addTime(Delta a, Delta b)
{
    // All stored finite times and configurable durations are nonnegative.
    return Delta(a.count() + std::min(b.count(), std::numeric_limits<std::int64_t>::max() - a.count()));
}
std::uint64_t scaleRate(std::uint64_t rate, double scale)
{
    // DataRate * double rounds, rather than truncates, to whole bit/s.
    const auto value = std::round(static_cast<double>(rate) * scale);
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return static_cast<long double>(value) >= static_cast<long double>(max) ? max : static_cast<std::uint64_t>(value);
}
std::uint64_t twiceRate(std::uint64_t rate)
{
    return rate + std::min(rate, std::numeric_limits<std::uint64_t>::max() - rate);
}
ProbeControllerConfig checkedConfig(ProbeControllerConfig c)
{
    const double positive[] = {c.first_exponential_probe_scale, c.further_exponential_probe_scale,
        c.alr_probe_scale, c.network_state_probe_scale, c.allocation_probe_limit_by_current_scale, c.loss_limited_probe_scale};
    const double nonnegative[] = {c.further_probe_threshold, c.probe_if_estimate_lower_than_network_state_estimate_ratio,
        c.skip_if_estimate_larger_than_fraction_of_max, c.skip_probe_max_allocated_scale};
    const Delta durations[] = {c.initial_probe_duration, c.initial_min_probe_delta, c.alr_probing_interval,
        c.network_state_probe_duration, c.network_state_min_probe_delta, c.min_probe_duration, c.min_probe_delta};
    const auto validOptionalScale = [](std::optional<double> scale, bool allow_zero) {
        return !scale || (std::isfinite(*scale) && (allow_zero ? *scale >= 0 : *scale > 0));
    };
    if (!std::all_of(std::begin(positive), std::end(positive), [](double v) { return std::isfinite(v) && v > 0; }) ||
        !std::all_of(std::begin(nonnegative), std::end(nonnegative), [](double v) { return std::isfinite(v) && v >= 0; }) ||
        !std::all_of(std::begin(durations), std::end(durations), [](Delta v) { return v > Delta::zero(); }) ||
        !validOptionalScale(c.second_exponential_probe_scale, true) || !validOptionalScale(c.first_allocation_probe_scale, false) ||
        !validOptionalScale(c.second_allocation_probe_scale, true) || c.repeated_initial_probing_time_period < Delta::zero() ||
        (c.network_state_estimate_probing_interval && *c.network_state_estimate_probing_interval <= Delta::zero()) ||
        (c.estimate_lower_than_network_state_estimate_probing_interval && *c.estimate_lower_than_network_state_estimate_probing_interval <= Delta::zero()) ||
        c.min_probe_packets_sent <= 0)
    {
        UAV_LOG_WARN << "[WebRTC][ProbeController] invalid configuration";
        throw std::invalid_argument("invalid probe controller configuration");
    }
    return c;
}
bool validCause(BandwidthLimitedCause cause)
{
    return cause >= BandwidthLimitedCause::kLossLimitedBweIncreasing && cause <= BandwidthLimitedCause::kRttBasedBackOffHighRtt;
}
} // namespace

ProbeController::ProbeController() : ProbeController(Config{}) {}
ProbeController::ProbeController(Config config) : config_(checkedConfig(config))
{
    UAV_LOG_INFO << "[WebRTC][ProbeController] initialized first_scale=" << config_.first_exponential_probe_scale
        << " further_scale=" << config_.further_exponential_probe_scale << " rapid_recovery=" << config_.rapid_recovery;
}
ProbeController::Delta ProbeController::ObserveTime(Time time)
{
    const auto now = micros(time);
    if (last_event_time_ && now < *last_event_time_)
    {
        UAV_LOG_WARN << "[WebRTC][ProbeController][" << this << "] backwards control time_us=" << now.count()
            << " previous_us=" << last_event_time_->count();
        throw std::invalid_argument("probe control timestamps moved backwards");
    }
    last_event_time_ = now;
    return now;
}
std::vector<ProbeClusterConfig> ProbeController::SetBitrates(std::uint64_t min, std::uint64_t start,
    std::optional<std::uint64_t> max, Time at_time)
{
    if (max && *max == 0)
    {
        UAV_LOG_WARN << "[WebRTC][ProbeController][" << this << "] maximum probe bitrate is zero";
        throw std::invalid_argument("maximum probe bitrate must be positive");
    }
    const auto now = ObserveTime(at_time);
    if (start > 0) start_bitrate_ = estimated_bitrate_ = start;
    else if (start_bitrate_ == 0) start_bitrate_ = min;
    const auto old_max = max_bitrate_;
    max_bitrate_ = max.value_or(kDefaultMaxProbingBitrate);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] set bitrates min_bps=" << min << " start_bps=" << start_bitrate_
        << " max_bps=" << max_bitrate_ << " network_available=" << network_available_ << " state=" << static_cast<int>(state_);
    switch (state_)
    {
    case State::kInit:
        if (network_available_ && start_bitrate_ > 0) return InitiateExponentialProbing(now);
        break;
    case State::kWaitingForProbingResult: break;
    case State::kProbingComplete:
        if (estimated_bitrate_ > 0 && old_max < max_bitrate_ && estimated_bitrate_ < max_bitrate_)
            return InitiateProbing(now, {max_bitrate_}, false);
        break;
    }
    return {};
}
std::vector<ProbeClusterConfig> ProbeController::OnMaxTotalAllocatedBitrate(std::uint64_t bitrate, Time at_time)
{
    const auto now = ObserveTime(at_time);
    const bool allow = alr_start_time_.has_value() || config_.probe_on_max_allocated_bitrate_change_without_alr;
    if (bitrate != max_total_allocated_bitrate_)
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] allocated_bps=" << bitrate
            << " previous_bps=" << max_total_allocated_bitrate_ << " allow_allocation_probe=" << allow;
    if (config_.probe_on_max_allocated_bitrate_change && state_ == State::kProbingComplete &&
        bitrate > max_total_allocated_bitrate_ && estimated_bitrate_ < max_bitrate_ && estimated_bitrate_ < bitrate && allow)
    {
        max_total_allocated_bitrate_ = bitrate;
        if (!config_.first_allocation_probe_scale) return {};
        auto first = scaleRate(bitrate, *config_.first_allocation_probe_scale);
        const auto limit = scaleRate(estimated_bitrate_, config_.allocation_probe_limit_by_current_scale);
        bool limited = limit < first;
        if (limited) first = limit;
        std::vector<std::uint64_t> probes{first};
        if (!limited && config_.second_allocation_probe_scale)
        {
            auto second = scaleRate(bitrate, *config_.second_allocation_probe_scale);
            limited = limit < second;
            if (limited) second = limit;
            if (second > first) probes.push_back(second);
        }
        return InitiateProbing(now, probes, limited);
    }
    if (bitrate > 0) last_allowed_repeated_initial_probe_ = now;
    max_total_allocated_bitrate_ = bitrate;
    return {};
}
std::vector<ProbeClusterConfig> ProbeController::OnNetworkAvailability(NetworkAvailability msg)
{
    const auto now = ObserveTime(msg.at_time);
    if (network_available_ != msg.network_available)
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] network_available=" << msg.network_available;
    network_available_ = msg.network_available;
    if (!network_available_ && state_ == State::kWaitingForProbingResult) UpdateState(State::kProbingComplete);
    if (network_available_ && state_ == State::kInit && start_bitrate_ > 0) return InitiateExponentialProbing(now);
    return {};
}
void ProbeController::UpdateState(State state)
{
    if (state != state_)
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] state=" << static_cast<int>(state) << " previous_state=" << static_cast<int>(state_);
    state_ = state;
    if (state == State::kProbingComplete) min_bitrate_to_probe_further_.reset();
}
std::vector<ProbeClusterConfig> ProbeController::InitiateExponentialProbing(Delta now)
{
    std::vector<std::uint64_t> probes{scaleRate(start_bitrate_, config_.first_exponential_probe_scale)};
    if (config_.second_exponential_probe_scale && *config_.second_exponential_probe_scale > 0)
        probes.push_back(scaleRate(start_bitrate_, *config_.second_exponential_probe_scale));
    if (repeated_initial_probing_enabled_ && max_total_allocated_bitrate_ == 0)
        last_allowed_repeated_initial_probe_ = addTime(now, config_.repeated_initial_probing_time_period);
    return InitiateProbing(now, probes, true);
}
std::vector<ProbeClusterConfig> ProbeController::SetEstimatedBitrate(std::uint64_t bitrate, BandwidthLimitedCause cause, Time at_time)
{
    if (!validCause(cause))
    {
        UAV_LOG_WARN << "[WebRTC][ProbeController][" << this << "] invalid bandwidth limited cause=" << static_cast<int>(cause);
        throw std::invalid_argument("invalid bandwidth limited cause");
    }
    const auto now = ObserveTime(at_time);
    if (bitrate != estimated_bitrate_ || cause != bandwidth_limited_cause_)
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] estimated_bps=" << bitrate << " previous_bps=" << estimated_bitrate_
            << " cause=" << static_cast<int>(cause) << " state=" << static_cast<int>(state_);
    bandwidth_limited_cause_ = cause;
    if (bitrate < scaleRate(estimated_bitrate_, 0.66))
    {
        time_of_last_large_drop_ = now;
        bitrate_before_last_large_drop_ = estimated_bitrate_;
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] large bitrate drop previous_bps="
            << estimated_bitrate_ << " current_bps=" << bitrate;
    }
    estimated_bitrate_ = bitrate;
    if (state_ == State::kWaitingForProbingResult)
    {
        if (config_.abort_further_probe_if_max_lower_than_current &&
            (bitrate > max_bitrate_ || (max_total_allocated_bitrate_ > 0 && bitrate > twiceRate(max_total_allocated_bitrate_))))
            min_bitrate_to_probe_further_.reset();
        std::optional<std::uint64_t> network_limit;
        if (config_.network_state_estimate_probing_interval && network_estimate_ && network_estimate_->link_capacity_upper)
            network_limit = scaleRate(*network_estimate_->link_capacity_upper, config_.further_probe_threshold);
        if (min_bitrate_to_probe_further_ && bitrate > *min_bitrate_to_probe_further_ && (!network_limit || bitrate <= *network_limit))
            return InitiateProbing(now, {scaleRate(bitrate, config_.further_exponential_probe_scale)}, true);
    }
    return {};
}
void ProbeController::EnablePeriodicAlrProbing(bool enable)
{
    if (enable != enable_periodic_alr_probing_) UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] periodic_alr_probing=" << enable;
    enable_periodic_alr_probing_ = enable;
}
void ProbeController::EnableRepeatedInitialProbing(bool enable)
{
    if (enable != repeated_initial_probing_enabled_) UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] repeated_initial_probing=" << enable;
    repeated_initial_probing_enabled_ = enable;
}
void ProbeController::SetAlrStartTime(std::optional<Time> time)
{
    const auto start = time ? std::optional<Delta>(micros(*time)) : std::nullopt;
    if (start != alr_start_time_) UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] alr_start_available="
        << start.has_value() << " start_us=" << start.value_or(Delta::zero()).count();
    alr_start_time_ = start;
}
void ProbeController::SetAlrEndedTime(Time time)
{
    alr_end_time_ = micros(time);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] alr_end_us=" << alr_end_time_->count();
}
std::vector<ProbeClusterConfig> ProbeController::RequestProbe(Time at_time)
{
    const auto now = ObserveTime(at_time);
    const bool recently_ended = alr_end_time_ && now - *alr_end_time_ < std::chrono::seconds(3);
    if ((alr_start_time_ || recently_ended || config_.rapid_recovery) && state_ == State::kProbingComplete)
    {
        const auto suggested = scaleRate(bitrate_before_last_large_drop_, 0.85);
        const auto min_expected = scaleRate(suggested, 0.95);
        if (min_expected > estimated_bitrate_ && now - time_of_last_large_drop_ < std::chrono::seconds(5) &&
            now - last_bwe_drop_probing_time_ > std::chrono::seconds(5))
        {
            last_bwe_drop_probing_time_ = now;
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] recovering after drop, suggested_probe_bps=" << suggested;
            return InitiateProbing(now, {suggested}, false);
        }
    }
    return {};
}
void ProbeController::SetNetworkStateEstimate(NetworkStateEstimate estimate)
{
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] network capacity lower_available=" << estimate.link_capacity_lower.has_value()
        << " lower_bps=" << estimate.link_capacity_lower.value_or(0) << " upper_available=" << estimate.link_capacity_upper.has_value()
        << " upper_bps=" << estimate.link_capacity_upper.value_or(0);
    network_estimate_ = estimate;
}
void ProbeController::Reset(Time at_time)
{
    const auto now = ObserveTime(at_time);
    bandwidth_limited_cause_ = BandwidthLimitedCause::kDelayBasedLimited;
    UpdateState(State::kInit);
    min_bitrate_to_probe_further_.reset();
    time_last_probing_initiated_ = Delta::zero();
    estimated_bitrate_ = start_bitrate_ = 0;
    network_estimate_.reset();
    max_bitrate_ = kDefaultMaxProbingBitrate;
    last_bwe_drop_probing_time_ = time_of_last_large_drop_ = now;
    alr_end_time_.reset();
    bitrate_before_last_large_drop_ = 0;
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] reset, next_cluster_id=" << next_probe_cluster_id_;
}
bool ProbeController::TimeForAlrProbe(Delta now) const
{
    return enable_periodic_alr_probing_ && alr_start_time_ &&
        now >= addTime(std::max(*alr_start_time_, time_last_probing_initiated_), config_.alr_probing_interval);
}
bool ProbeController::TimeForNetworkStateProbe(Delta now) const
{
    if (!network_estimate_ || !network_estimate_->link_capacity_upper) return false;
    const auto upper = *network_estimate_->link_capacity_upper;
    const bool low = bandwidth_limited_cause_ == BandwidthLimitedCause::kDelayBasedLimited &&
        estimated_bitrate_ < scaleRate(upper, config_.probe_if_estimate_lower_than_network_state_estimate_ratio);
    if (low && config_.estimate_lower_than_network_state_estimate_probing_interval)
        return now >= addTime(time_last_probing_initiated_, *config_.estimate_lower_than_network_state_estimate_probing_interval);
    return estimated_bitrate_ < upper && config_.network_state_estimate_probing_interval &&
        now >= addTime(time_last_probing_initiated_, *config_.network_state_estimate_probing_interval);
}
bool ProbeController::TimeForNextRepeatedInitialProbe(Delta now) const
{
    return state_ != State::kWaitingForProbingResult && last_allowed_repeated_initial_probe_ &&
        *last_allowed_repeated_initial_probe_ > now && now >= addTime(time_last_probing_initiated_, kMaxWaitingTimeForProbingResult);
}
std::vector<ProbeClusterConfig> ProbeController::Process(Time at_time)
{
    const auto now = ObserveTime(at_time);
    if (now - time_last_probing_initiated_ > kMaxWaitingTimeForProbingResult && state_ == State::kWaitingForProbingResult)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] probe result timed out";
        UpdateState(State::kProbingComplete);
    }
    if (estimated_bitrate_ == 0 || state_ != State::kProbingComplete) return {};
    if (TimeForNextRepeatedInitialProbe(now))
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] repeated initial probe estimated_bps=" << estimated_bitrate_;
        return InitiateProbing(now, {scaleRate(estimated_bitrate_, config_.first_exponential_probe_scale)}, true);
    }
    if (TimeForAlrProbe(now) || TimeForNetworkStateProbe(now))
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] periodic probe alr_due=" << TimeForAlrProbe(now)
            << " network_due=" << TimeForNetworkStateProbe(now) << " estimated_bps=" << estimated_bitrate_;
        return InitiateProbing(now, {scaleRate(estimated_bitrate_, config_.alr_probe_scale)}, true);
    }
    return {};
}
ProbeClusterConfig ProbeController::CreateProbeClusterConfig(Delta now, std::uint64_t bitrate)
{
    ProbeClusterConfig cluster;
    cluster.at_time = timeFromMicros(now);
    cluster.target_data_rate = bitrate;
    if (network_estimate_ && config_.network_state_estimate_probing_interval &&
        network_estimate_->link_capacity_upper && *network_estimate_->link_capacity_upper >= bitrate)
    {
        cluster.target_duration = config_.network_state_probe_duration;
        cluster.min_probe_delta = config_.network_state_min_probe_delta;
    }
    else if (last_allowed_repeated_initial_probe_ && now < *last_allowed_repeated_initial_probe_)
    {
        cluster.target_duration = config_.initial_probe_duration;
        cluster.min_probe_delta = config_.initial_min_probe_delta;
    }
    else
    {
        cluster.target_duration = config_.min_probe_duration;
        cluster.min_probe_delta = config_.min_probe_delta;
    }
    cluster.target_probe_count = config_.min_probe_packets_sent;
    cluster.id = static_cast<std::int32_t>(next_probe_cluster_id_++);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] created cluster_id=" << cluster.id << " target_bps=" << bitrate
        << " duration_us=" << cluster.target_duration.count() << " packets=" << cluster.target_probe_count
        << " min_delta_us=" << cluster.min_probe_delta.count();
    return cluster;
}
std::vector<ProbeClusterConfig> ProbeController::InitiateProbing(Delta now, const std::vector<std::uint64_t>& rates, bool further)
{
    if (config_.skip_if_estimate_larger_than_fraction_of_max > 0)
    {
        const auto max_rate = max_total_allocated_bitrate_ == 0 ? max_bitrate_ :
            std::min(scaleRate(max_total_allocated_bitrate_, config_.skip_probe_max_allocated_scale), max_bitrate_);
        // An existing estimate without an upper value maps to upstream's
        // default MinusInfinity, so it cannot satisfy this skip comparison.
        std::optional<std::uint64_t> estimate = estimated_bitrate_;
        if (network_estimate_)
            estimate = network_estimate_->link_capacity_upper ?
                std::optional<std::uint64_t>(std::min(estimated_bitrate_, *network_estimate_->link_capacity_upper)) : std::nullopt;
        if (estimate && *estimate > scaleRate(max_rate, config_.skip_if_estimate_larger_than_fraction_of_max))
        {
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] skipping probe near max estimated_bps="
                << *estimate << " max_bps=" << max_rate;
            UpdateState(State::kProbingComplete);
            return {};
        }
    }
    auto maximum = max_bitrate_;
    if (max_total_allocated_bitrate_ > 0) maximum = std::min(maximum, twiceRate(max_total_allocated_bitrate_));
    switch (bandwidth_limited_cause_)
    {
    case BandwidthLimitedCause::kRttBasedBackOffHighRtt:
    case BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased:
    case BandwidthLimitedCause::kLossLimitedBwe:
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController] probe suppressed, cause=" << static_cast<int>(bandwidth_limited_cause_);
        return {};
    case BandwidthLimitedCause::kLossLimitedBweIncreasing:
        maximum = std::min(maximum, scaleRate(estimated_bitrate_, config_.loss_limited_probe_scale));
        break;
    case BandwidthLimitedCause::kDelayBasedLimited: break;
    }
    if (config_.network_state_estimate_probing_interval && network_estimate_ && network_estimate_->link_capacity_upper)
    {
        const auto upper = *network_estimate_->link_capacity_upper;
        if (upper == 0)
        {
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] probe suppressed by zero network capacity";
            return {};
        }
        maximum = std::min(maximum, std::max(estimated_bitrate_, scaleRate(upper, config_.network_state_probe_scale)));
    }
    // Suppress zero-sized clusters, including floating-point underflow at very
    // small rates. Reserve id space before consuming any ids from the batch.
    std::vector<std::uint64_t> targets;
    for (auto bitrate : rates)
    {
        if (bitrate >= maximum) { bitrate = maximum; further = false; }
        if (bitrate > 0) targets.push_back(bitrate);
    }
    if (targets.empty())
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] no positive probe targets maximum_bps=" << maximum;
        return {};
    }
    if (targets.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max() - next_probe_cluster_id_ + 1))
    {
        UAV_LOG_ERROR << "[WebRTC][ProbeController][" << this << "] cluster id space exhausted next_id=" << next_probe_cluster_id_;
        throw std::overflow_error("probe cluster id space exhausted");
    }
    std::vector<ProbeClusterConfig> probes;
    probes.reserve(targets.size());
    for (auto target : targets) probes.push_back(CreateProbeClusterConfig(now, target));
    time_last_probing_initiated_ = now;
    UpdateState(further ? State::kWaitingForProbingResult : State::kProbingComplete);
    if (further) min_bitrate_to_probe_further_ = scaleRate(probes.back().target_data_rate, config_.further_probe_threshold);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeController][" << this << "] initiated clusters=" << probes.size() << " maximum_bps=" << maximum
        << " allow_further=" << further << " further_threshold_bps=" << min_bitrate_to_probe_further_.value_or(0);
    return probes;
}
ProbeController::Stats ProbeController::Snapshot() const
{
    Stats stats;
    stats.state = state_; stats.network_available = network_available_;
    stats.periodic_alr_probing = enable_periodic_alr_probing_; stats.repeated_initial_probing = repeated_initial_probing_enabled_;
    stats.estimated_bitrate = estimated_bitrate_; stats.start_bitrate = start_bitrate_; stats.max_bitrate = max_bitrate_;
    stats.max_total_allocated_bitrate = max_total_allocated_bitrate_;
    stats.min_bitrate_to_probe_further = min_bitrate_to_probe_further_;
    stats.time_last_probing_initiated = timeFromMicros(time_last_probing_initiated_);
    if (last_allowed_repeated_initial_probe_) stats.last_allowed_repeated_initial_probe = timeFromMicros(*last_allowed_repeated_initial_probe_);
    stats.next_probe_cluster_id = next_probe_cluster_id_; stats.bandwidth_limited_cause = bandwidth_limited_cause_;
    return stats;
}
} // namespace webrtc
