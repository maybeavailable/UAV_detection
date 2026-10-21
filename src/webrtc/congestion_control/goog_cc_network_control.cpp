// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// Adapted from libwebrtc 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
// See third_party/webrtc/LICENSE, PATENTS and NOTICE.
#include"webrtc/congestion_control/goog_cc_network_control.h"
#include"webrtc/congestion_control/probe_bitrate_estimator.h"
#include"webrtc/congestion_control/rate_math.h"
#include"log/log.h"
#include<algorithm>
#include<deque>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
using Time = TransportTime;
using Delta = std::chrono::microseconds;
Delta micros(Time time) { return std::chrono::duration_cast<Delta>(time.time_since_epoch()); }
void validateTime(Time time)
{ if (time < Time{}) throw std::invalid_argument("negative GoogCC timestamp"); }
void validateConstraints(const TargetRateConstraints& c)
{
    validateTime(c.at_time);
    for (const auto rate : {c.min_data_rate, c.max_data_rate, c.starting_rate})
        if (rate) SendSideBandwidthEstimation::ValidateRate(*rate);
    if (c.starting_rate && !*c.starting_rate) throw std::invalid_argument("zero GoogCC starting bitrate");
}
void validateStreams(const StreamsConfig& c)
{
    validateTime(c.at_time);
    if (c.pacing_factor && (!std::isfinite(*c.pacing_factor) || *c.pacing_factor <= 0))
        throw std::invalid_argument("invalid GoogCC pacing factor");
    for (const auto rate : {c.min_total_allocated_bitrate, c.max_total_allocated_bitrate, c.max_padding_rate})
        if (rate) SendSideBandwidthEstimation::ValidateRate(*rate);
}
BandwidthLimitedCause limitedCause(LossBasedState loss, bool high_rtt, BandwidthUsage usage)
{
    if (usage == BandwidthUsage::kBwOverusing || usage == BandwidthUsage::kBwUnderusing)
        return BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased;
    if (high_rtt) return BandwidthLimitedCause::kRttBasedBackOffHighRtt;
    if (loss == LossBasedState::kDecreasing || loss == LossBasedState::kIncreaseUsingPadding) return BandwidthLimitedCause::kLossLimitedBwe;
    return loss == LossBasedState::kIncreasing ? BandwidthLimitedCause::kLossLimitedBweIncreasing : BandwidthLimitedCause::kDelayBasedLimited;
}
void append(NetworkControlUpdate& update, std::vector<ProbeClusterConfig> probes)
{ update.probe_cluster_configs.insert(update.probe_cluster_configs.end(), probes.begin(), probes.end()); }
}
struct GoogCcNetworkController::Impl
{
    GoogCcConfig config;
    Time event_time{};
    std::optional<Time> last_event_time;
    ProbeController probes;
    std::unique_ptr<CongestionWindowPushbackController> pushback;
    SendSideBandwidthEstimation bandwidth;
    AlrDetector alr;
    std::unique_ptr<ProbeBitrateEstimator> probe_bitrate;
    std::unique_ptr<DelayBasedBwe> delay;
    std::unique_ptr<AcknowledgedBitrateEstimator> acknowledged;
    std::optional<NetworkControllerConfig> initial;
    std::optional<StreamsConfig> initial_streams;
    std::uint64_t min_target{0}, min_data{0};
    std::optional<std::uint64_t> max_data, starting;
    bool first_sent{false}, first_feedback{false}, previously_in_alr{false}, last_bandwidth_limited{true};
    std::optional<NetworkStateEstimate> estimate;
    std::deque<std::int64_t> feedback_max_rtts;
    std::uint64_t last_target{0}, last_pushback_target{0};
    LossBasedState last_loss_state{LossBasedState::kDelayBasedEstimate};
    std::uint8_t last_fraction_loss{0};
    std::optional<Delta> last_rtt;
    std::optional<double> pacing_factor;
    Delta pacing_window;
    std::uint64_t min_allocated{0}, max_padding{0};
    std::optional<std::size_t> data_window;
    std::optional<std::uint64_t> generation;

