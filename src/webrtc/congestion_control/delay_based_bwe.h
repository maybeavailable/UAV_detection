// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/aimd_rate_control.h"
#include"webrtc/congestion_control/inter_arrival_delta.h"
#include"webrtc/congestion_control/trendline_estimator.h"
#include"webrtc/transport_adapter.h"
#include<memory>

namespace webrtc
{
struct BweSeparateAudioPacketsSettings
{
    bool enabled{false};
    int packet_threshold{10};
    std::chrono::microseconds time_threshold{std::chrono::seconds(1)};
};

// TWCC feedback -> packet groups -> delay detector -> sender-side AIMD target.
// The owner must serialize all access and rebuild on route/generation changes.
class DelayBasedBwe
{
public:
    using Time = TransportTime;
    using Delta = std::chrono::microseconds;
    struct Config
    {
        TrendlineEstimatorSettings trendline;
        BweSeparateAudioPacketsSettings separate_audio;
        AimdRateControl::Config aimd; // send_side is always true here.
    };
    struct Result
    {
        bool updated{false}, probe{false};
        std::uint64_t target_bitrate{0}; // bit/s; consume only when updated.
        bool recovered_from_overuse{false}; // Upstream: underusing -> normal.
        BandwidthUsage delay_detector_state{BandwidthUsage::kBwNormal};
    };

    DelayBasedBwe();
    // Predictor is optional, borrowed, and must outlive this controller.
    explicit DelayBasedBwe(Config config, NetworkStatePredictor* predictor = nullptr);
    ~DelayBasedBwe();
    DelayBasedBwe(const DelayBasedBwe&) = delete;
    DelayBasedBwe& operator=(const DelayBasedBwe&) = delete;

    Result IncomingPacketFeedbackVector(const TransportPacketsFeedback& msg,
        std::optional<std::uint64_t> acked_bitrate, std::optional<std::uint64_t> probe_bitrate,
        std::optional<NetworkStateEstimate> network_estimate, bool in_alr);
    void OnRttUpdate(Delta avg_rtt);
    std::optional<std::uint64_t> LatestEstimate() const;
    bool LatestEstimate(std::vector<std::uint32_t>* ssrcs, std::uint64_t* bitrate) const;
    void SetStartBitrate(std::uint64_t start_bitrate_bps);
    void SetMinBitrate(std::uint64_t min_bitrate_bps);
    std::uint64_t TriggerOveruse(Time at_time, std::optional<std::uint64_t> link_capacity);
    std::uint64_t last_estimate() const { return prev_bitrate_; }
    BandwidthUsage last_state() const { return prev_state_; }

private:
    void ValidateTime(Time at_time) const;
    void IncomingPacketFeedback(const PacketResult& packet, Time at_time);
    Result MaybeUpdateEstimate(std::optional<std::uint64_t> acked_bitrate,
        std::optional<std::uint64_t> probe_bitrate, bool recovered_from_overuse, Time at_time);
    bool UpdateEstimate(Time at_time, std::optional<std::uint64_t> acked_bitrate, std::uint64_t* target);

    const Config config_;
    std::int64_t audio_packets_since_last_video_{0};
    std::optional<Delta> last_video_packet_recv_time_, last_seen_packet_, last_control_time_;
    NetworkStatePredictor* const predictor_;
    std::unique_ptr<InterArrivalDelta> video_inter_arrival_, audio_inter_arrival_;
    std::unique_ptr<TrendlineEstimator> video_detector_, audio_detector_;
    TrendlineEstimator* active_detector_;
    AimdRateControl rate_control_;
    std::uint64_t prev_bitrate_{0};
    BandwidthUsage prev_state_{BandwidthUsage::kBwNormal};
};
} // namespace webrtc
