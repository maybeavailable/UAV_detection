/*
 * Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/delay_based_bwe.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/delay_based_bwe.h"
#include"log/log.h"

#include<algorithm>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
using Delta = DelayBasedBwe::Delta;
using Time = DelayBasedBwe::Time;
Delta micros(Time time) { return std::chrono::duration_cast<Delta>(time.time_since_epoch()); }
std::int64_t millis(Time time)
{
    const auto us = micros(time).count();
    return us / 1000 + (us % 1000 >= 500); // Upstream Timestamp::ms().
}
DelayBasedBwe::Config checkedConfig(DelayBasedBwe::Config config)
{
    if (config.separate_audio.packet_threshold < 0 || config.separate_audio.time_threshold < Delta::zero())
        throw std::invalid_argument("invalid separate audio packet detection settings");
    config.aimd.send_side = true;
    return config;
}
} // namespace

DelayBasedBwe::DelayBasedBwe() : DelayBasedBwe(Config{}) {}
DelayBasedBwe::DelayBasedBwe(Config config, NetworkStatePredictor* predictor)
    : config_(checkedConfig(config)), predictor_(predictor),
      video_detector_(std::make_unique<TrendlineEstimator>(config_.trendline, predictor_)),
      audio_detector_(std::make_unique<TrendlineEstimator>(config_.trendline, predictor_)),
      active_detector_(video_detector_.get()), rate_control_(config_.aimd)
{
    UAV_LOG_INFO << "[WebRTC][DelayBasedBwe] initialized separate_audio=" << config_.separate_audio.enabled
        << " audio_packet_threshold=" << config_.separate_audio.packet_threshold;
}
DelayBasedBwe::~DelayBasedBwe() = default;
void DelayBasedBwe::ValidateTime(Time at_time) const
{
    if (at_time.time_since_epoch() < Time::duration::zero() ||
        (last_control_time_ && micros(at_time) < *last_control_time_))
        throw std::invalid_argument("DelayBasedBwe requires nonnegative, nondecreasing feedback timestamps");
}
DelayBasedBwe::Result DelayBasedBwe::IncomingPacketFeedbackVector(const TransportPacketsFeedback& msg,
    std::optional<std::uint64_t> acked, std::optional<std::uint64_t> probe,
    std::optional<NetworkStateEstimate> network, bool in_alr)
{
    const auto packets = msg.SortedByReceiveTime();
    if (packets.empty())
    {
        UAV_LOG_DEBUG << "[WebRTC][DelayBasedBwe] feedback has no received packets with send history";
        return {};
    }
    ValidateTime(msg.feedback_time);
    // Check all selected packets before consuming the batch. Receive times use
    // a reconstructed receiver epoch and need not precede feedback_time.
    for (const auto& packet : packets)
    {
        if (packet.sent_packet.send_time->time_since_epoch() < Time::duration::zero() ||
            packet.receive_time->time_since_epoch() < Time::duration::zero() ||
            packet.sent_packet.size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("invalid DelayBasedBwe packet timestamp or size");
    }
    bool recovered = false;
    auto previous = active_detector_->State();
    for (const auto& packet : packets)
    {
        IncomingPacketFeedback(packet, msg.feedback_time);
        if (previous == BandwidthUsage::kBwUnderusing && active_detector_->State() == BandwidthUsage::kBwNormal)
            recovered = true;
        previous = active_detector_->State();
    }
    rate_control_.SetInApplicationLimitedRegion(in_alr);
    rate_control_.SetNetworkStateEstimate(network);
    const auto result = MaybeUpdateEstimate(acked, probe, recovered, msg.feedback_time);
    last_control_time_ = micros(msg.feedback_time);
    return result;
}
void DelayBasedBwe::IncomingPacketFeedback(const PacketResult& packet, Time at_time)
{
    const auto now = micros(at_time);
    if (!last_seen_packet_ || now - *last_seen_packet_ > std::chrono::seconds(2))
    {
        video_inter_arrival_ = std::make_unique<InterArrivalDelta>(std::chrono::milliseconds(5));
        audio_inter_arrival_ = std::make_unique<InterArrivalDelta>(std::chrono::milliseconds(5));
        video_detector_ = std::make_unique<TrendlineEstimator>(config_.trendline, predictor_);
        audio_detector_ = std::make_unique<TrendlineEstimator>(config_.trendline, predictor_);
        active_detector_ = video_detector_.get();
        if (last_seen_packet_)
            UAV_LOG_DEBUG << "[WebRTC][DelayBasedBwe] stream timed out, rebuilding delay detectors";
    }
    last_seen_packet_ = now;
    auto* detector = video_detector_.get();
    const auto receive = micros(*packet.receive_time);
    if (config_.separate_audio.enabled)
    {
        if (packet.sent_packet.audio)
        {
            detector = audio_detector_.get();
            if (audio_packets_since_last_video_ < std::numeric_limits<std::int64_t>::max()) ++audio_packets_since_last_video_;
            if (audio_packets_since_last_video_ > config_.separate_audio.packet_threshold &&
                (!last_video_packet_recv_time_ || receive - *last_video_packet_recv_time_ > config_.separate_audio.time_threshold))
                active_detector_ = audio_detector_.get();
        }
        else
        {
            audio_packets_since_last_video_ = 0;
            last_video_packet_recv_time_ = last_video_packet_recv_time_ ? std::max(*last_video_packet_recv_time_, receive) : receive;
            active_detector_ = video_detector_.get();
        }
    }
    auto* inter_arrival = config_.separate_audio.enabled && packet.sent_packet.audio
        ? audio_inter_arrival_.get() : video_inter_arrival_.get();
    Delta send_delta{0}, receive_delta{0};
    int size_delta = 0;
    const auto size = packet.sent_packet.size; // prior_unacked_data belongs to ACK throughput only.
    const auto calculated = inter_arrival->ComputeDeltas(*packet.sent_packet.send_time, *packet.receive_time,
        at_time, size, &send_delta, &receive_delta, &size_delta);
    detector->Update(receive_delta.count() * 0.001, send_delta.count() * 0.001,
        millis(*packet.sent_packet.send_time), millis(*packet.receive_time), size, calculated);
}
std::uint64_t DelayBasedBwe::TriggerOveruse(Time at_time, std::optional<std::uint64_t> capacity)
{
    ValidateTime(at_time);
    const auto rate = rate_control_.Update({BandwidthUsage::kBwOverusing, capacity}, at_time);
    last_control_time_ = micros(at_time);
    return rate;
}
DelayBasedBwe::Result DelayBasedBwe::MaybeUpdateEstimate(std::optional<std::uint64_t> acked,
    std::optional<std::uint64_t> probe, bool recovered, Time at_time)
{
    Result result;
    if (active_detector_->State() == BandwidthUsage::kBwOverusing)
    {
        if (acked && rate_control_.TimeToReduceFurther(at_time, *acked))
            result.updated = UpdateEstimate(at_time, acked, &result.target_bitrate);
        else if (!acked && rate_control_.ValidEstimate() && rate_control_.InitialTimeToReduceFurther(at_time))
        {
            rate_control_.SetEstimate(rate_control_.LatestEstimate() / 2, at_time);
            result.updated = true;
            result.target_bitrate = rate_control_.LatestEstimate();
        }
    }
    else if (probe)
    {
        result.probe = result.updated = true;
        rate_control_.SetEstimate(*probe, at_time);
        result.target_bitrate = rate_control_.LatestEstimate();
        UAV_LOG_DEBUG << "[WebRTC][DelayBasedBwe] accepted probe_bps=" << *probe << " target_bps=" << result.target_bitrate;
    }
    else
    {
        result.updated = UpdateEstimate(at_time, acked, &result.target_bitrate);
        result.recovered_from_overuse = recovered;
    }
    const auto state = active_detector_->State();
    if ((result.updated && prev_bitrate_ != result.target_bitrate) || state != prev_state_)
    {
        if (state != prev_state_)
            UAV_LOG_DEBUG << "[WebRTC][DelayBasedBwe] detector_state=" << static_cast<int>(state)
                << " previous_state=" << static_cast<int>(prev_state_)
                << " target_bps=" << (result.updated ? result.target_bitrate : prev_bitrate_);
        if (result.updated) prev_bitrate_ = result.target_bitrate;
        prev_state_ = state;
    }
    result.delay_detector_state = state;
    return result;
}
bool DelayBasedBwe::UpdateEstimate(Time at_time, std::optional<std::uint64_t> acked, std::uint64_t* target)
{
    *target = rate_control_.Update({active_detector_->State(), acked}, at_time);
    return rate_control_.ValidEstimate();
}
void DelayBasedBwe::OnRttUpdate(Delta rtt) { rate_control_.SetRtt(rtt); }
std::optional<std::uint64_t> DelayBasedBwe::LatestEstimate() const
{
    return rate_control_.ValidEstimate() ? std::optional<std::uint64_t>(rate_control_.LatestEstimate()) : std::nullopt;
}
bool DelayBasedBwe::LatestEstimate(std::vector<std::uint32_t>* ssrcs, std::uint64_t* bitrate) const
{
    if (!ssrcs || !bitrate) throw std::invalid_argument("null DelayBasedBwe estimate output");
    if (!rate_control_.ValidEstimate()) return false;
    *ssrcs = {0}; // Upstream's legacy dummy SSRC; the estimate is transport-wide.
    *bitrate = rate_control_.LatestEstimate();
    return true;
}
void DelayBasedBwe::SetStartBitrate(std::uint64_t bitrate)
{
    UAV_LOG_INFO << "[WebRTC][DelayBasedBwe] start_bps=" << bitrate;
    rate_control_.SetStartBitrate(bitrate);
}
void DelayBasedBwe::SetMinBitrate(std::uint64_t bitrate) { rate_control_.SetMinBitrate(bitrate); }
} // namespace webrtc