    Impl(NetworkControllerConfig c, GoogCcConfig g)
        : config(std::move(g)), event_time(c.constraints.at_time), probes(config.probing),
          pushback(config.use_congestion_window && config.use_congestion_window_pushback ?
              std::make_unique<CongestionWindowPushbackController>(config.pushback) : nullptr),
          bandwidth(config.bandwidth), alr(config.alr, [this] { return event_time; }),
          probe_bitrate(std::make_unique<ProbeBitrateEstimator>()),
          delay(std::make_unique<DelayBasedBwe>(config.delay, config.network_state_predictor.get())),
          acknowledged(std::make_unique<AcknowledgedBitrateEstimator>(config.acknowledged)), initial(c), initial_streams(c.stream_based_config),
          pacing_factor(c.stream_based_config.pacing_factor), pacing_window(c.default_pacing_time_window),
          min_allocated(c.stream_based_config.min_total_allocated_bitrate.value_or(0)), max_padding(c.stream_based_config.max_padding_rate.value_or(0))
    {
        validateConstraints(c.constraints); validateStreams(c.stream_based_config);
        if (!c.constraints.starting_rate) throw std::invalid_argument("GoogCC requires initial starting_rate");
        if (pacing_window < Delta::zero() || pacing_window > std::chrono::seconds(1) ||
            config.congestion_window_additional_time < Delta::zero() || config.congestion_window_additional_time > std::chrono::hours(1))
            throw std::invalid_argument("invalid GoogCC pacing or congestion time window");
        last_target = last_pushback_target = *c.constraints.starting_rate;
        delay->SetMinBitrate(5000);
        UAV_LOG_INFO << "[WebRTC][GoogCC][" << this << "] initialized start_bps=" << last_target
            << " congestion_window=" << config.use_congestion_window << " pushback=" << bool(pushback) << " drop_frame_only=" << config.drop_frame_only;
    }
    void observe(Time at)
    {
        validateTime(at);
        if ((last_event_time && at < *last_event_time) || (initial && at < initial->constraints.at_time))
            throw std::invalid_argument("GoogCC event timestamps moved backwards");
        last_event_time = event_time = at;
    }
    void clampConstraints()
    {
        min_data = std::max<std::uint64_t>(min_target, 5000);
        if (config.use_min_allocatable_as_lower_bound) min_data = std::max(min_data, min_allocated);
        if (max_data && *max_data < min_data) max_data = min_data;
        if (starting && *starting < min_data) starting = min_data;
    }
    std::vector<ProbeClusterConfig> resetConstraints(TargetRateConstraints c)
    {
        min_target = c.min_data_rate.value_or(0); max_data = c.max_data_rate; starting = c.starting_rate;
        clampConstraints(); bandwidth.SetBitrates(starting, min_data, max_data, c.at_time);
        if (starting) delay->SetStartBitrate(*starting);
        delay->SetMinBitrate(min_data);
        return probes.SetBitrates(min_data, starting.value_or(0), max_data, c.at_time);
    }
    PacerConfig pacing(Time at) const
    {
        auto rate = cc_detail::scale(std::max(min_allocated, last_target), pacing_factor.value_or(first_feedback ? double(1.1f) : double(2.5f)));
        if (config.limit_pacing_factor_by_upper_link_capacity && estimate && estimate->link_capacity_upper && rate > *estimate->link_capacity_upper)
            rate = std::max({*estimate->link_capacity_upper, min_allocated, last_target});
        auto padding = last_loss_state == LossBasedState::kIncreaseUsingPadding ? std::max(max_padding, last_target) : max_padding;
        padding = std::min(padding, last_pushback_target);
        return PacerConfig::Create(at, rate, padding, pacing_window);
    }
    void maybeChanged(NetworkControlUpdate& update, Time at)
    {
        const auto fraction = bandwidth.fraction_loss(); const auto rtt = bandwidth.round_trip_time();
        const auto target = bandwidth.target_rate(); const auto state = bandwidth.loss_based_state();
        auto reduced = target; double reduction = 0;
        if (pushback)
        {
            reduced = std::max(bandwidth.GetMinBitrate(), pushback->UpdateTargetBitrate(target));
            if (config.drop_frame_only) reduction = static_cast<double>(target - reduced) / target;
        }
        const bool limited = !alr.GetApplicationLimitedRegionStartTime();
        if (target == last_target && reduced == last_pushback_target && state == last_loss_state &&
            fraction == last_fraction_loss && last_rtt && rtt == *last_rtt && limited == last_bandwidth_limited) return;
        last_target = target; last_pushback_target = reduced; last_loss_state = state;
        last_fraction_loss = fraction; last_rtt = rtt; last_bandwidth_limited = limited;
        alr.SetEstimatedBitrate(target);
        TargetTransferRate result;
        result.at_time = result.network_estimate.at_time = at;
        result.target_rate = pushback && config.drop_frame_only ? target : reduced;
        result.cwnd_reduce_ratio = reduction;
        result.network_estimate.round_trip_time = rtt;
        result.network_estimate.loss_rate_ratio = fraction / 255.0f;
        result.is_bandwidth_limited = limited;
        update.target_rate = result;
        append(update, probes.SetEstimatedBitrate(target, limitedCause(state, bandwidth.IsRttAboveLimit(), delay->last_state()), at));
        update.pacer_config = pacing(at);
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][GoogCC][" << this
            << "] target_bps=" << result.target_rate << " loss_target_bps=" << target << " pushback_bps=" << reduced
            << " cwnd_reduce_ratio=" << reduction << " loss_state=" << static_cast<int>(state)
            << " loss_q8=" << unsigned(fraction) << " rtt_us=" << rtt.count() << " in_alr=" << !limited;
    }
    void updateWindow()
    {
        const auto min_rtt_ms = *std::min_element(feedback_max_rtts.begin(), feedback_max_rtts.end());
        const auto window = std::chrono::milliseconds(min_rtt_ms) + config.congestion_window_additional_time;
        auto next = cc_detail::bytesForRate(last_target, window);
        if (data_window) next = next / 2 + *data_window / 2 + (next % 2 + *data_window % 2) / 2;
        data_window = std::max<std::uint64_t>(3000, next);
        UAV_LOG_IF(LogLevel::TRACE, LogManager::getInstance().shouldLog(LogLevel::TRACE)) << "[WebRTC][GoogCC][" << this
            << "] congestion_window_bytes=" << *data_window << " min_feedback_rtt_ms=" << min_rtt_ms;
    }
    void publishWindow(NetworkControlUpdate& update)
    { if (pushback && data_window) pushback->SetDataWindow(*data_window); else update.congestion_window = data_window; }
    void setEstimate(std::optional<NetworkStateEstimate> value)
    {
        const auto previous = estimate; estimate = value;
        if (estimate && (!previous || estimate->update_time != previous->update_time)) probes.SetNetworkStateEstimate(*estimate);
    }
};
GoogCcNetworkController::GoogCcNetworkController(NetworkControllerConfig c, GoogCcConfig g)
    : impl_(std::make_unique<Impl>(std::move(c), std::move(g))) {}
