#include"webrtc/rtcp_session.h"
#include"common/rtp_packet.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<deque>
#include<map>
#include<random>
#include<set>
#include<stdexcept>

namespace webrtc
{
namespace
{
using Time = RtcpSession::Time;
std::int64_t unwrap(std::uint64_t value, std::int64_t near, unsigned bits)
{
    const auto modulus = std::int64_t(1) << bits;
    auto result = (near & ~(modulus - 1)) | static_cast<std::int64_t>(value);
    if (result - near > modulus / 2) result -= modulus;
    else if (near - result > modulus / 2) result += modulus;
    return result;
}
std::int64_t micros(Time t) { return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); }
void append(RtcpBytes& b, const RtcpBytes& tail) { b.insert(b.end(), tail.begin(), tail.end()); }
std::optional<std::map<std::uint8_t, RtcpMediaParameters>> parameterMap(const RtcpParameters& p)
{
    std::map<std::uint8_t, RtcpMediaParameters> next;
    std::optional<std::uint8_t> sendId, receiveId;
    for (const auto& m : p.media)
    {
        if (m.payloadType > 127 || m.clockRate == 0 || m.clockRate > 1000000 ||
            (m.sendTwccExtension && !*m.sendTwccExtension) || (m.receiveTwccExtension && !*m.receiveTwccExtension)) return std::nullopt;
        // A BUNDLE transport has one sequence space; conflicting extension IDs cannot be interpreted safely.
        if (m.transportCc && m.send && m.sendTwccExtension)
        { if (sendId && sendId != m.sendTwccExtension) return std::nullopt; sendId = m.sendTwccExtension; }
        if (m.transportCc && m.receive && m.receiveTwccExtension)
        { if (receiveId && receiveId != m.receiveTwccExtension) return std::nullopt; receiveId = m.receiveTwccExtension; }
        const auto old = next.find(m.payloadType);
        if (old != next.end())
        {
            const auto& n = old->second;
            if (n.clockRate != m.clockRate || n.send != m.send || n.receive != m.receive || n.nack != m.nack ||
                n.pli != m.pli || n.fir != m.fir || n.transportCc != m.transportCc ||
                n.sendTwccExtension != m.sendTwccExtension || n.receiveTwccExtension != m.receiveTwccExtension || n.audio != m.audio) return std::nullopt;
        }
        next[m.payloadType] = m;
    }
    return next;
}
} // namespace

bool validateRtcpParameters(const RtcpParameters& p) { return parameterMap(p).has_value(); }

struct RtcpSession::Impl
{
    struct Missing { Time first, last; unsigned retries{0}; };
    struct Receiver
    {
        std::uint8_t pt{0}; std::uint32_t rate{0}, timestamp{0}, lsr{0};
        std::int64_t base{0}, highest{0}; std::uint64_t received{0}, previousExpected{0}, previousReceived{0};
        std::optional<std::uint16_t> badSequence;
        std::set<std::int64_t> seen;
        std::map<std::int64_t, Missing> missing;
        Time arrival{}, srAt{}, lastPli{};
        double jitter{0}; bool initialized{false}, hasPli{false}, hasMediaPayload{false};
    };
    struct Sender
    {
        std::uint8_t pt{0}; std::uint32_t rate{0}, timestamp{0};
        std::uint64_t packets{0}, octets{0}; Time lastSend{}, lastKeyFrame{};
        std::map<std::uint32_t, std::uint8_t> firSequences;
        bool hasKeyFrame{false};
        std::map<std::uint32_t, Time> reports;
    };
    struct Cached
    {
        std::uint32_t ssrc; std::uint16_t sequence; RtcpBytes bytes; Time sentAt, lastRetransmit{}; unsigned retries{0};
        std::optional<std::int64_t> transportSequence;
    };
    Config config; Callbacks callbacks; RtcpParameters parameters;
    RtcpStats stats;
    std::map<std::uint8_t, RtcpMediaParameters> media;
    std::map<std::uint32_t, Sender> senders;
    std::map<std::uint32_t, Receiver> receivers;
    std::deque<Cached> cache;
    TransportFeedbackAdapter transport;
    TransportFeedbackDeltaCalculator transportDeltas;
    std::map<std::int64_t, std::int64_t> arrivals, lateArrivals;
    std::vector<Output> output;
    std::optional<Time> reportAt, twccAt;
    std::int64_t nextTransport{0}, twccHighest{0}, twccCursor{0};
    std::uint8_t feedbackCount{0}; bool twccInitialized{false};

