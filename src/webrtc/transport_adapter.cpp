/*
 * Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 * Adapted TWCC transport feedback path; see third_party/webrtc/LICENSE and PATENTS.
 * Upstream revision: 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 */
#include"webrtc/transport_adapter.h"
#include"log/log.h"

#include<algorithm>
#include<limits>
#include<map>
#include<stdexcept>
#include<tuple>
#include<utility>

namespace webrtc
{
namespace
{
using Us = std::chrono::microseconds;
std::int64_t unwrap(std::uint64_t value, std::int64_t near, unsigned bits)
{
    const auto modulus = std::int64_t(1) << bits;
    auto result = (near & ~(modulus - 1)) | static_cast<std::int64_t>(value);
    if (result - near > modulus / 2) result -= modulus;
    else if (near - result > modulus / 2) result += modulus;
    return result;
}
std::int64_t floorMs(std::int64_t us) { return (us >= 0 ? us / 1000 : -((-us + 999) / 1000)) * 1000; }
std::size_t addSize(std::size_t a, std::size_t b)
{ return a + std::min(b, std::numeric_limits<std::size_t>::max() - a); }
bool valid(const RtcpTwcc& f)
{
    return f.referenceTime < 0x1000000 && !f.deltas.empty() && f.deltas.size() <= 8192 &&
        std::all_of(f.deltas.begin(), f.deltas.end(), [](auto d) { return !d || (*d >= -32768 && *d <= 32767); });
}
auto routeKey(const TransportNetworkRoute& r)
{
    return std::make_tuple(r.local_network_id, r.remote_network_id, r.local_adapter_id, r.remote_adapter_id,
        r.local_uses_turn, r.remote_uses_turn, r.connected);
}
}
bool TransportNetworkRoute::operator<(const TransportNetworkRoute& other) const { return routeKey(*this) < routeKey(other); }
bool TransportNetworkRoute::operator==(const TransportNetworkRoute& other) const { return routeKey(*this) == routeKey(other); }
std::vector<PacketResult> TransportPacketsFeedback::ReceivedWithSendInfo() const
{
    std::vector<PacketResult> result;
    for (const auto& p : packet_feedbacks) if (p.IsReceived() && p.sent_packet.send_time) result.push_back(p);
    return result;
}
std::vector<PacketResult> TransportPacketsFeedback::LostWithSendInfo() const
{
    std::vector<PacketResult> result;
    for (const auto& p : packet_feedbacks) if (!p.IsReceived() && p.sent_packet.send_time) result.push_back(p);
    return result;
}
std::vector<PacketResult> TransportPacketsFeedback::PacketsWithFeedback() const { return packet_feedbacks; }
std::vector<PacketResult> TransportPacketsFeedback::SortedByReceiveTime() const
{
    auto result = ReceivedWithSendInfo();
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.receive_time, a.sent_packet.send_time, a.sent_packet.sequence_number) <
            std::tie(b.receive_time, b.sent_packet.send_time, b.sent_packet.sequence_number);
    });
    return result;
}
struct TransportFeedbackAdapter::Impl
{
    struct PacketFeedback
    {
        Time creation_time{}; SentPacket sent;
        TransportNetworkRoute network_route;
        std::uint32_t ssrc{0}; std::uint16_t rtp_sequence_number{0}; std::size_t wire_size{0};
        bool is_retransmission{false}, ambiguous_receive_time{false}, previously_reported_lost{false}, in_flight{false};
    };
    using RtpKey = std::pair<std::uint32_t, std::uint16_t>;
    Config config; TransportAdapterStats stats;
    std::map<std::int64_t, PacketFeedback> history;
    std::map<RtpKey, std::int64_t> rtp_to_transport;
    std::map<TransportNetworkRoute, std::size_t> in_flight;
    TransportNetworkRoute route;
    std::optional<std::int64_t> sequence_anchor, remote_reference;
    std::int64_t last_ack_seq_num{-1};
    std::optional<Time> current_offset, last_send_time, last_untracked_send_time;
    std::size_t pending_untracked_size{0};

