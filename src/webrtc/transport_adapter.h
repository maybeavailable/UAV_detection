// Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE and PATENTS.
#pragma once

#include"webrtc/rtcp_protocol.h"
#include<chrono>
#include<cstddef>
#include<cstdint>
#include<memory>
#include<optional>
#include<vector>

namespace webrtc
{
using TransportTime = std::chrono::steady_clock::time_point;
// TWCC port of libwebrtc TransportFeedbackAdapter. See docs/transport-adapter.md
// for the pinned upstream revision, type adaptations and BSD license.
struct PacedPacketInfo
{
    int probe_cluster_id{-1}, probe_cluster_min_probes{-1}, probe_cluster_min_bytes{-1};
    std::uint64_t send_bitrate_bps{0};
};
struct SentPacket
{
    std::optional<TransportTime> send_time; // upstream MinusInfinity -> nullopt
    std::size_t size{0}, prior_unacked_data{0}; // UDP payload plus configured overhead
    PacedPacketInfo pacing_info;
    bool audio{false};
    std::int64_t sequence_number{-1};
    std::size_t data_in_flight{0};
};
struct PacketResult
{
    struct RtpPacketInfo { std::uint32_t ssrc{0}; std::uint16_t rtp_sequence_number{0}; bool is_retransmission{false}; };
    SentPacket sent_packet;
    std::optional<TransportTime> receive_time; // artificial offset, not clock sync; PlusInfinity -> nullopt
    std::optional<RtpPacketInfo> rtp_packet_info;
    bool ambiguous_receive_time{false}, reported_lost_for_the_first_time{false}, reported_recovered_for_the_first_time{false};
    // Extra diagnostic fields: receiver clock at 250 us precision and UDP payload size.
    std::optional<std::int64_t> remote_arrival_us;
    std::size_t wire_size{0};
    bool IsReceived() const { return receive_time.has_value(); }
};
struct TransportPacketsFeedback
{
    TransportTime feedback_time{};
    std::size_t data_in_flight{0};
    std::vector<PacketResult> packet_feedbacks;
    std::vector<PacketResult> ReceivedWithSendInfo() const;
    std::vector<PacketResult> LostWithSendInfo() const;
    std::vector<PacketResult> PacketsWithFeedback() const;
    std::vector<PacketResult> SortedByReceiveTime() const;
    std::uint64_t generation{0}; // local ICE association, not a wire field
    std::uint8_t feedback_count{0};
};
struct RtpPacketSendInfo
{
    std::uint16_t transport_sequence_number{0};
    std::size_t packet_size{0}; // full SRTP UDP payload
    std::uint32_t ssrc{0}; std::uint16_t rtp_sequence_number{0};
    bool audio{false}, is_retransmission{false};
    std::optional<std::uint32_t> original_ssrc{}; // RTX: independent SSRC makes arrival unambiguous
};
struct SentPacketInfo
{
    std::int64_t packet_id{-1}; // 16-bit TWCC id; -1 for untracked allocation traffic
    TransportTime send_time{};
    struct Info { bool included_in_feedback{false}, included_in_allocation{false}; std::size_t packet_size_bytes{0}; } info;
};
struct TransportNetworkRoute
{
    std::uint32_t local_network_id{0}, remote_network_id{0}, local_adapter_id{0}, remote_adapter_id{0};
    bool local_uses_turn{false}, remote_uses_turn{false}, connected{true};
    bool operator<(const TransportNetworkRoute& other) const;
    bool operator==(const TransportNetworkRoute& other) const;
};
struct TransportAdapterStats
{
    std::uint64_t generation{0}, registered_packets{0}, sent_packets{0}, retransmissions{0}, feedback_packets{0};
    std::uint64_t failed_lookups{0}, ignored_route_packets{0}, expired_packets{0}, failed_sends{0}, malformed_feedback{0};
    std::size_t history_size{0}, data_in_flight{0}, pending_untracked_size{0};
};
// Synchronous, serialized by the owning transport strand. Only authenticated RTCP
// may be passed in. Registered packets are not sent until ProcessSentPacket().
class TransportFeedbackAdapter
{
public:
    using Time = TransportTime;
    struct Config
    {
        std::chrono::milliseconds history_age{60000}; // upstream: 60 seconds
        std::size_t max_history{8192}; // bounded adaptation, at most half the 16-bit sequence space
    };
    TransportFeedbackAdapter();
    explicit TransportFeedbackAdapter(Config config);
    ~TransportFeedbackAdapter();
    TransportFeedbackAdapter(const TransportFeedbackAdapter&) = delete;
    TransportFeedbackAdapter& operator=(const TransportFeedbackAdapter&) = delete;
    bool AddPacket(const RtpPacketSendInfo& packet, const PacedPacketInfo& pacing, std::size_t overhead_bytes, Time creation_time);
    std::optional<SentPacket> ProcessSentPacket(const SentPacketInfo& packet);
    std::optional<TransportPacketsFeedback> ProcessTransportFeedback(const RtcpTwcc& feedback, Time feedback_receive_time);
    // Compound RTCP is validated completely before any TWCC state changes.
    bool ProcessRtcp(const RtcpBytes& plaintext, Time feedback_receive_time, std::vector<TransportPacketsFeedback>& out);
    void SetNetworkRoute(const TransportNetworkRoute& route);
    std::size_t GetOutstandingData() const;
    bool RemovePacket(std::int64_t sequence); // exact unwrapped id, safe for late local failure callbacks
    bool MarkRetransmitted(std::int64_t sequence, Time sentAt); // cached ciphertext, same transport id
    void Prune(Time now);
    void Clear(); // renegotiation: retain unwrap continuity and counters
    void Reset(std::uint64_t generation);
    TransportAdapterStats Snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using TransportAdapter = TransportFeedbackAdapter;

struct TransportPacketDelta
{
    std::int64_t previous_sequence_number{0}, sequence_number{0};
    std::chrono::microseconds send_delta{0}, receive_delta{0}, delay_delta{0};
};
// Convenience layer outside the upstream adapter: consecutive received packets,
// including across reports. Skips old/reordered/ambiguous packets; no GCC grouping.
class TransportFeedbackDeltaCalculator
{
public:
    std::vector<TransportPacketDelta> Calculate(const TransportPacketsFeedback& feedback);
    void Reset();
private:
    std::optional<PacketResult> previous_;
    std::optional<std::uint64_t> generation_;
};
} // namespace webrtc
