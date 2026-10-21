// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/probe_controller.h"
#include"webrtc/transport_adapter.h"
#include<memory>

namespace webrtc
{
// Rates are bit/s, sizes are bytes, times use the transport's steady clock.
// nullopt denotes an absent update or unbounded rate, never a finite zero.
struct TargetRateConstraints
{
    TransportTime at_time{};
    std::optional<std::uint64_t> min_data_rate, max_data_rate, starting_rate;
};
struct StreamsConfig
{
    TransportTime at_time{};
    std::optional<bool> requests_alr_probing;
    bool enable_repeated_initial_probing{false};
    std::optional<double> pacing_factor;
    std::optional<std::uint64_t> min_total_allocated_bitrate, max_padding_rate, max_total_allocated_bitrate;
};
struct NetworkRouteChange
{
    TransportTime at_time{};
    TargetRateConstraints constraints;
    bool restart_bwe{true}; // Present in upstream; GoogCC always resets on this event.
    std::optional<std::uint64_t> generation{}; // Adapter's new ICE association, if known.
};
struct ProcessInterval { TransportTime at_time{}; std::optional<std::size_t> pacer_queue; };
struct RemoteBitrateReport { TransportTime receive_time{}; std::optional<std::uint64_t> bandwidth; };
struct RoundTripTimeUpdate
{
    TransportTime receive_time{};
    std::chrono::microseconds round_trip_time{0};
    bool smoothed{false};
};
struct TransportLossReport
{
    TransportTime receive_time{}, start_time{}, end_time{};
    std::uint64_t packets_lost_delta{0}, packets_received_delta{0};
};
struct ReceivedPacket
{
    std::optional<TransportTime> send_time;
    TransportTime receive_time{};
    std::size_t size{0};
};
struct NetworkEstimate
{
    TransportTime at_time{};
    std::optional<std::chrono::microseconds> round_trip_time;
    float loss_rate_ratio{0};
};
struct TargetTransferRate
{
    TransportTime at_time{};
    NetworkEstimate network_estimate;
    std::uint64_t target_rate{0};
    double cwnd_reduce_ratio{0};
    bool is_bandwidth_limited{true};
};
struct PacerConfig
{
    static constexpr std::chrono::microseconds kDefaultTimeInterval{40000};
    TransportTime at_time{};
    std::size_t data_window{0}, pad_window{0};
    std::chrono::microseconds time_window{kDefaultTimeInterval};
    static PacerConfig Create(TransportTime at_time, std::uint64_t send_rate, std::uint64_t pad_rate,
        std::chrono::microseconds time_window = kDefaultTimeInterval);
    std::chrono::microseconds rate_window() const;
    std::uint64_t data_rate() const;
    std::uint64_t pad_rate() const;
};
struct NetworkControlUpdate
{
    std::optional<std::size_t> congestion_window;
    std::optional<PacerConfig> pacer_config;
    std::vector<ProbeClusterConfig> probe_cluster_configs;
    std::optional<TargetTransferRate> target_rate;
    bool has_updates() const
    { return congestion_window || pacer_config || !probe_cluster_configs.empty() || target_rate; }
};
struct NetworkControllerConfig
{
    TargetRateConstraints constraints; // starting_rate is required at construction.
    StreamsConfig stream_based_config;
    std::chrono::microseconds default_pacing_time_window{PacerConfig::kDefaultTimeInterval};
};
class NetworkControllerInterface
{
public:
    virtual ~NetworkControllerInterface() = default;
    [[nodiscard]] virtual NetworkControlUpdate OnNetworkAvailability(NetworkAvailability) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnNetworkRouteChange(NetworkRouteChange) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnProcessInterval(ProcessInterval) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnRemoteBitrateReport(RemoteBitrateReport) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnRoundTripTimeUpdate(RoundTripTimeUpdate) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnSentPacket(SentPacket) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnReceivedPacket(ReceivedPacket) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnStreamsConfig(StreamsConfig) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnTargetRateConstraints(TargetRateConstraints) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnTransportLossReport(TransportLossReport) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnTransportPacketsFeedback(TransportPacketsFeedback) = 0;
    [[nodiscard]] virtual NetworkControlUpdate OnNetworkStateEstimate(NetworkStateEstimate) = 0;
    virtual bool SupportsEcnAdaptation() const { return false; }
};
class NetworkStateEstimator
{
public:
    virtual ~NetworkStateEstimator() = default;
    virtual std::optional<NetworkStateEstimate> GetCurrentEstimate() = 0;
    virtual void OnTransportPacketsFeedback(const TransportPacketsFeedback&) = 0;
    virtual void OnRouteChange(const NetworkRouteChange&) = 0;
};
} // namespace webrtc
