// Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once
#include"webrtc/congestion_control/probe_cluster_config.h"
#include"webrtc/transport_adapter.h"
#include<deque>

namespace webrtc
{
// Probe scheduling policy from libwebrtc. The transport sends the actual burst
// and calls ProbeSent once with its total wire bytes, not once per datagram.
class BitrateProber
{
public:
    using Time = TransportTime;
    struct Config
    {
        std::chrono::microseconds max_probe_delay{std::chrono::milliseconds(10)};
        std::size_t min_packet_size{200};
        bool allow_start_probing_immediately{false};
    };
    BitrateProber();
    explicit BitrateProber(Config config);
    void SetEnabled(bool enabled);
    void SetAllowProbeWithoutMediaPacket(bool allow);
    bool is_probing() const;
    void OnIncomingPacket(std::size_t bytes);
    void CreateProbeCluster(const ProbeClusterConfig& config);
    std::optional<Time> NextProbeTime(Time now) const; // Time{} means immediately; nullopt means inactive.
    std::optional<PacedPacketInfo> CurrentCluster(Time now);
    std::size_t RecommendedMinProbeSize() const;
    void ProbeSent(Time now, std::size_t bytes);
    void Reset();
    std::size_t pending_clusters() const { return clusters_.size(); }
private:
    enum class State { Disabled, Inactive, Active };
    struct Cluster
    {
        PacedPacketInfo pacing;
        std::uint64_t sent_probes{0}, sent_bytes{0};
        std::chrono::microseconds min_delta{0};
        Time requested{};
        std::optional<Time> started;
    };
    void MaybeActivate(std::size_t bytes);
    Config config_;
    State state_{State::Inactive};
    std::deque<Cluster> clusters_;
    std::optional<Time> next_;
};
} // namespace webrtc
