/*
 * Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/probe_bitrate_estimator.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/probe_bitrate_estimator.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
constexpr double kMinReceivedProbesRatio = .80;
constexpr double kMinReceivedBytesRatio = .80;
constexpr float kMaxValidRatio = 2.0f;
constexpr float kMinRatioForUnsaturatedLink = 0.9f;
constexpr float kTargetUtilizationFraction = 0.95f;
constexpr auto kMaxClusterHistory = std::chrono::seconds(1);
constexpr auto kMaxProbeInterval = std::chrono::seconds(1);

std::uint64_t scaleRate(std::uint64_t rate, double scale)
{
    const auto value = std::round(static_cast<double>(rate) * scale);
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return static_cast<long double>(value) >= static_cast<long double>(max) ? max : static_cast<std::uint64_t>(value);
}
std::uint64_t rateFromBytes(std::uint64_t bytes, std::chrono::microseconds interval)
{
    // Source DataSize / TimeDelta truncates integer microbits/us. Split the
    // product to avoid overflow even with the complete uint64_t byte domain.
    // The validated interval is 1..1,000,000 us, so remainder * 8,000,000 fits.
    const auto us = static_cast<std::uint64_t>(interval.count());
    const auto whole = bytes / us;
    const auto fractional = (bytes % us) * 8000000 / us;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return whole > (max - fractional) / 8000000 ? max : whole * 8000000 + fractional;
}
} // namespace

ProbeBitrateEstimator::ProbeBitrateEstimator() = default;
ProbeBitrateEstimator::~ProbeBitrateEstimator() = default;
std::optional<std::uint64_t> ProbeBitrateEstimator::HandleProbeAndEstimateBitrate(const PacketResult& packet)
{
    const auto& pacing = packet.sent_packet.pacing_info;
    const int id = pacing.probe_cluster_id;
    if (id < 0 || !packet.receive_time || !packet.sent_packet.send_time)
    {
        UAV_LOG_IF(LogLevel::TRACE, LogManager::getInstance().shouldLog(LogLevel::TRACE)) << "[WebRTC][ProbeBitrateEstimator][" << this << "] ignored seq=" << packet.sent_packet.sequence_number
            << " cluster_id=" << id << " received=" << packet.IsReceived() << " send_time_available=" << packet.sent_packet.send_time.has_value();
        return std::nullopt;
    }
    if (pacing.probe_cluster_min_probes <= 0 || pacing.probe_cluster_min_bytes <= 0 ||
        packet.sent_packet.send_time->time_since_epoch() < TransportTime::duration::zero() ||
        packet.receive_time->time_since_epoch() < TransportTime::duration::zero())
    {
        UAV_LOG_WARN << "[WebRTC][ProbeBitrateEstimator][" << this << "] invalid metadata or timestamp cluster_id=" << id
            << " min_probes=" << pacing.probe_cluster_min_probes << " min_bytes=" << pacing.probe_cluster_min_bytes;
        throw std::invalid_argument("invalid probe metadata or packet timestamp");
    }
    const auto sent = std::chrono::duration_cast<Timestamp>(packet.sent_packet.send_time->time_since_epoch());
    const auto received = std::chrono::duration_cast<Timestamp>(packet.receive_time->time_since_epoch());
    const auto size = static_cast<std::uint64_t>(packet.sent_packet.size);
    const auto previous = clusters_.find(id);
    if (previous != clusters_.end() && received - previous->second.last_receive <= kMaxClusterHistory &&
        (size > std::numeric_limits<std::uint64_t>::max() - previous->second.size_total ||
         previous->second.num_probes == std::numeric_limits<std::uint64_t>::max()))
    {
        UAV_LOG_WARN << "[WebRTC][ProbeBitrateEstimator][" << this << "] cluster counters overflow cluster_id=" << id
            << " total_bytes=" << previous->second.size_total << " packet_bytes=" << size;
        throw std::overflow_error("probe cluster counters overflow");
    }

    EraseOldClusters(received);
    auto& cluster = clusters_[id];
    const bool first = cluster.num_probes == 0;
    if (first || sent < cluster.first_send) cluster.first_send = sent;
    if (first || sent > cluster.last_send)
    {
        cluster.last_send = sent;
        cluster.size_last_send = size;
    }
    if (first || received < cluster.first_receive)
    {
        cluster.first_receive = received;
        cluster.size_first_receive = size;
    }
    if (first || received > cluster.last_receive) cluster.last_receive = received;
    cluster.size_total += size;
    ++cluster.num_probes;

    // Upstream deliberately uses different rounding rules for the two gates.
    const auto min_probes = static_cast<std::uint64_t>(pacing.probe_cluster_min_probes * kMinReceivedProbesRatio);
    const auto min_bytes = scaleRate(static_cast<std::uint64_t>(pacing.probe_cluster_min_bytes), kMinReceivedBytesRatio);
    UAV_LOG_IF(LogLevel::TRACE, LogManager::getInstance().shouldLog(LogLevel::TRACE)) << "[WebRTC][ProbeBitrateEstimator][" << this << "] cluster_id=" << id << " probes=" << cluster.num_probes
        << " required_probes=" << min_probes << " total_bytes=" << cluster.size_total << " required_bytes=" << min_bytes
        << " send_us=" << sent.count() << " receive_us=" << received.count();
    if (cluster.num_probes < min_probes || cluster.size_total < min_bytes) return std::nullopt;

    const auto send_interval = cluster.last_send - cluster.first_send;
    const auto receive_interval = cluster.last_receive - cluster.first_receive;
    if (send_interval <= Timestamp::zero() || send_interval > kMaxProbeInterval ||
        receive_interval <= Timestamp::zero() || receive_interval > kMaxProbeInterval)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeBitrateEstimator] invalid interval cluster_id=" << id
            << " send_us=" << send_interval.count() << " receive_us=" << receive_interval.count();
        return std::nullopt;
    }
    const auto send_bytes = cluster.size_total - cluster.size_last_send;
    const auto receive_bytes = cluster.size_total - cluster.size_first_receive;
    if (send_bytes == 0 || receive_bytes == 0)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeBitrateEstimator] empty measurement cluster_id=" << id;
        return std::nullopt;
    }
    const auto send_rate = rateFromBytes(send_bytes, send_interval);
    const auto receive_rate = rateFromBytes(receive_bytes, receive_interval);
    const double ratio = static_cast<double>(receive_rate) / static_cast<double>(send_rate);
    if (ratio > kMaxValidRatio)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeBitrateEstimator] invalid receive/send ratio cluster_id=" << id
            << " send_bps=" << send_rate << " receive_bps=" << receive_rate << " ratio=" << ratio;
        return std::nullopt;
    }
    auto result = std::min(send_rate, receive_rate);
    // Keep the original float constants before their promotion to double.
    if (receive_rate < scaleRate(send_rate, kMinRatioForUnsaturatedLink))
        result = scaleRate(receive_rate, kTargetUtilizationFraction);
    estimated_data_rate_ = result;
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeBitrateEstimator] successful cluster_id=" << id
        << " estimate_bps=" << result << " send_bps=" << send_rate << " receive_bps=" << receive_rate;
    return result;
}
std::optional<std::uint64_t> ProbeBitrateEstimator::FetchAndResetLastEstimatedBitrate()
{
    const auto estimate = estimated_data_rate_;
    if (estimate) UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeBitrateEstimator][" << this << "] fetched estimate_bps=" << *estimate
        << " retained_clusters=" << clusters_.size();
    estimated_data_rate_.reset();
    return estimate;
}
void ProbeBitrateEstimator::EraseOldClusters(Timestamp timestamp)
{
    for (auto it = clusters_.begin(); it != clusters_.end();)
    {
        if (timestamp - it->second.last_receive > kMaxClusterHistory)
        {
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][ProbeBitrateEstimator][" << this << "] expired cluster_id=" << it->first
                << " age_us=" << (timestamp - it->second.last_receive).count() << " probes=" << it->second.num_probes;
            it = clusters_.erase(it);
        }
        else ++it;
    }
}
ProbeBitrateEstimator::Stats ProbeBitrateEstimator::Snapshot() const { return {clusters_.size(), estimated_data_rate_}; }
} // namespace webrtc