    explicit Impl(Config c) : config(c)
    {
        if (c.history_age.count() < 1 || c.history_age > std::chrono::hours(1) || !c.max_history || c.max_history > 32768)
            throw std::invalid_argument("invalid transport feedback history limits");
    }
    std::int64_t sequence(std::uint16_t value)
    {
        const auto extended = sequence_anchor ? unwrap(value, *sequence_anchor, 16) : std::int64_t(value);
        if (!sequence_anchor || extended > *sequence_anchor) sequence_anchor = extended;
        return extended;
    }
    std::size_t outstanding() const
    { const auto it = in_flight.find(route); return it == in_flight.end() ? 0 : it->second; }
    void removeInFlight(PacketFeedback& p)
    {
        if (!p.in_flight) return;
        const auto it = in_flight.find(p.network_route);
        if (it != in_flight.end())
        {
            if (it->second <= p.sent.size) in_flight.erase(it);
            else it->second -= p.sent.size;
        }
        p.in_flight = false;
    }
    void removeIndex(const PacketFeedback& p)
    {
        const auto it = rtp_to_transport.find({p.ssrc, p.rtp_sequence_number});
        if (it != rtp_to_transport.end() && it->second == p.sent.sequence_number) rtp_to_transport.erase(it);
    }
    bool sent(PacketFeedback& p, Time at)
    {
        const bool retransmit = p.sent.send_time.has_value();
        p.sent.send_time = at; last_send_time = last_send_time ? std::max(*last_send_time, at) : at;
        p.sent.prior_unacked_data = addSize(p.sent.prior_unacked_data, pending_untracked_size); pending_untracked_size = 0;
        if (retransmit) { p.ambiguous_receive_time = true; ++stats.retransmissions; return false; }
        if (p.sent.sequence_number > last_ack_seq_num)
        { in_flight[p.network_route] += p.sent.size; p.in_flight = true; }
        p.sent.data_in_flight = outstanding(); ++stats.sent_packets; return true;
    }
    void prune(Time now, std::size_t limit)
    {
        while (!history.empty() && (history.size() > limit || now - history.begin()->second.creation_time > config.history_age))
        {
            auto& p = history.begin()->second; removeInFlight(p); removeIndex(p);
            history.erase(history.begin()); ++stats.expired_packets;
        }
    }
    std::optional<PacketFeedback> retrieve(std::int64_t seq, bool received)
    {
        if (seq > last_ack_seq_num)
        {
            // A feedback status advances the feedback frontier even when it
            // says "not received". Keep that history for a later recovery.
            for (auto it = history.upper_bound(last_ack_seq_num), end = history.upper_bound(seq); it != end; ++it)
                removeInFlight(it->second);
            last_ack_seq_num = seq;
        }
        const auto it = history.find(seq);
        if (it == history.end() || !it->second.sent.send_time) return std::nullopt;
        auto packet = it->second;
        if (received) { removeIndex(it->second); history.erase(it); }
        else it->second.previously_reported_lost = true;
        return packet;
    }
    void clear(bool resetClock = false)
    {
        history.clear(); rtp_to_transport.clear(); in_flight.clear(); pending_untracked_size = 0;
        if (resetClock) { current_offset.reset(); remote_reference.reset(); }
        last_send_time.reset(); last_untracked_send_time.reset();
        last_ack_seq_num = -1;
    }
};
TransportFeedbackAdapter::TransportFeedbackAdapter() : TransportFeedbackAdapter(Config{}) {}
TransportFeedbackAdapter::TransportFeedbackAdapter(Config config) : impl_(std::make_unique<Impl>(config)) {}
TransportFeedbackAdapter::~TransportFeedbackAdapter() = default;
bool TransportFeedbackAdapter::AddPacket(const RtpPacketSendInfo& info, const PacedPacketInfo& pacing, std::size_t overhead, Time created)
{
    if (!info.packet_size || info.packet_size > 65507 || overhead > 512) return false;
    auto& s = *impl_; const auto seq = s.sequence(info.transport_sequence_number);
    if (seq < 0 || s.history.count(seq)) return false;
    s.prune(created, s.config.max_history - 1);
    Impl::PacketFeedback packet; packet.creation_time = created; packet.network_route = s.route;
    packet.sent.sequence_number = seq; packet.sent.size = info.packet_size + overhead; packet.sent.audio = info.audio;
    packet.sent.pacing_info = pacing; packet.ssrc = info.ssrc; packet.rtp_sequence_number = info.rtp_sequence_number;
    packet.wire_size = info.packet_size; packet.is_retransmission = info.is_retransmission;
    const auto previous = s.rtp_to_transport.emplace(Impl::RtpKey{info.ssrc, info.rtp_sequence_number}, seq);
    if (!previous.second)
    {
        packet.ambiguous_receive_time = true;
        const auto old = s.history.find(previous.first->second);
        if (old != s.history.end()) old->second.ambiguous_receive_time = true;
    }
    // Retransmission without an independent original/RTX mapping is ambiguous.
    if (info.is_retransmission && (!info.original_ssrc || *info.original_ssrc == info.ssrc)) packet.ambiguous_receive_time = true;
    s.history.emplace(seq, std::move(packet)); ++s.stats.registered_packets; return true;
}
std::optional<SentPacket> TransportFeedbackAdapter::ProcessSentPacket(const SentPacketInfo& info)
{
    auto& s = *impl_;
    if (info.info.included_in_feedback || info.packet_id != -1)
    {
        if (info.packet_id < 0 || info.packet_id > 65535) return std::nullopt;
        const auto it = s.history.find(s.sequence(static_cast<std::uint16_t>(info.packet_id)));
        if (it == s.history.end()) return std::nullopt;
        if (s.sent(it->second, info.send_time)) return it->second.sent;
        return std::nullopt;
    }
    if (info.info.included_in_allocation)
    {
        s.pending_untracked_size = addSize(s.pending_untracked_size, info.info.packet_size_bytes);
        s.last_untracked_send_time = s.last_untracked_send_time ? std::max(*s.last_untracked_send_time, info.send_time) : info.send_time;
    }
    return std::nullopt;
}
std::optional<TransportPacketsFeedback> TransportFeedbackAdapter::ProcessTransportFeedback(const RtcpTwcc& f, Time now)
{
    auto& s = *impl_;
    if (!valid(f)) { ++s.stats.malformed_feedback; return std::nullopt; }
    s.prune(now, s.config.max_history);
    const auto reference = s.remote_reference ? unwrap(f.referenceTime, *s.remote_reference, 24) :
        (f.referenceTime & 0x800000 ? std::int64_t(f.referenceTime) - 0x1000000 : std::int64_t(f.referenceTime));
    if (!s.current_offset) s.current_offset = now;
    else
    {
        const auto delta = Us((reference - *s.remote_reference) * 64000);
        s.current_offset = delta < Time{} - *s.current_offset ? now : *s.current_offset + delta;
    }
    s.remote_reference = reference;
    TransportPacketsFeedback result; result.feedback_time = now; result.generation = s.stats.generation; result.feedback_count = f.feedbackCount;
    result.packet_feedbacks.reserve(f.deltas.size());
    std::int64_t offset = 0; const auto base = s.sequence(f.baseSequence);
    std::size_t failed = 0, ignored = 0;
    for (std::size_t i = 0; i < f.deltas.size(); ++i)
    {
        const bool received = f.deltas[i].has_value(); if (received) offset += std::int64_t(*f.deltas[i]) * 250;
        const auto seq = base + static_cast<std::int64_t>(i);
        const auto packet = s.retrieve(seq, received);
        if (!packet) { ++failed; continue; }
        if (!(packet->network_route == s.route)) { ++ignored; continue; }
        PacketResult p; p.sent_packet = packet->sent; p.wire_size = packet->wire_size;
        p.rtp_packet_info = PacketResult::RtpPacketInfo{packet->ssrc, packet->rtp_sequence_number, packet->is_retransmission};
        p.ambiguous_receive_time = packet->ambiguous_receive_time;
        p.reported_lost_for_the_first_time = !received && !packet->previously_reported_lost;
        p.reported_recovered_for_the_first_time = received && packet->previously_reported_lost;
        if (received)
        { p.receive_time = *s.current_offset + Us(floorMs(offset)); p.remote_arrival_us = reference * 64000 + offset; }
        result.packet_feedbacks.push_back(std::move(p));
    }
    s.stats.failed_lookups += failed; s.stats.ignored_route_packets += ignored; ++s.stats.feedback_packets;
    // Lost results intentionally repeat in the raw upstream view; received
    // results are removed from history, so duplicates cannot be delivered twice.
    if (result.packet_feedbacks.empty()) return std::nullopt;
    result.data_in_flight = s.outstanding();
    UAV_LOG_DEBUG << "[WebRTC][TransportAdapter][g=" << result.generation << "] feedback=" << unsigned(f.feedbackCount)
        << " packets=" << result.packet_feedbacks.size() << " in_flight=" << result.data_in_flight;
    return result;
}
bool TransportFeedbackAdapter::ProcessRtcp(const RtcpBytes& bytes, Time now, std::vector<TransportPacketsFeedback>& out)
{
    std::vector<RtcpMessage> messages;
    if (!parseRtcp(bytes.data(), bytes.size(), messages)) { ++impl_->stats.malformed_feedback; return false; }
    std::vector<TransportPacketsFeedback> results;
    for (const auto& message : messages) if (const auto* twcc = std::get_if<RtcpTwcc>(&message))
        if (auto result = ProcessTransportFeedback(*twcc, now)) results.push_back(std::move(*result));
    out = std::move(results); return true;
}
void TransportFeedbackAdapter::SetNetworkRoute(const TransportNetworkRoute& route) { impl_->route = route; }
std::size_t TransportFeedbackAdapter::GetOutstandingData() const { return impl_->outstanding(); }
bool TransportFeedbackAdapter::RemovePacket(std::int64_t sequence)
{
    auto& s = *impl_; const auto it = s.history.find(sequence); if (it == s.history.end()) return false;
    s.removeInFlight(it->second); s.removeIndex(it->second); s.history.erase(it); ++s.stats.failed_sends; return true;
}
bool TransportFeedbackAdapter::MarkRetransmitted(std::int64_t sequence, Time at)
{
    auto& s = *impl_; const auto it = s.history.find(sequence);
    if (it == s.history.end() || !it->second.sent.send_time) return false;
    s.sent(it->second, at); return true;
}
void TransportFeedbackAdapter::Prune(Time now) { impl_->prune(now, impl_->config.max_history); }
void TransportFeedbackAdapter::Clear() { impl_->clear(); }
void TransportFeedbackAdapter::Reset(std::uint64_t generation)
{ auto& s = *impl_; s.clear(true); s.sequence_anchor.reset(); s.route = {}; s.stats = {}; s.stats.generation = generation; }
TransportAdapterStats TransportFeedbackAdapter::Snapshot() const
{
    auto result = impl_->stats; result.history_size = impl_->history.size(); result.data_in_flight = impl_->outstanding();
    result.pending_untracked_size = impl_->pending_untracked_size; return result;
}
std::vector<TransportPacketDelta> TransportFeedbackDeltaCalculator::Calculate(const TransportPacketsFeedback& feedback)
{
    if (generation_ != feedback.generation) { Reset(); generation_ = feedback.generation; }
    std::vector<TransportPacketDelta> result;
    for (const auto& packet : feedback.SortedByReceiveTime())
    {
        if (packet.ambiguous_receive_time) continue;
        if (previous_)
        {
            if (packet.sent_packet.sequence_number <= previous_->sent_packet.sequence_number ||
                *packet.sent_packet.send_time < *previous_->sent_packet.send_time || *packet.receive_time < *previous_->receive_time) continue;
            const auto sent = std::chrono::duration_cast<Us>(*packet.sent_packet.send_time - *previous_->sent_packet.send_time);
            const auto received = packet.remote_arrival_us && previous_->remote_arrival_us ?
                Us(*packet.remote_arrival_us - *previous_->remote_arrival_us) : std::chrono::duration_cast<Us>(*packet.receive_time - *previous_->receive_time);
            if (received.count() < 0) continue;
            result.push_back({previous_->sent_packet.sequence_number, packet.sent_packet.sequence_number, sent, received, received - sent});
        }
        previous_ = packet;
    }
    return result;
}
void TransportFeedbackDeltaCalculator::Reset() { previous_.reset(); generation_.reset(); }
} // namespace webrtc