    Impl(Config c, Callbacks cb) : config(std::move(c)), callbacks(std::move(cb)),
        transport(TransportFeedbackAdapter::Config{config.historyAge, config.maxTwccHistory})
    {
        if (config.reportInterval.count() < 10 || config.tickInterval.count() < 1 || config.tickInterval.count() > 1000 ||
            config.twccInterval.count() < 5 || config.nackDelay.count() < 0 || config.nackRetry.count() < 1 ||
            config.historyAge.count() < 1 || config.pliInterval.count() < 1 || !config.maxNackRetries || config.maxNackRetries > 20 ||
            !config.maxStreams || config.maxStreams > 256 || !config.maxMissing || config.maxMissing > 4096 ||
            !config.maxCachedPackets || config.maxCachedPackets > 32768 || config.maxCachedBytes < 1200 || config.maxCachedBytes > 64 * 1024 * 1024 ||
            !config.maxTwccHistory || config.maxTwccHistory > 32768 || !config.maxOutputPackets || config.maxOutputPackets > 256 ||
            config.maxDatagramSize < 512 || config.maxDatagramSize > 65507 || config.cname.size() > 255)
            throw std::invalid_argument("invalid RTCP session limits");
        if (!config.senderSsrc) { std::random_device random; do { config.senderSsrc = random(); } while (!config.senderSsrc); }
        if (config.cname.empty()) config.cname = "webrtc-" + std::to_string(config.senderSsrc);
        if (!configure(config.parameters)) throw std::invalid_argument("invalid RTCP media parameters");
    }
    bool configure(RtcpParameters p)
    {
        auto next = parameterMap(p); if (!next) return false;
        parameters = std::move(p); media = std::move(*next);
        // Preserve counters across renegotiation, but discard history for removed capabilities/streams.
        for (auto it = senders.begin(); it != senders.end();) if (!find(it->second.pt, true)) it = senders.erase(it); else ++it;
        for (auto it = receivers.begin(); it != receivers.end();) if (!find(it->second.pt, false)) it = receivers.erase(it); else ++it;
        cache.clear(); stats.cachedBytes = 0; transport.Clear(); transportDeltas.Reset(); arrivals.clear(); lateArrivals.clear(); twccInitialized = false;
        output.clear(); return true;
    }
    const RtcpMediaParameters* find(std::uint8_t pt, bool send) const
    {
        const auto it = media.find(pt);
        return it != media.end() && (send ? it->second.send : it->second.receive) ? &it->second : nullptr;
    }
    bool queue(RtcpBytes bytes, bool retransmission = false, std::optional<std::int64_t> transportSequence = {})
    {
        if (output.size() >= config.maxOutputPackets || (!retransmission && bytes.size() > config.maxDatagramSize))
        { ++stats.droppedOutputs; return false; }
        output.push_back({std::move(bytes), retransmission, transportSequence}); return true;
    }
    RtcpBytes feedbackPrefix() const
    {
        if (parameters.reducedSize) return {};
        RtcpReport rr; rr.senderSsrc = config.senderSsrc;
        auto b = buildRtcpReport(rr); append(b, buildRtcpSdes(config.senderSsrc, config.cname)); return b;
    }
    void prune(Time now)
    {
        while (!cache.empty() && (now - cache.front().sentAt > config.historyAge || cache.size() > config.maxCachedPackets || stats.cachedBytes > config.maxCachedBytes))
        { stats.cachedBytes -= cache.front().bytes.size(); cache.pop_front(); }
        transport.Prune(now);
        for (auto& entry : senders)
            for (auto it = entry.second.reports.begin(); it != entry.second.reports.end();)
                if (now - it->second > std::chrono::seconds(60)) it = entry.second.reports.erase(it); else ++it;
    }
    void activity(Time now) { if (!reportAt) reportAt = now + config.reportInterval; }
    bool prepare(RtcpBytes& bytes, bool assignSequence = true)
    {
        RtpPacketParser r(bytes.data(), bytes.size()); if (!r.valid()) return false;
        const auto* m = find(r.payloadType(), true); if (!config.enabled || !m || !m->transportCc || !m->sendTwccExtension) return true;
        const auto sequence = assignSequence ? static_cast<std::uint16_t>(nextTransport) : rtpTransportSequence(bytes, *m->sendTwccExtension).value_or(0);
        if (!setRtpTransportSequence(bytes, *m->sendTwccExtension, sequence)) return false;
        if (assignSequence) ++nextTransport;
        return true;
    }
    void untracked(std::size_t bytes, Time now, bool audio = false)
    {
        (void)transport.ProcessSentPacket({-1, now, {false, true, bytes}});
        if (callbacks.onSentPacket)
        {
            SentPacket sent; sent.send_time = now; sent.size = bytes; sent.audio = audio; sent.data_in_flight = transport.GetOutstandingData();
            callbacks.onSentPacket(sent);
        }
    }
    std::optional<std::int64_t> sending(const RtcpBytes& bytes, std::size_t wireSize, Time created, Time now, std::size_t overhead,
        const PacedPacketInfo& pacing = {})
    {
        if (!config.enabled) return std::nullopt;
        RtpPacketParser r(bytes.data(), bytes.size()); if (!r.valid()) return std::nullopt;
        const auto* m = find(r.payloadType(), true);
        if (!m) return std::nullopt;
        if (!m->transportCc || !m->sendTwccExtension) { untracked(wireSize + overhead, now, m->audio); return std::nullopt; }
        const auto seq = rtpTransportSequence(bytes, *m->sendTwccExtension); if (!seq) return std::nullopt;
        if (!transport.AddPacket({*seq, wireSize, r.ssrc(), r.sequenceNumber(), m->audio, false}, pacing, overhead, created)) return std::nullopt;
        const auto sent = transport.ProcessSentPacket({*seq, now, {true, true, wireSize}});
        if (sent && callbacks.onSentPacket) callbacks.onSentPacket(*sent);
        return sent ? std::optional<std::int64_t>(sent->sequence_number) : std::nullopt;
    }
    void sent(const RtcpBytes& bytes, RtcpBytes wire, Time now, bool recordTransport, std::optional<std::int64_t> transportSequence)
    {
        if (!config.enabled) return;
        RtpPacketParser r(bytes.data(), bytes.size()); if (!r.valid()) return;
        const auto* m = find(r.payloadType(), true); if (!m) return;
        if (recordTransport) transportSequence = sending(bytes, wire.size(), now, now, 0);
        if (!senders.count(r.ssrc()) && senders.size() >= config.maxStreams) return;
        auto& s = senders[r.ssrc()]; s.pt = r.payloadType(); s.rate = m->clockRate; s.timestamp = r.timestamp(); s.lastSend = now;
        ++s.packets; s.octets += r.payloadSize(); activity(now);
        if (m->nack && r.payloadSize() && wire.size() <= config.maxCachedBytes)
        {
            stats.cachedBytes += wire.size(); cache.push_back({r.ssrc(), r.sequenceNumber(), std::move(wire), now, {}, 0, transportSequence});
        }
        prune(now);
    }
    void receiveTwcc(std::uint16_t seq, Time now)
    {
        const auto extended = twccInitialized ? unwrap(seq, twccHighest, 16) : std::int64_t(seq);
        if (!twccInitialized) { twccCursor = extended; twccHighest = extended; twccInitialized = true; }
        if (extended < twccCursor)
        {
            if (twccCursor - extended <= static_cast<std::int64_t>(config.maxTwccHistory) && lateArrivals.size() < config.maxTwccHistory)
                lateArrivals.emplace(extended, micros(now));
        }
        else
        {
            if (extended - twccCursor >= static_cast<std::int64_t>(config.maxTwccHistory)) { arrivals.clear(); twccCursor = extended; }
            if (arrivals.size() < config.maxTwccHistory) arrivals.emplace(extended, micros(now));
            twccHighest = std::max(twccHighest, extended);
        }
        if (!twccAt) twccAt = now + config.twccInterval;
    }
    void received(const RtcpBytes& bytes, Time now)
    {
        if (!config.enabled) return;
        RtpPacketParser r(bytes.data(), bytes.size()); if (!r.valid()) return;
        const auto* m = find(r.payloadType(), false); if (!m) return;
        if (m->transportCc && m->receiveTwccExtension)
            if (const auto seq = rtpTransportSequence(bytes, *m->receiveTwccExtension)) receiveTwcc(*seq, now);
        if (!receivers.count(r.ssrc()) && receivers.size() >= config.maxStreams) return;
        auto& s = receivers[r.ssrc()];
        auto seq = s.initialized ? unwrap(r.sequenceNumber(), s.highest, 16) : std::int64_t(r.sequenceNumber());
        if (s.initialized && seq - s.highest > 3000)
        {
            if (s.badSequence && *s.badSequence == r.sequenceNumber()) { s = Receiver{}; seq = r.sequenceNumber(); }
            else { s.badSequence = static_cast<std::uint16_t>(r.sequenceNumber() + 1); return; }
        }
        if (!s.initialized) { s.base = seq; s.highest = seq; s.initialized = true; }
        s.hasMediaPayload = s.hasMediaPayload || r.payloadSize() != 0;
        if (seq < s.base || seq < s.highest - 4096 || !s.seen.insert(seq).second) return;
        while (!s.seen.empty() && *s.seen.begin() < std::max(s.base, s.highest - 4096)) s.seen.erase(s.seen.begin());
        if (seq > s.highest)
        {
            if (m->nack && s.hasMediaPayload)
                for (auto gap = std::max(s.highest + 1, seq - static_cast<std::int64_t>(config.maxMissing)); gap < seq; ++gap)
                {
                    if (s.missing.size() >= config.maxMissing) s.missing.erase(s.missing.begin());
                    s.missing.emplace(gap, Missing{now, {}, 0});
                }
            s.highest = seq;
        }
        s.missing.erase(seq);
        if (s.received && s.rate == m->clockRate)
        {
            const auto timestampDelta = unwrap(r.timestamp(), s.timestamp, 32) - s.timestamp;
            const auto delta = std::chrono::duration<double>(now - s.arrival).count() * m->clockRate - timestampDelta;
            s.jitter += (std::abs(delta) - s.jitter) / 16.0;
        }
        else s.jitter = 0;
        ++s.received; s.pt = r.payloadType(); s.rate = m->clockRate; s.timestamp = r.timestamp(); s.arrival = now; activity(now);
    }
    void nack(const RtcpNack& f, Time now)
    {
        const auto sender = senders.find(f.mediaSsrc); if (sender == senders.end()) return;
        const auto* m = find(sender->second.pt, true); if (!m || !m->nack) return;
        ++stats.receivedNacks; prune(now); RtcpNackEvent event{stats.generation, f, {}};
        for (auto seq : f.sequences)
        {
            auto it = std::find_if(cache.rbegin(), cache.rend(), [&](const auto& p) { return p.ssrc == f.mediaSsrc && p.sequence == seq; });
            if (it == cache.rend()) { event.cacheMisses.push_back(seq); ++stats.cacheMisses; continue; }
            if (it->retries >= config.maxNackRetries || (it->retries && now - it->lastRetransmit < config.nackDelay)) continue;
            if (queue(it->bytes, true, it->transportSequence)) { ++it->retries; it->lastRetransmit = now; ++stats.retransmissions; }
        }
        UAV_LOG_DEBUG << "[WebRTC][RTCP][g=" << stats.generation << "] NACK ssrc=" << f.mediaSsrc
            << " requested=" << f.sequences.size() << " cache_misses=" << event.cacheMisses.size();
        if (callbacks.onNack) callbacks.onNack(std::move(event));
    }
    void keyFrame(const RtcpKeyFrameRequest& f, Time now)
    {
        const auto sender = senders.find(f.mediaSsrc); if (sender == senders.end()) return;
        auto& s = sender->second; const auto* m = find(s.pt, true);
        if (!m || (f.kind == RtcpKeyFrameRequest::Kind::Pli ? !m->pli : !m->fir)) return;
        if (f.kind == RtcpKeyFrameRequest::Kind::Fir)
        {
            const auto last = s.firSequences.find(f.senderSsrc);
            if (last != s.firSequences.end() && last->second == f.sequence) return;
            if (last == s.firSequences.end() && s.firSequences.size() >= config.maxStreams) return;
        }
        if (s.hasKeyFrame && now - s.lastKeyFrame < config.pliInterval) return;
        s.lastKeyFrame = now; s.hasKeyFrame = true;
        if (f.kind == RtcpKeyFrameRequest::Kind::Fir) s.firSequences[f.senderSsrc] = f.sequence;
        ++stats.receivedKeyFrameRequests;
        UAV_LOG_DEBUG << "[WebRTC][RTCP][g=" << stats.generation << "] key frame requested ssrc=" << f.mediaSsrc;
        if (callbacks.onKeyFrameRequest) callbacks.onKeyFrameRequest({stats.generation, f});
    }
    void report(const RtcpReport& r, Time now)
    {
        ++stats.receivedReports;
        if (r.senderReport)
        {
            const auto receiver = receivers.find(r.senderSsrc);
            if (receiver != receivers.end()) { receiver->second.lsr = static_cast<std::uint32_t>(r.ntp >> 16); receiver->second.srAt = now; }
        }
        for (const auto& block : r.reports)
        {
            const auto sender = senders.find(block.ssrc); if (sender == senders.end()) continue;
            RtcpReportEvent event{stats.generation, r.senderSsrc, block, std::nullopt};
            const auto sentReport = sender->second.reports.find(block.lsr);
            if (block.lsr && sentReport != sender->second.reports.end())
            {
                const double elapsed = std::chrono::duration<double>(now - sentReport->second).count();
                const double rtt = elapsed - double(block.dlsr) / 65536.0;
                if (rtt >= 0 && rtt <= 60) { event.rttMs = rtt * 1000; stats.lastRttMs = event.rttMs; }
            }
            if (callbacks.onReport) callbacks.onReport(std::move(event));
        }
    }
    void twccFeedback(const RtcpTwcc& f, Time now)
    {
        auto feedback = transport.ProcessTransportFeedback(f, now); if (!feedback) return;
        TwccFeedbackEvent event; event.generation = stats.generation; event.feedbackCount = f.feedbackCount; event.feedbackAt = now;
        event.packetDeltas = transportDeltas.Calculate(*feedback);
        for (const auto& p : feedback->packet_feedbacks)
        {
            // Keep the existing callback's changed-status view. The new raw
            // TransportPacketsFeedback view follows upstream repeated-loss semantics.
            if (!p.IsReceived() && !p.reported_lost_for_the_first_time) continue;
            TwccPacketResult packet; packet.transportSequence = static_cast<std::uint64_t>(p.sent_packet.sequence_number);
            packet.ssrc = p.rtp_packet_info->ssrc; packet.rtpSequence = p.rtp_packet_info->rtp_sequence_number;
            packet.wireSize = p.wire_size; packet.sentAt = *p.sent_packet.send_time;
            packet.remoteArrivalUs = p.remote_arrival_us; packet.ambiguousReceiveTime = p.ambiguous_receive_time;
            if (p.IsReceived()) { ++event.received; event.receivedBytes += packet.wireSize; }
            else ++event.notReceived;
            event.packets.push_back(std::move(packet));
        }
        if (!event.packets.empty())
        {
            ++stats.receivedTwcc;
            if (callbacks.onTwccFeedback) callbacks.onTwccFeedback(std::move(event));
        }
        if (callbacks.onTransportFeedback) callbacks.onTransportFeedback(std::move(*feedback));
    }
    bool control(const RtcpBytes& b, Time now)
    {
        std::vector<RtcpMessage> messages;
        if (!parseRtcp(b.data(), b.size(), messages))
        {
            const auto n = ++stats.malformedPackets;
            UAV_LOG_IF(LogLevel::WARN, (n & (n - 1)) == 0) << "[WebRTC][RTCP][g=" << stats.generation << "] malformed packets=" << n;
            return false;
        }
        if (!config.enabled) return true;
        for (const auto& message : messages)
        {
            if (const auto* r = std::get_if<RtcpReport>(&message)) report(*r, now);
            else if (const auto* n = std::get_if<RtcpNack>(&message)) nack(*n, now);
            else if (const auto* k = std::get_if<RtcpKeyFrameRequest>(&message)) keyFrame(*k, now);
            else if (const auto* t = std::get_if<RtcpTwcc>(&message)) twccFeedback(*t, now);
            else if (const auto* remb = std::get_if<RtcpRemb>(&message))
            {
                const bool ours = std::any_of(remb->ssrcs.begin(), remb->ssrcs.end(), [&](auto ssrc) { return senders.count(ssrc) != 0; });
                if (ours && callbacks.onRemb) callbacks.onRemb({stats.generation, *remb});
            }
        }
        return true;
    }
    void reports(Time now, std::uint64_t ntp)
    {
        std::vector<RtcpReceptionReport> blocks;
        for (auto& entry : receivers)
        {
            auto& r = entry.second;
            const auto expected = std::uint64_t(r.highest - r.base + 1);
            const auto interval = expected - r.previousExpected;
            const auto lost = std::int64_t(expected) - std::int64_t(r.received);
            const auto intervalLost = std::int64_t(interval) - std::int64_t(r.received - r.previousReceived);
            const auto fraction = interval && intervalLost > 0 ? std::min<std::int64_t>(255, intervalLost * 256 / interval) : 0;
            r.previousExpected = expected; r.previousReceived = r.received;
            const auto delay = r.lsr ? std::clamp(std::chrono::duration<double>(now - r.srAt).count() * 65536, 0.0, 4294967295.0) : 0;
            blocks.push_back({entry.first, static_cast<std::uint8_t>(fraction), static_cast<std::int32_t>(std::clamp<std::int64_t>(lost, -8388608, 8388607)),
                static_cast<std::uint32_t>(r.highest), static_cast<std::uint32_t>(std::min(r.jitter, 4294967295.0)), r.lsr, static_cast<std::uint32_t>(delay)});
        }
        std::size_t cursor = 0;
        auto emit = [&](RtcpReport report) {
            const auto sdes = buildRtcpSdes(report.senderSsrc, config.cname);
            const auto baseSize = report.senderReport ? 28u : 8u;
            const auto count = std::min<std::size_t>({31, blocks.size() - cursor, (config.maxDatagramSize - baseSize - sdes.size()) / 24});
            report.reports.insert(report.reports.end(), blocks.begin() + cursor, blocks.begin() + cursor + count); cursor += count;
            auto b = buildRtcpReport(report); append(b, sdes);
            if (queue(std::move(b)))
            {
                if (report.senderReport)
                {
                    ++stats.sentSenderReports; auto& sent = senders.at(report.senderSsrc).reports;
                    sent[static_cast<std::uint32_t>(ntp >> 16)] = now; if (sent.size() > 128) sent.erase(sent.begin());
                }
                else ++stats.sentReceiverReports;
            }
        };
        bool sentSr = false;
        for (auto& entry : senders)
        {
            const auto& s = entry.second;
            if (now - s.lastSend > config.reportInterval * 2) continue;
            RtcpReport r; r.senderSsrc = entry.first; r.senderReport = true; r.ntp = ntp;
            r.rtpTimestamp = s.timestamp + static_cast<std::uint32_t>(std::chrono::duration<double>(now - s.lastSend).count() * s.rate);
            r.packetCount = s.packets; r.octetCount = s.octets; emit(std::move(r)); sentSr = true;
        }
        if (!sentSr || cursor < blocks.size())
            do { RtcpReport r; r.senderSsrc = config.senderSsrc; emit(std::move(r)); } while (cursor < blocks.size());
        UAV_LOG_DEBUG << "[WebRTC][RTCP][g=" << stats.generation << "] periodic reports send_streams=" << senders.size()
            << " receive_streams=" << receivers.size() << " total_sr=" << stats.sentSenderReports << " total_rr=" << stats.sentReceiverReports;
    }
    void nacks(Time now)
    {
        const auto prefix = feedbackPrefix(); const auto maxEntries = (config.maxDatagramSize - prefix.size() - 12) / 4;
        for (auto& entry : receivers)
        {
            auto& r = entry.second; const auto* m = find(r.pt, false); if (!m || !m->nack) continue;
            RtcpNack f{config.senderSsrc, entry.first, {}};
            for (auto it = r.missing.begin(); it != r.missing.end();)
            {
                auto& gap = it->second;
                if (now - gap.first > config.historyAge || gap.retries >= config.maxNackRetries) { it = r.missing.erase(it); continue; }
                if (f.sequences.size() < maxEntries && now - gap.first >= config.nackDelay && (!gap.retries || now - gap.last >= config.nackRetry))
                { f.sequences.push_back(static_cast<std::uint16_t>(it->first)); ++gap.retries; gap.last = now; }
                ++it;
            }
            if (!f.sequences.empty()) { auto b = prefix; append(b, buildRtcpNack(f)); if (queue(std::move(b))) ++stats.sentNacks; }
        }
    }
    void twccReports(Time now)
    {
        if (!twccAt || now < *twccAt) return;
        const auto prefix = feedbackPrefix();
        const auto maxCount = std::min<std::size_t>(256, (config.maxDatagramSize - prefix.size() - 24) / 3);
        auto emit = [&](std::int64_t base, const std::map<std::int64_t, std::int64_t>& times, std::int64_t highest) -> std::size_t {
            const auto first = times.lower_bound(base); if (first == times.end()) return 0;
            const auto reference = first->second / 64000; auto previous = reference * 64000;
            RtcpTwcc f; f.senderSsrc = config.senderSsrc; f.baseSequence = base; f.feedbackCount = feedbackCount;
            f.referenceTime = static_cast<std::uint32_t>(reference) & 0xffffff;
            for (std::size_t i = 0; i < maxCount && base + static_cast<std::int64_t>(i) <= highest; ++i)
            {
                const auto found = times.find(base + static_cast<std::int64_t>(i));
                if (found == times.end()) f.deltas.push_back(std::nullopt);
                else
                {
                    const auto delta = std::llround(double(found->second - previous) / 250.0);
                    if (delta < -32768 || delta > 32767) break;
                    f.deltas.push_back(static_cast<std::int32_t>(delta)); previous += delta * 250;
                }
            }
            if (f.deltas.empty()) return 0;
            auto b = prefix; append(b, buildRtcpTwcc(f)); if (!queue(std::move(b))) return 0;
            ++feedbackCount; ++stats.sentTwcc; return f.deltas.size();
        };
        while (!lateArrivals.empty() && output.size() < config.maxOutputPackets)
        {
            const auto first = lateArrivals.begin()->first;
            if (!emit(first, lateArrivals, first)) break;
            lateArrivals.erase(lateArrivals.begin());
        }
        while (!arrivals.empty() && twccCursor <= twccHighest && output.size() < config.maxOutputPackets)
        {
            const auto count = emit(twccCursor, arrivals, twccHighest); if (!count) break;
            twccCursor += count; arrivals.erase(arrivals.begin(), arrivals.lower_bound(twccCursor));
        }
        if (arrivals.empty() && lateArrivals.empty()) twccAt.reset(); else twccAt = now + config.twccInterval;
    }
    bool pli(std::uint32_t ssrc, Time now)
    {
        if (!config.enabled) return false;
        const auto receiver = receivers.find(ssrc); if (receiver == receivers.end()) return false;
        auto& r = receiver->second; const auto* m = find(r.pt, false);
        if (!m || !m->pli || (r.hasPli && now - r.lastPli < config.pliInterval)) return false;
        auto b = feedbackPrefix(); append(b, buildRtcpKeyFrameRequest({config.senderSsrc, ssrc, RtcpKeyFrameRequest::Kind::Pli, 0}));
        if (!queue(std::move(b))) return false;
        r.hasPli = true; r.lastPli = now; ++stats.sentPlis; return true;
    }
};
RtcpSession::RtcpSession(Config config, Callbacks callbacks) : impl_(std::make_unique<Impl>(std::move(config), std::move(callbacks))) {}
RtcpSession::RtcpSession(Config config) : RtcpSession(std::move(config), Callbacks{}) {}
RtcpSession::~RtcpSession() = default;
bool RtcpSession::configure(RtcpParameters p) { return impl_->configure(std::move(p)); }
bool RtcpSession::prepareRtp(RtcpBytes& b) { return impl_->prepare(b); }
bool RtcpSession::reserveRtpExtensions(RtcpBytes& b) { return impl_->prepare(b, false); }
std::optional<std::int64_t> RtcpSession::sendingRtp(const RtcpBytes& b, std::size_t size, Time created, Time now, std::size_t overhead, const PacedPacketInfo& pacing)
{ return impl_->sending(b, size, created, now, overhead, pacing); }
void RtcpSession::retransmittedRtp(std::int64_t seq, Time now, std::size_t size, std::size_t overhead)
{ impl_->transport.MarkRetransmitted(seq, now); if (size) impl_->untracked(size + overhead, now); }
std::size_t RtcpSession::outstandingData() const { return impl_->transport.GetOutstandingData(); }
void RtcpSession::failedRtp(std::int64_t seq) { impl_->transport.RemovePacket(seq); }
void RtcpSession::sentRtp(const RtcpBytes& b, RtcpBytes wire, Time now, bool record, std::optional<std::int64_t> sequence)
{ impl_->sent(b, std::move(wire), now, record, sequence); }
void RtcpSession::receivedRtp(const RtcpBytes& b, Time now) { impl_->received(b, now); }
bool RtcpSession::receivedRtcp(const RtcpBytes& b, Time now) { return impl_->control(b, now); }
bool RtcpSession::requestPli(std::uint32_t ssrc, Time now) { return impl_->pli(ssrc, now); }
void RtcpSession::poll(Time now, std::uint64_t ntp)
{
    auto& s = *impl_; if (!s.config.enabled || s.media.empty()) return;
    s.prune(now);
    if (s.reportAt && now >= *s.reportAt) { s.reports(now, ntp); s.reportAt = now + s.config.reportInterval; }
    s.nacks(now); s.twccReports(now);
}
std::vector<RtcpSession::Output> RtcpSession::takeOutput() { auto out = std::move(impl_->output); impl_->output.clear(); return out; }
RtcpStats RtcpSession::snapshot() const
{
    auto stats = impl_->stats; stats.sendStreams = impl_->senders.size(); stats.receiveStreams = impl_->receivers.size();
    stats.transport = impl_->transport.Snapshot();
    stats.cachedPackets = impl_->cache.size(); stats.twccHistory = stats.transport.history_size; return stats;
}
void RtcpSession::reset(std::uint64_t generation)
{
    auto& s = *impl_; s.stats = {}; s.stats.generation = generation;
    invalidate();
    s.transport.Reset(generation);
}
void RtcpSession::invalidate()
{
    auto& s = *impl_; s.stats.cachedBytes = 0;
    s.senders.clear(); s.receivers.clear(); s.cache.clear(); s.transport.Clear(); s.transportDeltas.Reset(); s.arrivals.clear(); s.lateArrivals.clear();
    s.output.clear(); s.reportAt.reset(); s.twccAt.reset(); s.nextTransport = 0;
    s.twccInitialized = false; s.feedbackCount = 0;
}
} // namespace webrtc