GoogCcNetworkController::~GoogCcNetworkController() = default;
void GoogCcNetworkController::UpdateOutstandingData(std::size_t bytes)
{ if (impl_->pushback) impl_->pushback->UpdateOutstandingData(bytes); }
NetworkControlUpdate GoogCcNetworkController::OnNetworkAvailability(NetworkAvailability msg)
{
    auto& s = *impl_; s.observe(msg.at_time); NetworkControlUpdate update;
    update.probe_cluster_configs = s.probes.OnNetworkAvailability(msg); return update;
}
NetworkControlUpdate GoogCcNetworkController::OnProcessInterval(ProcessInterval msg)
{
    auto& s = *impl_; s.observe(msg.at_time); NetworkControlUpdate update;
    if (s.initial)
    {
        auto constraints = s.initial->constraints; constraints.at_time = msg.at_time;
        append(update, s.resetConstraints(constraints)); update.pacer_config = s.pacing(msg.at_time);
        s.initial.reset();
    }
    if (s.initial_streams)
    {
        const auto& streams = *s.initial_streams;
        if (streams.requests_alr_probing) s.probes.EnablePeriodicAlrProbing(*streams.requests_alr_probing);
        s.probes.EnableRepeatedInitialProbing(streams.enable_repeated_initial_probing);
        if (streams.max_total_allocated_bitrate) append(update, s.probes.OnMaxTotalAllocatedBitrate(*streams.max_total_allocated_bitrate, msg.at_time));
        s.initial_streams.reset();
    }
    if (s.pushback && msg.pacer_queue) s.pushback->UpdatePacingQueue(*msg.pacer_queue);
    s.bandwidth.UpdateEstimate(msg.at_time);
    s.probes.SetAlrStartTime(s.first_feedback ? s.alr.GetApplicationLimitedRegionStartTime() : std::nullopt);
    append(update, s.probes.Process(msg.at_time));
    if (s.config.use_congestion_window && !s.feedback_max_rtts.empty()) s.updateWindow();
    s.publishWindow(update); s.maybeChanged(update, msg.at_time); return update;
}
NetworkControlUpdate GoogCcNetworkController::OnNetworkRouteChange(NetworkRouteChange msg)
{
    auto& s = *impl_; validateConstraints(msg.constraints); s.observe(msg.at_time);
    if (s.config.safe_reset_on_route_change)
    {
        std::optional<std::uint64_t> estimated;
        if (s.config.safe_reset_acknowledged_rate)
        { estimated = s.acknowledged->bitrate(); if (!estimated) estimated = s.acknowledged->PeekRate(); }
        else estimated = s.bandwidth.target_rate();
        if (estimated) msg.constraints.starting_rate = msg.constraints.starting_rate ? std::min(*msg.constraints.starting_rate, *estimated) : *estimated;
    }
    if (s.config.network_state_estimator) s.config.network_state_estimator->OnRouteChange(msg);
    s.acknowledged = std::make_unique<AcknowledgedBitrateEstimator>(s.config.acknowledged);
    s.probe_bitrate = std::make_unique<ProbeBitrateEstimator>();
    s.delay = std::make_unique<DelayBasedBwe>(s.config.delay, s.config.network_state_predictor.get());
    s.bandwidth.OnRouteChange(); s.probes.Reset(msg.at_time);
    s.generation = msg.generation;
    // Discard deferred startup constraints so the first timer cannot restore the old route.
    s.initial.reset(); msg.constraints.at_time = msg.at_time;
    NetworkControlUpdate update; append(update, s.resetConstraints(msg.constraints)); s.maybeChanged(update, msg.at_time);
    UAV_LOG_INFO << "[WebRTC][GoogCC][" << &s << "] route changed generation_available=" << s.generation.has_value()
        << " generation=" << s.generation.value_or(0);
    return update;
}
NetworkControlUpdate GoogCcNetworkController::OnRemoteBitrateReport(RemoteBitrateReport msg)
{ auto& s = *impl_; s.observe(msg.receive_time); s.bandwidth.UpdateReceiverEstimate(msg.receive_time, msg.bandwidth); return {}; }
NetworkControlUpdate GoogCcNetworkController::OnRoundTripTimeUpdate(RoundTripTimeUpdate msg)
{
    if (msg.round_trip_time < Delta::zero() ||
        msg.round_trip_time.count() > std::numeric_limits<std::int64_t>::max() / 2 - 100000)
        throw std::invalid_argument("invalid GoogCC RTT");
    auto& s = *impl_; s.observe(msg.receive_time); if (msg.smoothed) return {};
    s.delay->OnRttUpdate(msg.round_trip_time); s.bandwidth.UpdateRtt(msg.round_trip_time, msg.receive_time); return {};
}
NetworkControlUpdate GoogCcNetworkController::OnSentPacket(SentPacket msg)
{
    if (!msg.send_time) throw std::invalid_argument("GoogCC sent packet has no send time");
    auto& s = *impl_; s.observe(*msg.send_time);
    s.alr.OnBytesSent(msg.size, *msg.send_time); s.acknowledged->SetAlr(s.alr.GetApplicationLimitedRegionStartTime().has_value());
    if (!s.first_sent) { s.first_sent = true; s.bandwidth.UpdatePropagationRtt(*msg.send_time, Delta::zero()); }
    s.bandwidth.OnSentPacket(msg);
    NetworkControlUpdate update;
    if (s.pushback) { s.pushback->UpdateOutstandingData(msg.data_in_flight); s.maybeChanged(update, *msg.send_time); }
    return update;
}
NetworkControlUpdate GoogCcNetworkController::OnReceivedPacket(ReceivedPacket msg)
{ impl_->observe(msg.receive_time); return {}; }
NetworkControlUpdate GoogCcNetworkController::OnStreamsConfig(StreamsConfig msg)
{
    validateStreams(msg); auto& s = *impl_; s.observe(msg.at_time); NetworkControlUpdate update;
    if (msg.requests_alr_probing) s.probes.EnablePeriodicAlrProbing(*msg.requests_alr_probing);
    if (msg.max_total_allocated_bitrate) append(update, s.probes.OnMaxTotalAllocatedBitrate(*msg.max_total_allocated_bitrate, msg.at_time));
    bool changed = false;
    if (msg.pacing_factor && msg.pacing_factor != s.pacing_factor) { s.pacing_factor = msg.pacing_factor; changed = true; }
    if (msg.min_total_allocated_bitrate && *msg.min_total_allocated_bitrate != s.min_allocated)
    {
        s.min_allocated = *msg.min_total_allocated_bitrate; changed = true;
        if (s.config.use_min_allocatable_as_lower_bound)
        { s.clampConstraints(); s.delay->SetMinBitrate(s.min_data); s.bandwidth.SetMinMaxBitrate(s.min_data, s.max_data); }
    }
    if (msg.max_padding_rate && *msg.max_padding_rate != s.max_padding) { s.max_padding = *msg.max_padding_rate; changed = true; }
    if (changed) update.pacer_config = s.pacing(msg.at_time);
    return update;
}
NetworkControlUpdate GoogCcNetworkController::OnTargetRateConstraints(TargetRateConstraints msg)
{
    validateConstraints(msg); auto& s = *impl_; s.observe(msg.at_time); s.initial.reset();
    NetworkControlUpdate update; append(update, s.resetConstraints(msg)); s.maybeChanged(update, msg.at_time); return update;
}
NetworkControlUpdate GoogCcNetworkController::OnTransportLossReport(TransportLossReport msg)
{
    constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (msg.packets_lost_delta > max || msg.packets_received_delta > max - msg.packets_lost_delta)
        throw std::overflow_error("GoogCC loss report counters overflow");
    auto& s = *impl_; s.observe(msg.receive_time);
    s.bandwidth.UpdatePacketsLost(static_cast<std::int64_t>(msg.packets_lost_delta), msg.packets_lost_delta + msg.packets_received_delta, msg.receive_time);
    return {};
}
NetworkControlUpdate GoogCcNetworkController::OnTransportPacketsFeedback(TransportPacketsFeedback report)
{
    auto& s = *impl_; NetworkControlUpdate update;
    if (report.packet_feedbacks.empty()) return update;
    if (s.generation && *s.generation != report.generation)
    {
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][GoogCC][" << &s
            << "] ignored feedback generation=" << report.generation << " expected_generation=" << *s.generation;
        return update;
    }
    validateTime(report.feedback_time);
    for (const auto& p : report.packet_feedbacks)
    {
        if (!p.sent_packet.send_time || *p.sent_packet.send_time < Time{} || *p.sent_packet.send_time > report.feedback_time ||
            (p.receive_time && *p.receive_time < Time{}) || p.sent_packet.sequence_number < 0 ||
            p.sent_packet.size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            p.sent_packet.prior_unacked_data > std::numeric_limits<std::size_t>::max() - p.sent_packet.size)
            throw std::invalid_argument("invalid GoogCC packet feedback or send history");
        const auto& pacing = p.sent_packet.pacing_info;
        if (p.IsReceived() && pacing.probe_cluster_id >= 0 && (pacing.probe_cluster_min_probes <= 0 || pacing.probe_cluster_min_bytes <= 0))
            throw std::invalid_argument("invalid GoogCC probe metadata");
    }
    s.observe(report.feedback_time); s.generation = report.generation; s.first_feedback = true;
    if (s.pushback) s.pushback->UpdateOutstandingData(report.data_in_flight);
    const auto received = report.SortedByReceiveTime();
    std::optional<Delta> max_rtt, min_propagation;
    if (!received.empty())
    {
        const auto max_recv = micros(*received.back().receive_time);
        for (const auto& p : received)
        {
            const auto rtt = micros(report.feedback_time) - micros(*p.sent_packet.send_time);
            const auto propagation = rtt - (max_recv - micros(*p.receive_time));
            max_rtt = max_rtt ? std::max(*max_rtt, rtt) : rtt;
            min_propagation = min_propagation ? std::min(*min_propagation, propagation) : propagation;
        }
        const auto us = max_rtt->count();
        s.feedback_max_rtts.push_back(us / 1000 + (us % 1000 >= 500));
        if (s.feedback_max_rtts.size() > 32) s.feedback_max_rtts.pop_front();
        s.bandwidth.UpdatePropagationRtt(report.feedback_time, *min_propagation);
    }
    const auto alr_start = s.alr.GetApplicationLimitedRegionStartTime();
    if (s.previously_in_alr && !alr_start)
    { s.acknowledged->SetAlrEndedTime(report.feedback_time); s.probes.SetAlrEndedTime(report.feedback_time); }
    s.previously_in_alr = alr_start.has_value();
    s.acknowledged->IncomingPacketFeedbackVector(received);
    const auto acked = s.acknowledged->bitrate(); s.bandwidth.SetAcknowledgedRate(acked, report.feedback_time);
    for (const auto& p : received)
        if (p.sent_packet.pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe) s.probe_bitrate->HandleProbeAndEstimateBitrate(p);
    if (s.config.network_state_estimator)
    { s.config.network_state_estimator->OnTransportPacketsFeedback(report); s.setEstimate(s.config.network_state_estimator->GetCurrentEstimate()); }
    auto probe = s.probe_bitrate->FetchAndResetLastEstimatedBitrate();
    if (s.config.ignore_probes_lower_than_network_estimate && probe && s.estimate && s.estimate->link_capacity_lower &&
        *probe < s.delay->last_estimate() && *probe < *s.estimate->link_capacity_lower) probe.reset();
    if (s.config.limit_probes_lower_than_throughput_estimate && probe && acked)
        probe = std::max(*probe, std::min(s.delay->last_estimate(), cc_detail::scale(*acked, 0.85)));
    const auto result = s.delay->IncomingPacketFeedbackVector(report, acked, probe, s.estimate, alr_start.has_value());
    if (result.updated)
    {
        if (result.probe) s.bandwidth.SetSendBitrate(result.target_bitrate, report.feedback_time);
        s.bandwidth.UpdateDelayBasedEstimate(report.feedback_time, result.target_bitrate);
    }
    s.bandwidth.UpdateLossBasedEstimator(report, result.delay_detector_state, probe, alr_start.has_value());
    if (result.updated) s.maybeChanged(update, report.feedback_time);
    if (result.recovered_from_overuse)
    { s.probes.SetAlrStartTime(alr_start); append(update, s.probes.RequestProbe(report.feedback_time)); }
    if (s.config.use_congestion_window && max_rtt) s.updateWindow();
    s.publishWindow(update);
    UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG)) << "[WebRTC][GoogCC][" << &s
        << "] feedback generation=" << report.generation << " packets=" << report.packet_feedbacks.size() << " received=" << received.size()
        << " acknowledged_bps=" << acked.value_or(0) << " probe_bps=" << probe.value_or(0) << " target_bps=" << s.last_target;
    return update;
}
NetworkControlUpdate GoogCcNetworkController::OnNetworkStateEstimate(NetworkStateEstimate msg)
{
    // Estimate calculation time may precede delivery; it is not a control event clock.
    if (msg.update_time) validateTime(*msg.update_time);
    if (!impl_->config.network_state_estimator) impl_->setEstimate(msg);
    return {};
}
NetworkControlUpdate GoogCcNetworkController::GetNetworkState(Time at) const
{
    validateTime(at); const auto& s = *impl_; NetworkControlUpdate update; TargetTransferRate target;
    target.at_time = target.network_estimate.at_time = at; target.target_rate = s.last_pushback_target;
    target.network_estimate.round_trip_time = s.last_rtt;
    target.network_estimate.loss_rate_ratio = static_cast<float>(s.last_fraction_loss / 255.0);
    target.is_bandwidth_limited = !s.alr.GetApplicationLimitedRegionStartTime();
    update.target_rate = target; update.pacer_config = s.pacing(at); update.congestion_window = s.data_window; return update;
}
} // namespace webrtc
