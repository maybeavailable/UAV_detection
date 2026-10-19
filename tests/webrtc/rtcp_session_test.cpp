#include"webrtc/rtcp_session.h"
#include"common/rtp_packet.h"

#include<algorithm>
#include<chrono>
#include<iostream>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
RtcpBytes rtp(std::uint16_t seq, std::uint32_t timestamp = 1000, std::uint32_t ssrc = 42)
{
    return {0x80, 96, static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq),
        static_cast<std::uint8_t>(timestamp >> 24), static_cast<std::uint8_t>(timestamp >> 16), static_cast<std::uint8_t>(timestamp >> 8), static_cast<std::uint8_t>(timestamp),
        static_cast<std::uint8_t>(ssrc >> 24), static_cast<std::uint8_t>(ssrc >> 16), static_cast<std::uint8_t>(ssrc >> 8), static_cast<std::uint8_t>(ssrc), 1, 2, 3};
}
std::vector<RtcpMessage> parse(const RtcpBytes& b) { std::vector<RtcpMessage> m; CHECK(parseRtcp(b.data(), b.size(), m)); return m; }
void codecs()
{
    RtcpReport sr; sr.senderSsrc = 42; sr.senderReport = true; sr.ntp = 0x123456789abcdef0; sr.rtpTimestamp = 800; sr.packetCount = 99; sr.octetCount = 1000;
    sr.reports.push_back({43, 128, -2, 65537, 40, 500, 65536});
    auto b = buildRtcpReport(sr); auto sdes = buildRtcpSdes(42, "test"); b.insert(b.end(), sdes.begin(), sdes.end());
    auto m = parse(b); const auto& r = std::get<RtcpReport>(m[0]);
    CHECK(r.ntp == sr.ntp && r.packetCount == 99 && r.reports[0].cumulativeLost == -2 && r.reports[0].highestSequence == 65537);
    auto n = parse(buildRtcpNack({10, 42, {65534, 65535, 0, 2, 40}})); CHECK(std::get<RtcpNack>(n[0]).sequences == std::vector<std::uint16_t>({65534, 65535, 0, 2, 40}));
    for (auto kind : {RtcpKeyFrameRequest::Kind::Pli, RtcpKeyFrameRequest::Kind::Fir})
    {
        const auto f = std::get<RtcpKeyFrameRequest>(parse(buildRtcpKeyFrameRequest({9, 42, kind, 7}))[0]);
        CHECK(f.mediaSsrc == 42 && f.kind == kind && f.senderSsrc == 9);
    }
    RtcpTwcc tw; tw.senderSsrc = 3; tw.baseSequence = 65534; tw.referenceTime = 0xffffff; tw.feedbackCount = 255;
    tw.deltas = {0, 255, std::nullopt, -1, 256, -32768, 32767}; tw.deltas.insert(tw.deltas.end(), 30, std::nullopt);
    auto wire = buildRtcpTwcc(tw); const auto decoded = std::get<RtcpTwcc>(parse(wire)[0]);
    CHECK(decoded.deltas == tw.deltas && decoded.referenceTime == 0xffffff && decoded.baseSequence == 65534);
    // Independent one-bit status vector: received, missing, received; then uint8 deltas 1,2.
    RtcpBytes one{0x8f, 205, 0, 5, 0, 0, 0, 3, 0, 0, 0, 0, 0xff, 0xfe, 0, 3, 0, 0, 0, 9, 0xa8, 0, 1, 2};
    CHECK(std::get<RtcpTwcc>(parse(one)[0]).deltas == std::vector<std::optional<std::int32_t>>({1, std::nullopt, 2}));
    auto unchanged = m; wire[20] = 0xf0; CHECK(!parseRtcp(wire.data(), wire.size(), m)); CHECK(m.size() == unchanged.size());
    wire = buildRtcpTwcc(tw); wire.pop_back(); CHECK(!parseRtcp(wire.data(), wire.size(), m));
    // Truncated compound tail cannot trigger a NACK/PLI from an otherwise valid prefix.
    auto bad = buildRtcpKeyFrameRequest({9, 42, RtcpKeyFrameRequest::Kind::Pli, 0}); bad.insert(bad.end(), {0x80, 200, 0, 6});
    CHECK(!parseRtcp(bad.data(), bad.size(), m));
    auto padded = buildRtcpTwcc(tw); padded[0] |= 0x20; padded.insert(padded.end(), {0, 0, 0, 4});
    auto length = padded.size() / 4 - 1; padded[2] = length >> 8; padded[3] = length; CHECK(parseRtcp(padded.data(), padded.size(), m));
    padded.back() = 255; CHECK(!parseRtcp(padded.data(), padded.size(), m));
}
void extensions()
{
    auto b = rtp(1); CHECK(setRtpTransportSequence(b, 5, 65535)); CHECK(rtpTransportSequence(b, 5) == 65535);
    CHECK(setRtpTransportSequence(b, 3, 1)); CHECK(setRtpTransportSequence(b, 5, 0)); CHECK(rtpTransportSequence(b, 3) == 1 && rtpTransportSequence(b, 5) == 0);
    RtpPacketParser p(b.data(), b.size()); CHECK(p.valid() && p.payloadSize() == 3 && p.payload()[2] == 3);
    CHECK(!setRtpTransportSequence(b, 20, 1));
    b = rtp(3); b[0] = 0xa1; b.insert(b.begin() + 12, {0, 0, 0, 99}); b.insert(b.end(), {0, 0, 0, 4});
    CHECK(setRtpTransportSequence(b, 20, 300)); CHECK(setRtpTransportSequence(b, 1, 55));
    CHECK(rtpTransportSequence(b, 20) == 300 && rtpTransportSequence(b, 1) == 55);
    RtpPacketParser two(b.data(), b.size()); CHECK(two.valid() && two.csrcCount() == 1 && two.payloadSize() == 3 && two.padding());
    auto broken = b; broken[18] = 0xff; broken[19] = 0xff; CHECK(!rtpTransportSequence(broken, 20)); CHECK(!setRtpTransportSequence(broken, 20, 1));
}
RtcpSession::Config config()
{
    RtcpSession::Config c; c.senderSsrc = 100; c.cname = "test"; c.reportInterval = 100ms; c.twccInterval = 20ms;
    c.nackDelay = 10ms; c.nackRetry = 30ms; c.pliInterval = 50ms; c.maxCachedPackets = 4;
    RtcpMediaParameters m; m.payloadType = 96; m.clockRate = 90000; m.nack = m.pli = m.fir = m.transportCc = true;
    m.sendTwccExtension = m.receiveTwccExtension = 5; c.parameters.media = {m}; return c;
}
void sessions()
{
    const auto start = RtcpSession::Time(10s); auto c = config();
    std::vector<TwccFeedbackEvent> twcc; std::vector<RtcpReportEvent> rr; std::vector<RtcpKeyFrameEvent> keyframes; std::vector<RtcpNackEvent> nacks;
    RtcpSession sender(c, {[&](auto e) { rr.push_back(e); }, [&](auto e) { nacks.push_back(e); }, [&](auto e) { keyframes.push_back(e); }, [&](auto e) { twcc.push_back(e); }, {}});
    RtcpSession receiver(c);
    std::vector<RtcpBytes> packets;
    for (unsigned i = 0; i < 3; ++i)
    {
        auto p = rtp(static_cast<std::uint16_t>(65534 + i), 1000 + i * 900);
        CHECK(sender.prepareRtp(p)); sender.sentRtp(p, {9, static_cast<std::uint8_t>(i), 8}, start + i * 10ms); packets.push_back(p);
    }
    receiver.receivedRtp(packets[0], start); receiver.receivedRtp(packets[2], start + 20ms);
    receiver.poll(start + 35ms, 0x1234000000000000); auto feedback = receiver.takeOutput();
    bool gotNack = false, gotTwcc = false;
    for (auto& o : feedback)
    {
        CHECK(!o.retransmission); CHECK(o.bytes[1] == 201); // compound RR+SDES prefix when rsize isn't negotiated
        for (const auto& message : parse(o.bytes))
        {
            if (const auto* n = std::get_if<RtcpNack>(&message)) { CHECK(n->sequences == std::vector<std::uint16_t>({65535})); gotNack = true; }
            if (const auto* f = std::get_if<RtcpTwcc>(&message)) { CHECK(f->deltas.size() == 3 && !f->deltas[1]); gotTwcc = true; }
        }
        CHECK(sender.receivedRtcp(o.bytes, start + 40ms));
    }
    CHECK(gotNack && gotTwcc && twcc.size() == 1 && twcc[0].received == 2 && twcc[0].notReceived == 1);
    auto retransmission = sender.takeOutput(); CHECK(retransmission.size() == 1 && retransmission[0].retransmission && retransmission[0].bytes == RtcpBytes({9, 1, 8}));
    receiver.receivedRtp(packets[1], start + 45ms); // late delivery cancels NACK, and TWCC corrects previous missing status
    receiver.poll(start + 80ms, 0x1234000000000000);
    for (auto& o : receiver.takeOutput()) CHECK(sender.receivedRtcp(o.bytes, start + 81ms));
    CHECK(twcc.size() == 2 && twcc.back().received == 1 && twcc.back().packets[0].rtpSequence == 65535);
    CHECK(receiver.requestPli(42, start + 90ms)); CHECK(!receiver.requestPli(42, start + 91ms));
    for (auto& o : receiver.takeOutput()) { CHECK(sender.receivedRtcp(o.bytes, start + 91ms)); CHECK(sender.receivedRtcp(o.bytes, start + 92ms)); }
    CHECK(keyframes.size() == 1);
    sender.poll(start + 100ms, 0x1234567890000000);
    for (auto& o : sender.takeOutput())
    {
        CHECK(!o.retransmission); const auto sr = std::get<RtcpReport>(parse(o.bytes)[0]);
        CHECK(sr.senderReport && sr.packetCount == 3 && sr.octetCount == 9 && sr.rtpTimestamp == 10000);
        CHECK(receiver.receivedRtcp(o.bytes, start + 105ms));
    }
    receiver.poll(start + 110ms, 0x1234567900000000);
    for (auto& o : receiver.takeOutput())
    {
        const auto report = std::get<RtcpReport>(parse(o.bytes)[0]);
        CHECK(!report.senderReport && report.reports.size() == 1 && report.reports[0].highestSequence == 65536 && report.reports[0].cumulativeLost == 0 && report.reports[0].jitter == 196);
        CHECK(sender.receivedRtcp(o.bytes, start + 120ms));
    }
    CHECK(rr.size() == 1 && rr[0].rttMs && *rr[0].rttMs >= 14.9 && *rr[0].rttMs <= 15.1);
    const auto fir = buildRtcpKeyFrameRequest({9, 42, RtcpKeyFrameRequest::Kind::Fir, 3});
    CHECK(sender.receivedRtcp(fir, start + 200ms)); CHECK(sender.receivedRtcp(fir, start + 300ms));
    CHECK(keyframes.size() == 2 && keyframes.back().feedback.kind == RtcpKeyFrameRequest::Kind::Fir);
    CHECK(sender.receivedRtcp(buildRtcpKeyFrameRequest({10, 42, RtcpKeyFrameRequest::Kind::Fir, 3}), start + 400ms));
    CHECK(keyframes.size() == 3); // independent requester uses its own FIR command sequence
    const auto malformed = buildRtcpKeyFrameRequest({9, 42, RtcpKeyFrameRequest::Kind::Pli, 0});
    auto bad = malformed; bad.insert(bad.end(), {0x80, 201, 0, 2}); CHECK(!sender.receivedRtcp(bad, start + 1s)); CHECK(keyframes.size() == 3);
    CHECK(sender.receivedRtcp(buildRtcpNack({9, 42, {99}}), start + 1s)); CHECK(nacks.back().cacheMisses == std::vector<std::uint16_t>({99}));
    const auto beforeInvalidation = sender.snapshot(); sender.invalidate();
    CHECK(sender.snapshot().receivedNacks == beforeInvalidation.receivedNacks && sender.snapshot().cachedPackets == 0 && sender.snapshot().sendStreams == 0);
    sender.reset(7); receiver.reset(7); CHECK(sender.snapshot().generation == 7 && sender.snapshot().cachedPackets == 0 && sender.snapshot().twccHistory == 0);
    CHECK(!receiver.requestPli(42, start + 2s)); CHECK(sender.receivedRtcp(malformed, start + 2s)); CHECK(keyframes.size() == 3);
}
void rolloverAndLimits()
{
    auto c = config(); c.parameters.reducedSize = true; const auto t = RtcpSession::Time(30s);
    std::vector<TwccFeedbackEvent> events; RtcpSession sender(c, {{}, {}, {}, [&](auto e) { events.push_back(e); }, {}});
    for (unsigned i = 0; i < 65538; ++i)
    {
        auto b = rtp(i); CHECK(sender.prepareRtp(b)); sender.sentRtp(b, {1, 2, 3}, t + i * 1us);
    }
    CHECK(sender.snapshot().cachedPackets == 4 && sender.snapshot().cachedBytes == 12 && sender.snapshot().twccHistory == c.maxTwccHistory);
    RtcpTwcc f; f.baseSequence = 65535; f.referenceTime = 0xffffff; f.deltas = {1, -1, 256};
    auto b = buildRtcpTwcc(f); CHECK(sender.receivedRtcp(b, t + 1s)); CHECK(events.size() == 1);
    CHECK(events[0].packets[0].transportSequence == 65535 && events[0].packets[2].transportSequence == 65537);
    CHECK(sender.receivedRtcp(b, t + 1s)); CHECK(events.size() == 1); // duplicate feedback
    f.referenceTime = 0; f.baseSequence = 0; f.deltas = {1}; // already acknowledged; no callback
    CHECK(sender.receivedRtcp(buildRtcpTwcc(f), t + 1s)); CHECK(events.size() == 1);
    auto next = rtp(2); CHECK(sender.prepareRtp(next)); sender.sentRtp(next, {1}, t + 1s);
    f.baseSequence = 2; CHECK(sender.receivedRtcp(buildRtcpTwcc(f), t + 1s)); CHECK(events.size() == 2);
    CHECK(events.back().packets[0].remoteArrivalUs.value() - events.front().packets[0].remoteArrivalUs.value() == 64000);
    auto p = c.parameters; auto other = p.media[0]; other.payloadType = 97; other.sendTwccExtension = 7; p.media.push_back(other);
    CHECK(!sender.configure(p)); CHECK(sender.snapshot().twccHistory > 0); // invalid update is transactional
    sender.poll(t + 10s, 1); CHECK(sender.snapshot().cachedPackets == 0 && sender.snapshot().twccHistory == 0);
    RtcpSession recv(c); recv.receivedRtp(rtp(1), t); recv.receivedRtp(rtp(3, 2800), t + 20ms); recv.poll(t + 100ms, 1);
    bool report = false;
    for (auto& output : recv.takeOutput()) for (auto& message : parse(output.bytes)) if (const auto* r = std::get_if<RtcpReport>(&message))
    { CHECK(r->reports[0].cumulativeLost == 1 && r->reports[0].fractionLost == 85); report = true; }
    CHECK(report);
}
void submissionBeforeCompletion()
{
    auto c = config(); const auto t = RtcpSession::Time(30s);
    std::vector<TwccFeedbackEvent> events; std::vector<TransportPacketsFeedback> raw;
    RtcpSession sender(c, {{}, {}, {}, [&](auto e) { events.push_back(std::move(e)); }, [&](auto e) { raw.push_back(std::move(e)); }});
    auto p = rtp(1); CHECK(sender.prepareRtp(p));
    CHECK(sender.sendingRtp(p, 1000, t, t + 10ms, 28) == 0);
    CHECK(sender.snapshot().transport.data_in_flight == 1028 && sender.snapshot().sendStreams == 0);
    RtcpTwcc f; f.referenceTime = 3; f.deltas = {1};
    CHECK(sender.receivedRtcp(buildRtcpTwcc(f), t + 20ms));
    CHECK(raw.size() == 1 && events.size() == 1 && raw[0].packet_feedbacks[0].sent_packet.send_time == t + 10ms);
    CHECK(raw[0].packet_feedbacks[0].sent_packet.size == 1028 && raw[0].data_in_flight == 0);
    sender.sentRtp(p, {1, 2, 3}, t + 10ms, false); // delayed success only updates report/cache
    CHECK(sender.snapshot().twccHistory == 0 && sender.snapshot().sendStreams == 1);
    CHECK(sender.receivedRtcp(buildRtcpTwcc(f), t + 21ms) && raw.size() == 1);
    auto next = rtp(2); CHECK(sender.prepareRtp(next)); CHECK(sender.sendingRtp(next, 1000, t + 30ms, t + 30ms) == 1);
    f.baseSequence = 1; f.deltas = {41}; CHECK(sender.receivedRtcp(buildRtcpTwcc(f), t + 40ms));
    CHECK(events.size() == 2 && events.back().packetDeltas.size() == 1);
    CHECK(events.back().packetDeltas[0].send_delta == 20ms && events.back().packetDeltas[0].receive_delta == 10ms);
    CHECK(events.back().packetDeltas[0].delay_delta == -10ms);
    auto failed = rtp(3); CHECK(sender.prepareRtp(failed)); CHECK(sender.sendingRtp(failed, 1000, t + 50ms, t + 50ms) == 2);
    sender.failedRtp(2); CHECK(sender.snapshot().transport.data_in_flight == 0 && sender.snapshot().transport.failed_sends == 1);
    sender.reset(7); CHECK(sender.snapshot().transport.generation == 7 && sender.snapshot().transport.sent_packets == 0);
}
void schedulingAndNegotiation()
{
    auto c = config(); c.parameters.media[0].transportCc = false; const auto t = RtcpSession::Time(50s);
    RtcpSession receiver(c); receiver.receivedRtp(rtp(1), t); receiver.receivedRtp(rtp(3), t + 1ms);
    receiver.poll(t + 5ms, 1); CHECK(receiver.takeOutput().empty());
    receiver.poll(t + 12ms, 1); CHECK(receiver.takeOutput().size() == 1);
    receiver.poll(t + 20ms, 1); CHECK(receiver.takeOutput().empty());
    receiver.receivedRtp(rtp(2), t + 25ms); receiver.poll(t + 45ms, 1); CHECK(receiver.takeOutput().empty());
    receiver.receivedRtp(rtp(5), t + 46ms); receiver.poll(t + 60ms, 1); CHECK(receiver.takeOutput().size() == 1);
    receiver.poll(t + 3s, 1); CHECK(receiver.snapshot().sentNacks == 2); // expired gap does not keep retrying
    c.parameters.media[0].nack = c.parameters.media[0].pli = c.parameters.media[0].fir = false;
    unsigned events = 0; RtcpSession disabled(c, {{}, [&](auto) { ++events; }, [&](auto) { ++events; }, [&](auto) { ++events; }, {}});
    auto p = rtp(1); CHECK(disabled.prepareRtp(p)); CHECK(!rtpTransportSequence(p, 5)); disabled.sentRtp(p, {1}, t);
    CHECK(disabled.receivedRtcp(buildRtcpNack({10, 42, {1}}), t + 20ms));
    CHECK(disabled.receivedRtcp(buildRtcpKeyFrameRequest({10, 42, RtcpKeyFrameRequest::Kind::Pli, 0}), t + 20ms));
    CHECK(disabled.receivedRtcp(buildRtcpKeyFrameRequest({10, 42, RtcpKeyFrameRequest::Kind::Fir, 0}), t + 20ms));
    CHECK(disabled.takeOutput().empty() && events == 0 && disabled.snapshot().cachedPackets == 0);
    // Reception reports split at the MTU, with all SSRCs covered and SDES in every compound packet.
    c.maxDatagramSize = 512; RtcpSession many(c);
    for (unsigned i = 0; i < 40; ++i) many.receivedRtp(rtp(1, 1000, i + 1), t);
    many.poll(t + 100ms, 1); unsigned blocks = 0;
    for (auto& out : many.takeOutput())
    {
        CHECK(out.bytes.size() <= 512 && out.bytes[1] == 201);
        const auto messages = parse(out.bytes); const auto& r = std::get<RtcpReport>(messages[0]);
        CHECK(!r.reports.empty() && r.reports.size() <= 31); blocks += r.reports.size();
    }
    CHECK(blocks == 40 && many.snapshot().sentReceiverReports == 2);
}
}
int main()
{
    try { codecs(); extensions(); sessions(); rolloverAndLimits(); submissionBeforeCompletion(); schedulingAndNegotiation(); std::cout << "PASS: RTCP reports, feedback codecs, recovery, TWCC and lifecycle\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
