// Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
// Adapted from libwebrtc 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
#include"webrtc/congestion_control/bitrate_prober.h"
#include"webrtc/congestion_control/rate_math.h"
#include"log/log.h"
#include<algorithm>
#include<limits>
#include<stdexcept>

namespace webrtc
{
BitrateProber::BitrateProber() : BitrateProber(Config{}) {}
BitrateProber::BitrateProber(Config config) : config_(config)
{ if (config.max_probe_delay.count() < 0) throw std::invalid_argument("negative probe delay"); }
void BitrateProber::SetEnabled(bool enabled)
{ if (!enabled) state_ = State::Disabled; else if (state_ == State::Disabled) state_ = State::Inactive; }
void BitrateProber::SetAllowProbeWithoutMediaPacket(bool allow)
{ config_.allow_start_probing_immediately = allow; MaybeActivate(0); }
bool BitrateProber::is_probing() const { return state_ == State::Active; }
void BitrateProber::MaybeActivate(std::size_t bytes)
{
    if (state_ == State::Inactive && !clusters_.empty() &&
        (config_.allow_start_probing_immediately || bytes >= std::min(RecommendedMinProbeSize(), config_.min_packet_size)))
    { next_ = Time{}; state_ = State::Active; }
}
void BitrateProber::OnIncomingPacket(std::size_t bytes) { MaybeActivate(bytes); }
void BitrateProber::CreateProbeCluster(const ProbeClusterConfig& c)
{
    const auto min_bytes = cc_detail::bytesForRate(c.target_data_rate, c.target_duration);
    if (state_ == State::Disabled || !c.target_data_rate || c.target_probe_count <= 0 || c.id < 0 ||
        c.at_time < Time{} || c.min_probe_delta.count() <= 0 || min_bytes == 0 || min_bytes > std::numeric_limits<int>::max())
        throw std::invalid_argument("invalid probe cluster");
    // Upstream checks >5 before insertion; therefore at most six requests remain.
    while (!clusters_.empty() && (c.at_time - clusters_.front().requested > std::chrono::seconds(5) || clusters_.size() > 5)) clusters_.pop_front();
    Cluster cluster; cluster.requested = c.at_time; cluster.min_delta = c.min_probe_delta;
    cluster.pacing = {c.id, c.target_probe_count, static_cast<int>(min_bytes), c.target_data_rate};
    clusters_.push_back(cluster); MaybeActivate(0);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateProber] queued id="
        << c.id << " rate_bps=" << c.target_data_rate << " min_bytes=" << min_bytes << " clusters=" << clusters_.size();
}
std::optional<TransportTime> BitrateProber::NextProbeTime(Time) const
{ return is_probing() && !clusters_.empty() ? next_ : std::nullopt; }
std::optional<PacedPacketInfo> BitrateProber::CurrentCluster(Time now)
{
    if (!is_probing() || clusters_.empty()) return std::nullopt;
    if (next_ && *next_ != Time{} && now - *next_ > config_.max_probe_delay)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateProber] late probe discarded id=" << clusters_.front().pacing.probe_cluster_id;
        clusters_.pop_front();
        if (clusters_.empty()) { state_ = State::Inactive; return std::nullopt; }
    }
    auto info = clusters_.front().pacing; info.probe_cluster_bytes_sent = clusters_.front().sent_bytes; return info;
}
std::size_t BitrateProber::RecommendedMinProbeSize() const
{ return clusters_.empty() ? 0 : cc_detail::bytesForRate(clusters_.front().pacing.send_bitrate_bps, clusters_.front().min_delta); }
void BitrateProber::ProbeSent(Time now, std::size_t bytes)
{
    if (!is_probing() || clusters_.empty() || !bytes || now < Time{}) throw std::invalid_argument("probe not active, invalid time or empty burst");
    auto& c = clusters_.front();
    if (bytes > std::numeric_limits<std::uint64_t>::max() - c.sent_bytes) throw std::overflow_error("probe byte count overflow");
    if (!c.started) c.started = now;
    c.sent_bytes += bytes; ++c.sent_probes;
    const auto us = cc_detail::bounded(static_cast<long double>(c.sent_bytes) * 8000000 / c.pacing.send_bitrate_bps);
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(Time::max() - *c.started).count();
    next_ = us > static_cast<std::uint64_t>(left) ? Time::max() : *c.started + std::chrono::microseconds(us);
    if (c.sent_bytes >= static_cast<std::uint64_t>(c.pacing.probe_cluster_min_bytes) && c.sent_probes >= static_cast<std::uint64_t>(c.pacing.probe_cluster_min_probes))
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][BitrateProber] completed id="
            << c.pacing.probe_cluster_id << " bytes=" << c.sent_bytes << " bursts=" << c.sent_probes;
        clusters_.pop_front();
    }
    if (clusters_.empty()) state_ = State::Inactive;
}
void BitrateProber::Reset() { clusters_.clear(); next_.reset(); state_ = State::Inactive; }
} // namespace webrtc
