// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/network_control.h"
#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"
#include"webrtc/congestion_control/alr_detector.h"
#include"webrtc/congestion_control/congestion_window_pushback_controller.h"
#include"webrtc/congestion_control/delay_based_bwe.h"
#include"webrtc/congestion_control/send_side_bandwidth_estimation.h"

namespace webrtc
{
struct GoogCcConfig
{
    ProbeController::Config probing;
    AcknowledgedBitrateEstimator::Config acknowledged;
    AlrDetector::Config alr;
    DelayBasedBwe::Config delay;
    SendSideBandwidthEstimation::Config bandwidth;
    bool safe_reset_on_route_change{false}, safe_reset_acknowledged_rate{false};
    bool use_min_allocatable_as_lower_bound{true};
    bool ignore_probes_lower_than_network_estimate{true};
    bool limit_probes_lower_than_throughput_estimate{true};
    bool limit_pacing_factor_by_upper_link_capacity{false};
    bool use_congestion_window{true}, use_congestion_window_pushback{true}, drop_frame_only{true};
    std::chrono::microseconds congestion_window_additional_time{std::chrono::milliseconds(350)};
    CongestionWindowPushbackController::Config pushback;
    std::unique_ptr<NetworkStateEstimator> network_state_estimator;
    std::unique_ptr<NetworkStatePredictor> network_state_predictor;
};

// Event-driven GoogCC orchestration. Serialized calls, nonnegative chronological
// local event times. OnSentPacket must be called exactly once per actual send;
// feedback is the adapter's raw view (received AND lost packets with send info).
// Outputs are requests: the owner applies encoder rate, pacing and probes.
class GoogCcNetworkController final : public NetworkControllerInterface
{
public:
    using Time = TransportTime;
    using Delta = std::chrono::microseconds;
    explicit GoogCcNetworkController(NetworkControllerConfig config, GoogCcConfig goog_cc_config = {});
    ~GoogCcNetworkController() override;
    GoogCcNetworkController(const GoogCcNetworkController&) = delete;
    GoogCcNetworkController& operator=(const GoogCcNetworkController&) = delete;
    [[nodiscard]] NetworkControlUpdate OnNetworkAvailability(NetworkAvailability) override;
    [[nodiscard]] NetworkControlUpdate OnNetworkRouteChange(NetworkRouteChange) override;
    [[nodiscard]] NetworkControlUpdate OnProcessInterval(ProcessInterval) override;
    [[nodiscard]] NetworkControlUpdate OnRemoteBitrateReport(RemoteBitrateReport) override;
    [[nodiscard]] NetworkControlUpdate OnRoundTripTimeUpdate(RoundTripTimeUpdate) override;
    [[nodiscard]] NetworkControlUpdate OnSentPacket(SentPacket) override;
    [[nodiscard]] NetworkControlUpdate OnReceivedPacket(ReceivedPacket) override;
    [[nodiscard]] NetworkControlUpdate OnStreamsConfig(StreamsConfig) override;
    [[nodiscard]] NetworkControlUpdate OnTargetRateConstraints(TargetRateConstraints) override;
    [[nodiscard]] NetworkControlUpdate OnTransportLossReport(TransportLossReport) override;
    [[nodiscard]] NetworkControlUpdate OnTransportPacketsFeedback(TransportPacketsFeedback) override;
    [[nodiscard]] NetworkControlUpdate OnNetworkStateEstimate(NetworkStateEstimate) override;
    NetworkControlUpdate GetNetworkState(Time at_time) const;
    void UpdateOutstandingData(std::size_t bytes); // Transport correction after local send failures; does not count another sent packet.
    static constexpr Delta GetProcessInterval() { return std::chrono::milliseconds(25); }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace webrtc
