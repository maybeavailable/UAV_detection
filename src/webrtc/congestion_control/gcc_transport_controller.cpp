#include"webrtc/congestion_control/gcc_transport_controller.h"
#include"log/log.h"
#include<algorithm>
#include<cmath>
#include<stdexcept>

namespace webrtc
{
GccTransportController::GccTransportController(GccTransportConfig config) : config_(std::move(config)), prober_(config_.prober)
{
    if (!config_.start_bitrate || !config_.min_bitrate || (config_.max_bitrate && !*config_.max_bitrate)) throw std::invalid_argument("invalid GCC transport bitrate limits");
    SendSideBandwidthEstimation::ValidateRate(config_.start_bitrate); SendSideBandwidthEstimation::ValidateRate(config_.min_bitrate);
    if (config_.max_bitrate) SendSideBandwidthEstimation::ValidateRate(*config_.max_bitrate);
    if (config_.streams.pacing_factor && (!std::isfinite(*config_.streams.pacing_factor) || *config_.streams.pacing_factor <= 0))
        throw std::invalid_argument("invalid GCC transport pacing factor");
    for (const auto rate : {config_.streams.min_total_allocated_bitrate, config_.streams.max_padding_rate, config_.streams.max_total_allocated_bitrate})
        if (rate) SendSideBandwidthEstimation::ValidateRate(*rate);
}
TransportTime GccTransportController::observe(Time now)
{ if (last_time_ && now < *last_time_) now = *last_time_; last_time_ = now; return now; }
NetworkControlUpdate GccTransportController::record(NetworkControlUpdate update)
{
    if (update.target_rate) stats_.target = update.target_rate;
    if (update.pacer_config) stats_.pacing = update.pacer_config;
    if (update.congestion_window) stats_.congestion_window = update.congestion_window;
    for (const auto& cluster : update.probe_cluster_configs) if (probing_supported_)
    {
        try { prober_.CreateProbeCluster(cluster); ++stats_.requested_probes; }
        catch (const std::invalid_argument&) { UAV_LOG_WARN << "[WebRTC][GCC Transport] rejected probe id=" << cluster.id; }
    }
    return update;
}
NetworkControlUpdate GccTransportController::Start(std::uint64_t generation, bool probing, Time now)
{
    Stop(); stats_ = {}; stats_.generation = generation; last_time_ = now; probing_supported_ = probing;
    NetworkControllerConfig config; config.constraints = {now, config_.min_bitrate, config_.max_bitrate, config_.start_bitrate};
    config.stream_based_config = config_.streams; config.stream_based_config.at_time = now;
    controller_ = std::make_unique<GoogCcNetworkController>(config, config_.make_algorithm_config ? config_.make_algorithm_config() : GoogCcConfig{});
    stats_.active = true;
    auto available = controller_->OnNetworkAvailability({now, true});
    auto update = controller_->OnProcessInterval({now, {}});
    update.probe_cluster_configs.insert(update.probe_cluster_configs.begin(), available.probe_cluster_configs.begin(), available.probe_cluster_configs.end());
    return record(std::move(update));
}
void GccTransportController::Stop()
{ controller_.reset(); prober_.Reset(); reports_.clear(); last_time_.reset(); stats_.active = false; }
void GccTransportController::Reset(std::uint64_t generation)
{ Stop(); stats_ = {}; stats_.generation = generation; }
NetworkControlUpdate GccTransportController::OnSentPacket(SentPacket packet, Time now)
{
    if (!controller_) return {};
    packet.send_time = observe(now); ++stats_.sent_packets;
    return record(controller_->OnSentPacket(std::move(packet)));
}
NetworkControlUpdate GccTransportController::OnFeedback(TransportPacketsFeedback feedback, Time now)
{
    if (!controller_ || feedback.generation != stats_.generation) return {};
    feedback.feedback_time = observe(now); ++stats_.feedback_packets;
    return record(controller_->OnTransportPacketsFeedback(std::move(feedback)));
}
NetworkControlUpdate GccTransportController::OnReport(const RtcpReportEvent& event, Time now)
{
    if (!controller_ || event.generation != stats_.generation) return {};
    now = observe(now); const auto& r = event.report;
    if (event.rttMs && std::isfinite(*event.rttMs) && *event.rttMs >= 0 && *event.rttMs <= 60000)
        (void)controller_->OnRoundTripTimeUpdate({now, std::chrono::microseconds(std::llround(*event.rttMs * 1000)), false});
    const auto key = std::make_pair(event.reporterSsrc, r.ssrc); const auto previous = reports_.find(key);
    if (previous != reports_.end())
    {
        const auto count = std::uint32_t(r.highestSequence - previous->second.highest);
        if (count >= 0x80000000u) return {}; // Reordered RR must not move the baseline backwards and recount packets.
        if (count > 0 && count < 0x80000000u)
        {
            const auto lost = std::clamp<std::int64_t>(std::int64_t(r.cumulativeLost) - previous->second.lost, 0, count);
            TransportLossReport report; report.receive_time = now; report.packets_lost_delta = lost; report.packets_received_delta = count - lost;
            (void)controller_->OnTransportLossReport(report);
        }
    }
    // Bounded even when many authenticated reporters use different SSRCs.
    if (previous != reports_.end() || reports_.size() < 256) reports_[key] = {r.highestSequence, r.cumulativeLost};
    return {};
}
NetworkControlUpdate GccTransportController::OnRemoteBitrate(std::uint64_t rate, Time now)
{ return controller_ ? record(controller_->OnRemoteBitrateReport({observe(now), rate})) : NetworkControlUpdate{}; }
NetworkControlUpdate GccTransportController::OnNetworkAvailability(bool available, Time now)
{
    if (!controller_) return {};
    stats_.network_available = available;
    if (!available) prober_.Reset();
    return record(controller_->OnNetworkAvailability({observe(now), available}));
}
NetworkControlUpdate GccTransportController::Process(Time now, std::size_t queue, std::size_t outstanding)
{
    if (!controller_) return {};
    controller_->UpdateOutstandingData(outstanding);
    ++stats_.process_ticks; return record(controller_->OnProcessInterval({observe(now), queue}));
}
NetworkControlUpdate GccTransportController::SetConstraints(TargetRateConstraints c, Time now)
{
    c.at_time = observe(now);
    if (!controller_) throw std::logic_error("GCC is not active");
    auto update = record(controller_->OnTargetRateConstraints(c));
    config_.min_bitrate = c.min_data_rate.value_or(5000); config_.max_bitrate = c.max_data_rate;
    if (c.starting_rate) config_.start_bitrate = *c.starting_rate;
    return update;
}
NetworkControlUpdate GccTransportController::SetStreams(StreamsConfig s, Time now)
{
    s.at_time = observe(now);
    if (!controller_) throw std::logic_error("GCC is not active");
    auto update = record(controller_->OnStreamsConfig(s));
    if (s.requests_alr_probing) config_.streams.requests_alr_probing = s.requests_alr_probing;
    if (s.pacing_factor) config_.streams.pacing_factor = s.pacing_factor;
    if (s.min_total_allocated_bitrate) config_.streams.min_total_allocated_bitrate = s.min_total_allocated_bitrate;
    if (s.max_total_allocated_bitrate) config_.streams.max_total_allocated_bitrate = s.max_total_allocated_bitrate;
    if (s.max_padding_rate) config_.streams.max_padding_rate = s.max_padding_rate;
    return update;
}
NetworkControlUpdate GccTransportController::SetNetworkEstimate(NetworkStateEstimate estimate, Time now)
{
    if (!controller_) throw std::logic_error("GCC is not active");
    if (!estimate.update_time) estimate.update_time = observe(now);
    return record(controller_->OnNetworkStateEstimate(estimate));
}
GccTransportSnapshot GccTransportController::snapshot() const
{ auto result = stats_; result.probing = prober_.is_probing(); result.pending_probe_clusters = prober_.pending_clusters(); return result; }
} // namespace webrtc
