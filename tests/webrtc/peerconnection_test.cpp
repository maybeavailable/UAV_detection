#include"webrtc/peerconnection.h"

#include<boost/asio/executor_work_guard.hpp>
#include<atomic>
#include<chrono>
#include<iostream>
#include<stdexcept>
#include<thread>

using namespace webrtc;
using namespace std::chrono_literals;
namespace net = boost::asio;
using ErrorCode = PeerConnection::ErrorCode;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

namespace
{
template<class Predicate> void until(net::io_context& io, Predicate predicate, std::chrono::milliseconds timeout = 4000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
template<class Action> ErrorCode perform(net::io_context& io, Action action)
{
    bool done = false; ErrorCode result;
    action([&](auto ec) { result = ec; done = true; }); until(io, [&] { return done; }); return result;
}
PeerSessionDescription offer(net::io_context& io, PeerConnection& peer, bool restart = false)
{
    bool done = false; std::optional<PeerSessionDescription> description;
    peer.createOffer({restart}, [&](auto ec, auto result) { CHECK(!ec && result); description = std::move(result); done = true; });
    until(io, [&] { return done; }); return *description;
}
PeerSessionDescription answer(net::io_context& io, PeerConnection& peer)
{
    bool done = false; std::optional<PeerSessionDescription> description;
    peer.createAnswer([&](auto ec, auto result) { CHECK(!ec && result); description = std::move(result); done = true; });
    until(io, [&] { return done; }); return *description;
}
PeerConnection::Config config(std::vector<PeerMedia> media, bool ipv6 = false)
{
    PeerConnection::Config c; c.media = std::move(media);
    c.ice.ice.bindings = {{UdpSocket::Endpoint(net::ip::make_address(ipv6 ? "::1" : "127.0.0.1"), 0), 1, 65535}};
    c.ice.ice.checkInterval = 5ms; c.ice.ice.initialRto = 500ms; c.ice.ice.checkTimeout = 2500ms;
    c.dtls.handshakeTimeout = 2500ms; return c;
}
std::vector<PeerMedia> avData(unsigned videoPt = 96, unsigned rtxPt = 97)
{
    auto video = PeerMedia::video("video", {{videoPt, "VP8", 90000, 1, {}}, {rtxPt, "rtx", 90000, 1, "apt=" + std::to_string(videoPt)}});
    video.feedback = {{std::nullopt, "nack"}, {videoPt, "nack pli"}};
    video.extmaps = {{1, {}, "urn:ietf:params:rtp-hdrext:sdes:mid", {}}};
    return {PeerMedia::audio("audio", {{111, "opus", 48000, 2, "minptime=10;useinbandfec=1"}}), video, PeerMedia::data()};
}
struct Pair
{
    net::io_context io;
    std::unique_ptr<PeerConnection> a, b;
    std::vector<PeerIceCandidate> candidatesA, candidatesB;
    std::vector<PeerConnection::Bytes> receivedA, receivedB, secureB;
    std::vector<DtlsSrtpKeyMaterial> keysA, keysB;
    std::vector<CongestionControlEvent> targetsA;
    std::vector<TransportPacketsFeedback> feedbackA;
    std::vector<PeerConnection::Bytes> rtpB;
    unsigned errors{0}, connectedA{0}, connectedB{0};
    bool trickle{true};
    explicit Pair(PeerConnection::Config ca = config({PeerMedia::data()}), PeerConnection::Config cb = config({PeerMedia::data()}))
    {
        PeerConnection::Callbacks ha, hb;
        ha.onIceCandidate = [&](auto candidate) {
            candidatesA.push_back(candidate); if (trickle) b->addIceCandidate(std::move(candidate), [](auto ec) { if (ec) std::cerr << "candidate A->B: " << ec.message() << "\n"; CHECK(!ec); });
        };
        hb.onIceCandidate = [&](auto candidate) {
            candidatesB.push_back(candidate); if (trickle) a->addIceCandidate(std::move(candidate), [](auto ec) { if (ec) std::cerr << "candidate B->A: " << ec.message() << "\n"; CHECK(!ec); });
        };
        ha.onConnectionState = [&](auto state) { if (state == PeerConnectionState::Connected) ++connectedA; };
        hb.onConnectionState = [&](auto state) { if (state == PeerConnectionState::Connected) ++connectedB; };
        ha.onDtlsData = [&](auto data) { receivedA.push_back(std::move(data)); };
        hb.onDtlsData = [&](auto data) { receivedB.push_back(std::move(data)); };
        ha.onSrtpKeys = [&](auto keys) { keysA.push_back(std::move(keys)); };
        hb.onSrtpKeys = [&](auto keys) { keysB.push_back(std::move(keys)); };
        hb.onSecurePacket = [&](auto packet) { secureB.push_back(std::move(packet.datagram.payload)); };
        ha.onTargetTransferRate = [&](auto event) { targetsA.push_back(std::move(event)); };
        ha.onTransportFeedback = [&](auto event) { feedbackA.push_back(std::move(event)); };
        hb.onRtpPacket = [&](auto packet) { rtpB.push_back(std::move(packet.datagram.payload)); };
        ha.onError = hb.onError = [&](auto ec, auto message) { ++errors; std::cerr << "Peer error " << ec.message() << ": " << message << '\n'; };
        a = std::make_unique<PeerConnection>(io, std::move(ca), std::move(ha));
        b = std::make_unique<PeerConnection>(io, std::move(cb), std::move(hb));
    }
    void negotiate(bool restart = false, bool reverse = false)
    {
        auto& first = reverse ? *b : *a; auto& second = reverse ? *a : *b;
        auto o = offer(io, first, restart);
        CHECK(first.snapshot().signalingState == PeerSignalingState::Stable);
        CHECK(!perform(io, [&](auto done) { first.setLocalDescription(o, done); }));
        CHECK(first.snapshot().signalingState == PeerSignalingState::HaveLocalOffer);
        if (!trickle) o = *first.snapshot().pendingLocalDescription;
        CHECK(!perform(io, [&](auto done) { second.setRemoteDescription(SdpDescriptionType::Offer, o.toSdp(), done); }));
        CHECK(second.snapshot().signalingState == PeerSignalingState::HaveRemoteOffer);
        auto ans = answer(io, second);
        CHECK(!perform(io, [&](auto done) { second.setLocalDescription(ans, done); }));
        CHECK(!perform(io, [&](auto done) { first.setRemoteDescription(SdpDescriptionType::Answer, ans.toSdp(), done); }));
        CHECK(a->snapshot().signalingState == PeerSignalingState::Stable && b->snapshot().signalingState == PeerSignalingState::Stable);
    }
    void connected()
    {
        until(io, [&] { return a->snapshot().connectionState == PeerConnectionState::Connected && b->snapshot().connectionState == PeerConnectionState::Connected; });
        CHECK(errors == 0);
        CHECK(a->snapshot().dtls->peerFingerprint && b->snapshot().dtls->peerFingerprint);
        CHECK(a->snapshot().ice->ice.role != b->snapshot().ice->ice.role);
    }
    void data()
    {
        const auto beforeA = receivedA.size(), beforeB = receivedB.size(); unsigned completed = 0;
        a->asyncSendDtls({'S', 'C', 'T', 'P'}, [&](auto ec, auto size) { CHECK(!ec && size == 4); ++completed; });
        b->asyncSendDtls({0, 1, 255}, [&](auto ec, auto size) { CHECK(!ec && size == 3); ++completed; });
        until(io, [&] { return completed == 2 && receivedA.size() == beforeA + 1 && receivedB.size() == beforeB + 1; });
        CHECK(receivedB.back() == PeerConnection::Bytes({'S', 'C', 'T', 'P'}));
        CHECK(a->snapshot().dtls->stats.sentBytes >= 4 && b->snapshot().dtls->stats.receivedBytes >= 4);
    }
    void close()
    {
        unsigned count = 0;
        a->close([&](auto ec) { CHECK(!ec); ++count; }); b->close([&](auto ec) { CHECK(!ec); ++count; });
        until(io, [&] { return count == 2; });
        CHECK(a->snapshot().ice->state == IceTransportState::Closed && a->snapshot().dtls->state == DtlsState::Closed);
        CHECK(!perform(io, [&](auto done) { a->close(done); }));
        CHECK(perform(io, [&](auto done) { a->addMedia(PeerMedia::data("extra"), done); }) == net::error::operation_aborted);
    }
};
void transportAndRestart(bool ipv6)
{
    auto answerConfig = config({PeerMedia::data("other")}, ipv6);
    answerConfig.ice.preferDtlsClient = !ipv6;
    Pair p(config({PeerMedia::data()}, ipv6), answerConfig);
    unsigned rejected = 0;
    p.a->asyncSendDtls({1}, [&](auto ec, auto n) { CHECK(ec == net::error::not_connected && n == 0); ++rejected; });
    p.negotiate(); p.connected(); p.data(); CHECK(rejected == 1);
    CHECK(!p.candidatesA.empty() && p.candidatesA.back().candidate.empty());
    CHECK(p.a->snapshot().canTrickleIceCandidates.value_or(false));
    const auto oldA = p.a->snapshot(), oldB = p.b->snapshot();
    CHECK(oldA.currentLocalDescription->session.media[0].candidates.size() == 1);
    CHECK(oldA.currentRemoteDescription->session.media[0].endOfCandidates);
    p.negotiate(); p.connected(); p.data();
    CHECK(p.a->snapshot().ice->generation == 0 && p.b->snapshot().ice->generation == 0);
    CHECK(p.connectedA == 1 && p.connectedB == 1); // 复用现有 association
    p.negotiate(true); p.connected(); p.data();
    CHECK(p.a->snapshot().ice->generation == 1 && p.b->snapshot().ice->generation == 1);
    CHECK(p.a->snapshot().localCredentials.ufrag != oldA.localCredentials.ufrag);
    CHECK(p.b->snapshot().localCredentials.ufrag != oldB.localCredentials.ufrag);
    CHECK(perform(p.io, [&](auto done) { p.a->addIceCandidate(p.candidatesB.front(), done); }) == net::error::operation_aborted);
    p.negotiate(true, true); p.connected(); p.data();
    CHECK(p.a->snapshot().ice->generation == 2 && p.b->snapshot().ice->generation == 2);
    p.close();
}
void mediaNegotiation()
{
    auto first = avData(), second = avData(120, 121); second[1].direction = SdpDirection::RecvOnly;
    second[0].mid = "video"; second[1].mid = "audio"; // 本地能力标识与远端 MID 碰撞时仍按媒体类型绑定
    Pair p(config(first), config(second)); p.negotiate(); p.connected();
    const auto a = p.a->snapshot(), b = p.b->snapshot();
    CHECK(a.negotiatedMedia.size() == 3 && !a.negotiatedMedia[0].rejected);
    CHECK(a.negotiatedMedia[1].codecs[0].payloadType == 96 && a.negotiatedMedia[1].codecs[1].parameter("apt") == "96");
    CHECK(a.negotiatedMedia[1].localDirection == SdpDirection::SendOnly && b.negotiatedMedia[1].localDirection == SdpDirection::RecvOnly);
    CHECK(p.keysA.size() == 1 && p.keysB.size() == 1);
    CHECK(p.keysA[0].localMasterKey == p.keysB[0].remoteMasterKey);
    CHECK(a.currentLocalDescription->session.media[1].bundleOnly && !a.currentRemoteDescription->session.media[1].bundleOnly);
    CHECK(a.currentRemoteDescription->session.media[1].common.extmaps.size() == 1);
    const PeerConnection::Bytes encrypted{0x80, 96, 0, 1, 2, 3, 4, 5, 0, 0, 0, 1, 99, 98, 97, 96};
    bool sent = false;
    p.a->asyncSendSecurePacket(encrypted, [&](auto ec, auto n) { CHECK(!ec && n == encrypted.size()); sent = true; });
    until(p.io, [&] { return sent && p.secureB.size() == 1; }); CHECK(p.secureB[0] == encrypted);
    p.data();
    CHECK(!perform(p.io, [&](auto done) { p.a->setDirection("video", SdpDirection::Inactive, done); }));
    p.negotiate(); p.connected();
    CHECK(p.a->snapshot().negotiatedMedia[1].localDirection == SdpDirection::Inactive);
    CHECK(p.keysA.size() == 1 && p.keysB.size() == 1);
    p.negotiate(false, true); p.connected(); // answerer 也可在后续协商中成为 offerer
    CHECK(!perform(p.io, [&](auto done) { p.a->addMedia(PeerMedia::video("screen", {{98, "H264", 90000, 1, "packetization-mode=1;profile-level-id=42e01f"}}), done); }));
    p.negotiate(); p.connected();
    CHECK(p.a->snapshot().negotiatedMedia.size() == 4 && p.a->snapshot().negotiatedMedia[3].rejected);
    p.close();
}
void statesAndValidation()
{
    net::io_context io; PeerConnection a(io, config({PeerMedia::data()})), b(io, config({PeerMedia::data()}));
    bool badAnswer = false; a.createAnswer([&](auto ec, auto d) { CHECK(ec == net::error::already_started && !d); badAnswer = true; }); until(io, [&] { return badAnswer; });
    auto o = offer(io, a); auto forged = o; forged.session.common.fingerprints[0].value[0] = forged.session.common.fingerprints[0].value[0] == '0' ? '1' : '0';
    CHECK(perform(io, [&](auto done) { a.setLocalDescription(forged, done); }) == net::error::invalid_argument);
    CHECK(a.snapshot().signalingState == PeerSignalingState::Stable && !a.snapshot().ice);
    CHECK(!perform(io, [&](auto done) { a.setLocalDescription(o, done); }));
    CHECK(perform(io, [&](auto done) { a.setLocalDescription(o, done); }) == net::error::already_started);
    CHECK(perform(io, [&](auto done) { a.setRemoteDescription(o, done); }) == net::error::already_started);
    CHECK(!perform(io, [&](auto done) { a.rollback(done); }));
    CHECK(a.snapshot().signalingState == PeerSignalingState::Stable);
    CHECK(!perform(io, [&](auto done) { a.setLocalDescription(o, done); }));
    CHECK(!perform(io, [&](auto done) { b.setRemoteDescription(o, done); }));
    auto ans = answer(io, b), invalid = ans; invalid.session.media[0].mid = "wrong"; invalid.session.groups[0].mids[0] = "wrong";
    CHECK(perform(io, [&](auto done) { a.setRemoteDescription(invalid, done); }) == net::error::invalid_argument);
    CHECK(a.snapshot().signalingState == PeerSignalingState::HaveLocalOffer && !a.snapshot().currentRemoteDescription);
    PeerIceCandidate c{"candidate:abc 1 UDP 2130706431 127.0.0.1 12345 typ host", "unknown", 0, o.session.common.iceUfrag.value(), {}};
    CHECK(perform(io, [&](auto done) { b.addIceCandidate(c, done); }) == net::error::invalid_argument);
    c.sdpMid = "data"; c.sdpMLineIndex = 1;
    CHECK(perform(io, [&](auto done) { b.addIceCandidate(c, done); }) == net::error::invalid_argument);
    c.sdpMLineIndex = 0;
    CHECK(!perform(io, [&](auto done) { b.addIceCandidate(c, done); }));
    CHECK(b.snapshot().pendingRemoteDescription->session.media[0].candidates.size() == 1);
    c.usernameFragment = "stale";
    CHECK(perform(io, [&](auto done) { b.addIceCandidate(c, done); }) == net::error::operation_aborted);
    CHECK(perform(io, [&](auto done) { b.setRemoteDescription(SdpDescriptionType::Offer, "bad", done); }) == net::error::invalid_argument);
    CHECK(!perform(io, [&](auto done) { a.rollback(done); })); CHECK(!perform(io, [&](auto done) { b.rollback(done); }));
    unsigned completed = 0; bool cancelled = false;
    a.createOffer([&](auto ec, auto d) { CHECK(ec == net::error::operation_aborted && !d); cancelled = true; ++completed; });
    a.close([&](auto ec) { CHECK(!ec && cancelled); ++completed; });
    b.close([&](auto ec) { CHECK(!ec); ++completed; }); until(io, [&] { return completed == 3; });
}
void incompatibleCodecsAndNoCapabilities()
{
    auto h264a = PeerMedia::video("video", {{96, "H264", 90000, 1, "packetization-mode=1;profile-level-id=42e01f"}});
    auto h264b = PeerMedia::video("video", {{96, "H264", 90000, 1, "packetization-mode=0;profile-level-id=42e01f"}});
    Pair p(config({h264a, PeerMedia::data()}), config({h264b, PeerMedia::data()}));
    p.negotiate(); p.connected(); CHECK(p.a->snapshot().negotiatedMedia[0].rejected); p.data(); p.close();
    net::io_context io; PeerConnection a(io, config({PeerMedia::data()})), b(io, config({}));
    auto o = offer(io, a); CHECK(!perform(io, [&](auto done) { a.setLocalDescription(o, done); }));
    CHECK(!perform(io, [&](auto done) { b.setRemoteDescription(o, done); })); auto ans = answer(io, b);
    CHECK(ans.session.media[0].isRejected());
    CHECK(!perform(io, [&](auto done) { b.setLocalDescription(ans, done); }));
    CHECK(!perform(io, [&](auto done) { a.setRemoteDescription(ans, done); }));
    CHECK(a.snapshot().connectionState == PeerConnectionState::New && a.snapshot().signalingState == PeerSignalingState::Stable);
    CHECK(!perform(io, [&](auto done) { a.close(done); })); CHECK(!perform(io, [&](auto done) { b.close(done); }));
}
void embeddedCandidatesAndAddingRtp()
{
    Pair p; p.trickle = false; p.negotiate(); p.connected(); p.data();
    CHECK(p.keysA.empty() && p.keysB.empty());
    const auto video = PeerMedia::video("video", {{96, "VP8", 90000, 1, {}}});
    CHECK(!perform(p.io, [&](auto done) { p.a->addMedia(video, done); }));
    CHECK(!perform(p.io, [&](auto done) { p.b->addMedia(video, done); }));
    p.negotiate(); p.connected();
    CHECK(p.keysA.size() == 1 && p.keysB.size() == 1);
    CHECK(p.a->snapshot().ice->generation == 0 && p.connectedA == 1);
    CHECK(p.a->snapshot().negotiatedMedia.size() == 2 && !p.a->snapshot().negotiatedMedia[1].rejected);
    p.close();
}
void externallyResolvedCandidate()
{
    auto cb = config({PeerMedia::data()}); cb.ice.ice.maxCandidates = 1;
    Pair p(config({PeerMedia::data()}), cb); p.trickle = false;
    auto o = offer(p.io, *p.a);
    CHECK(!perform(p.io, [&](auto done) { p.a->setLocalDescription(o, done); }));
    o = *p.a->snapshot().pendingLocalDescription;
    auto c = o.session.media[0].candidates[0]; const auto endpoint = c.udpEndpoint(); CHECK(endpoint);
    c.address = "first.invalid"; auto extra = c; extra.address = "second.invalid"; extra.foundation = "alt";
    o.session.media[0].candidates = {c, extra};
    CHECK(!perform(p.io, [&](auto done) { p.b->setRemoteDescription(o, done); }));
    const PeerIceCandidate resolved{c.toCandidateString(), "data", 0, o.session.common.iceUfrag.value(), endpoint};
    PeerIceCandidate retry{extra.toCandidateString(), "data", 0, o.session.common.iceUfrag.value(),
                           UdpSocket::Endpoint(net::ip::make_address("127.0.0.2"), endpoint->port())};
    CHECK(!perform(p.io, [&](auto done) { p.b->addIceCandidate(resolved, done); }));
    CHECK(!perform(p.io, [&](auto done) { p.b->addIceCandidate(retry, done); }));
    auto a = answer(p.io, *p.b);
    CHECK(!perform(p.io, [&](auto done) { p.b->setLocalDescription(a, done); }));
    until(p.io, [&] { return p.errors == 1; }); // 第二个解析端点触发底层候选容量上限
    CHECK(!p.b->snapshot().ice->ice.remoteEndOfCandidates);
    retry.resolvedEndpoint = endpoint; // 重试已宣告候选，解析到相同已有端点，不占额外容量
    CHECK(!perform(p.io, [&](auto done) { p.b->addIceCandidate(retry, done); }));
    until(p.io, [&] { return p.b->snapshot().ice->ice.remoteEndOfCandidates; });
    p.errors = 0;
    CHECK(!perform(p.io, [&](auto done) { p.a->setRemoteDescription(a, done); }));
    p.connected(); p.data();
    CHECK(p.b->snapshot().ice->ice.remoteCandidates[0].address == "first.invalid");
    p.close();
}
void fingerprintFailure()
{
    Pair p; auto o = offer(p.io, *p.a);
    CHECK(!perform(p.io, [&](auto done) { p.a->setLocalDescription(o, done); }));
    CHECK(!perform(p.io, [&](auto done) { p.b->setRemoteDescription(o, done); }));
    auto a = answer(p.io, *p.b);
    CHECK(!perform(p.io, [&](auto done) { p.b->setLocalDescription(a, done); }));
    auto& hex = a.session.common.fingerprints[0].value[0]; hex = hex == '0' ? '1' : '0';
    CHECK(!perform(p.io, [&](auto done) { p.a->setRemoteDescription(a, done); }));
    // Failed state and the diagnostic callback are posted separately across transport strands.
    until(p.io, [&] { return p.a->snapshot().connectionState == PeerConnectionState::Failed && p.errors > 0; });
    CHECK(p.errors > 0 && !p.a->snapshot().dtls->peerFingerprint && p.receivedA.empty() && p.keysA.empty());
    p.close();
}
void boundedCandidatesAndThreads()
{
    net::io_context io; auto c = config({PeerMedia::data()}); c.maxPendingCandidates = 1;
    PeerConnection p(io, c);
    const PeerIceCandidate candidate{"candidate:abc 1 UDP 2130706431 127.0.0.1 12345 typ host", "data", 0, "next", {}};
    CHECK(!perform(io, [&](auto done) { p.addIceCandidate(candidate, done); }));
    CHECK(perform(io, [&](auto done) { p.addIceCandidate(candidate, done); }) == net::error::no_buffer_space);
    auto work = net::make_work_guard(io); io.restart(); std::thread worker1([&] { io.run(); }), worker2([&] { io.run(); });
    std::atomic<unsigned> calls{0}; std::vector<std::thread> senders;
    for (unsigned i = 0; i < 4; ++i) senders.emplace_back([&] {
        for (unsigned n = 0; n < 8; ++n) p.asyncSendDtls({1}, [&](auto ec, auto size) { CHECK(ec && size == 0); ++calls; });
    });
    for (auto& sender : senders) sender.join();
    std::atomic<bool> closed{false}; p.close([&](auto ec) { CHECK(!ec && calls == 32); closed = true; });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!closed && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    work.reset(); worker1.join(); worker2.join(); CHECK(closed && calls == 32);
}
void gccOverRealUdp()
{
    auto video = PeerMedia::video("video", {{96, "VP8", 90000, 1, {}}});
    video.feedback = {{std::nullopt, "transport-cc"}};
    video.extmaps = {{5, {}, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", {}}};
    auto cfg = config({video}); cfg.dtls.enableSrtp = true; cfg.dtls.rtcp.twccInterval = 10ms;
    Pair p(cfg, cfg); p.negotiate(); p.connected();
    until(p.io, [&] { return !p.targetsA.empty() && p.a->snapshot().dtls->congestionControl->active; });
    CHECK(p.targetsA.back().generation == 0);
    PeerConnection::Bytes rtp{0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 42}; rtp.resize(400, 7);
    bool sent = false; p.a->asyncSendRtp(rtp, [&](auto ec, auto n) { CHECK(!ec && n == rtp.size()); sent = true; });
    until(p.io, [&] { return sent && !p.feedbackA.empty() && p.rtpB.size() > 1; });
    CHECK(p.a->snapshot().dtls->congestionControl->feedback_packets > 0);
    TargetRateConstraints limits; limits.min_data_rate = 30000; limits.max_data_rate = 120000;
    CHECK(!perform(p.io, [&](auto done) { p.a->setBitrateConstraints(limits, done); }));
    until(p.io, [&] { return p.targetsA.back().rate.target_rate <= 120000; });
    StreamsConfig streams; streams.max_padding_rate = 10000;
    CHECK(!perform(p.io, [&](auto done) { p.a->setStreamsConfig(streams, done); }));
    CHECK(p.a->snapshot().dtls->pacer->paddingRateBps > 0);
    NetworkStateEstimate estimate; estimate.link_capacity_lower = 60000; estimate.link_capacity_upper = 120000;
    CHECK(!perform(p.io, [&](auto done) { p.a->setNetworkStateEstimate(estimate, done); }));
    p.negotiate(true); p.connected();
    until(p.io, [&] { return p.targetsA.back().generation == 1; });
    CHECK(p.a->snapshot().dtls->congestionControl->generation == 1);
    p.close();
}
} // namespace
int main()
{
    try
    {
        transportAndRestart(false); transportAndRestart(true); mediaNegotiation(); statesAndValidation();
        incompatibleCodecsAndNoCapabilities(); embeddedCandidatesAndAddingRtp(); externallyResolvedCandidate(); fingerprintFailure(); boundedCandidatesAndThreads();
        gccOverRealUdp();
        std::cout << "peerconnection tests passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
