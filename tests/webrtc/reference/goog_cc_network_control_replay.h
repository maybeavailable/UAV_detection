// Shared input and output hashing only; contains no congestion-control algorithm.
#pragma once
#include<algorithm>
#include<cstdint>
#include<cmath>
#include<cstdlib>
#include<iostream>

namespace goog_cc_replay
{
using namespace webrtc;
#ifdef UAV_CC_UPSTREAM_REFERENCE
inline auto T(std::int64_t us) { return Timestamp::Micros(us); }
inline auto R(std::uint64_t bps) { return DataRate::BitsPerSec(bps); }
inline auto S(std::size_t bytes) { return DataSize::Bytes(bytes); }
inline auto D(std::int64_t us) { return TimeDelta::Micros(us); }
inline std::uint64_t rate(DataRate r) { return r.bps(); }
inline std::uint64_t bytes(DataSize s) { return s.bytes(); }
inline std::int64_t duration(TimeDelta d) { return d.us(); }
inline std::int64_t timestamp(Timestamp t) { return t.us(); }
inline bool rttAvailable(TimeDelta rtt) { return rtt.IsFinite(); }
inline std::int64_t rttValue(TimeDelta rtt) { return rtt.us(); }
inline void setClock(std::int64_t us) { reference_now_us = us; }
#else
inline auto T(std::int64_t us) { return TransportTime(std::chrono::microseconds(us)); }
inline auto R(std::uint64_t bps) { return bps; }
inline auto S(std::size_t bytes) { return bytes; }
inline auto D(std::int64_t us) { return std::chrono::microseconds(us); }
inline std::uint64_t rate(std::uint64_t r) { return r; }
inline std::uint64_t bytes(std::size_t s) { return s; }
inline std::int64_t duration(std::chrono::microseconds d) { return d.count(); }
inline std::int64_t timestamp(TransportTime t) { return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); }
inline bool rttAvailable(std::optional<std::chrono::microseconds> rtt) { return rtt.has_value(); }
inline std::int64_t rttValue(std::optional<std::chrono::microseconds> rtt) { return rtt->count(); }
inline void setClock(std::int64_t) {}
#endif
struct Hash
{
    std::uint64_t hash{14695981039346656037ULL}, updates{0}, targets{0}, probes{0};
    void mix(std::uint64_t v)
    {
        static const bool trace = std::getenv("UAV_CC_REPLAY_TRACE") != nullptr;
        if (trace) std::cerr << updates << ' ' << v << '\n';
        hash = (hash ^ v) * 1099511628211ULL;
    }
    void consume(const NetworkControlUpdate& u)
    {
        ++updates;
        mix(u.congestion_window.has_value()); if (u.congestion_window) mix(bytes(*u.congestion_window));
        mix(u.pacer_config.has_value());
        if (u.pacer_config)
        {
            const auto& p = *u.pacer_config;
            mix(timestamp(p.at_time)); mix(bytes(p.data_window)); mix(bytes(p.pad_window)); mix(duration(p.time_window));
            mix(rate(p.data_rate())); mix(rate(p.pad_rate()));
        }
        mix(u.target_rate.has_value());
        if (u.target_rate)
        {
            ++targets; const auto& r = *u.target_rate;
            mix(timestamp(r.at_time)); mix(rate(r.target_rate)); mix(std::llround(r.cwnd_reduce_ratio * 1000000000));
            mix(r.is_bandwidth_limited); mix(rttAvailable(r.network_estimate.round_trip_time));
            if (rttAvailable(r.network_estimate.round_trip_time)) mix(rttValue(r.network_estimate.round_trip_time));
            mix(std::llround(r.network_estimate.loss_rate_ratio * 1000000000.0));
        }
        mix(u.probe_cluster_configs.size()); probes += u.probe_cluster_configs.size();
        for (const auto& p : u.probe_cluster_configs)
        {
            mix(p.id); mix(timestamp(p.at_time)); mix(rate(p.target_data_rate)); mix(duration(p.target_duration));
            mix(duration(p.min_probe_delta)); mix(p.target_probe_count);
        }
    }
};
inline Hash run(int scenario, GoogCcNetworkController& c)
{
    constexpr std::int64_t base = 1000000000;
    Hash h; setClock(base);
    h.consume(c.OnNetworkAvailability({T(base), true})); h.consume(c.OnProcessInterval({T(base), {}}));
    std::int64_t queue_delay = 0;
    for (int i = 0; i < 600; ++i)
    {
        const std::int64_t at = base + i * 100000LL;
        setClock(at);
        if (i == 150 || i == 155) h.consume(c.OnNetworkAvailability({T(at), i == 155}));
        if (i == 100 || i == 350)
        {
            TargetRateConstraints m; m.at_time = T(at); m.min_data_rate = R(20000); m.max_data_rate = R(3000000);
            if (i == 350) m.starting_rate = R(500000);
            h.consume(c.OnTargetRateConstraints(m));
        }
        if (i == 200 || i == 420)
        {
            StreamsConfig m; m.at_time = T(at); m.requests_alr_probing = true;
            m.pacing_factor = i == 200 ? 1.5 : 3.0; m.min_total_allocated_bitrate = R(i == 200 ? 100000 : 0);
            m.max_total_allocated_bitrate = R(1200000); m.max_padding_rate = R(100000);
            h.consume(c.OnStreamsConfig(m));
        }
        if (i == 300)
        {
            NetworkRouteChange m; m.at_time = T(at); m.constraints.at_time = T(at);
            m.constraints.starting_rate = R(700000); m.constraints.max_data_rate = R(5000000);
            h.consume(c.OnNetworkRouteChange(m));
        }
        if (i % 60 == 10)
        {
            RoundTripTimeUpdate m; m.receive_time = T(at); m.round_trip_time = D(i % 120 ? 50000 : 200000); m.smoothed = i % 3 == 0;
            h.consume(c.OnRoundTripTimeUpdate(m));
        }
        if (i == 70 || i == 130)
        {
            RemoteBitrateReport m; m.receive_time = T(at); m.bandwidth = R(i == 70 ? 400000 : 1500000);
            h.consume(c.OnRemoteBitrateReport(m));
        }
        if (i % 40 == 0)
        {
            NetworkStateEstimate e; e.update_time = T(at); e.link_capacity_lower = R(400000); e.link_capacity_upper = R(800000);
            h.consume(c.OnNetworkStateEstimate(e));
        }
        if (i >= 80 && i < 160) queue_delay += 4000;
        if (i >= 220 && i < 300) queue_delay -= 4000;
        TransportPacketsFeedback feedback; feedback.feedback_time = T(at + 90000); feedback.data_in_flight = S(i % 7 ? 0 : 6000);
        for (int j = 0; j < 6; ++j)
        {
            SentPacket sent; sent.send_time = T(at + j * 5000); sent.size = S(i >= 360 && i < 430 ? 60 : (j % 2 ? 1000 : 1200));
            sent.sequence_number = i * 6 + j; sent.prior_unacked_data = S(i % 19 == 0 ? 100 : 0);
            sent.data_in_flight = S(j * 1200 + (i % 40 < 8 ? 30000 : 0));
            if (i == 0 || i == 1 || i == 120 || i == 450)
            {
                sent.pacing_info.probe_cluster_id = i + 1; sent.pacing_info.probe_cluster_min_probes = 5;
                sent.pacing_info.probe_cluster_min_bytes = 5000;
            }
            setClock(at + j * 5000); h.consume(c.OnSentPacket(sent));
            PacketResult packet; packet.sent_packet = sent;
            if (!(i % 90 >= 50 && i % 90 < 70 && j >= 4))
                packet.receive_time = T(at + 1000000 + queue_delay + j * (i == 120 ? 15000 : 5000));
            feedback.packet_feedbacks.push_back(packet);
        }
        if (i % 2) std::reverse(feedback.packet_feedbacks.begin(), feedback.packet_feedbacks.end());
        setClock(at + 90000); h.consume(c.OnTransportPacketsFeedback(feedback));
        if (scenario == 6 && i % 10 == 9)
        {
            TransportLossReport m; m.receive_time = T(at + 90000); m.packets_lost_delta = i % 30 == 29 ? 20 : 0;
            m.packets_received_delta = 100 - m.packets_lost_delta; h.consume(c.OnTransportLossReport(m));
        }
        setClock(at + 95000); h.consume(c.OnProcessInterval({T(at + 95000), S(i % 40 < 8 ? 40000 : 0)}));
        h.consume(c.GetNetworkState(T(at + 95000)));
    }
    return h;
}
} // namespace goog_cc_replay
