#include"webrtc/ice_transport.h"

#include<boost/asio/executor_work_guard.hpp>

#include<atomic>
#include<chrono>
#include<iostream>
#include<stdexcept>
#include<thread>

using namespace webrtc;
using namespace std::chrono_literals;
namespace net = boost::asio;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

namespace
{
template<class Predicate> void until(net::io_context& io, Predicate predicate, std::chrono::milliseconds timeout = 2500ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
void runFor(net::io_context& io, std::chrono::milliseconds time) { io.restart(); io.run_for(time); }
IceTransport::Config config(StunIceRole role = StunIceRole::Controlling, bool ipv6 = false)
{
    IceTransport::Config value;
    value.ice.role = role; value.ice.checkInterval = 10ms;
    value.ice.bindings = {{{net::ip::make_address(ipv6 ? "::1" : "127.0.0.1"), 0}, 1, 65535}};
    return value;
}
std::string fingerprint(char ch)
{
    std::string value;
    for (unsigned i = 0; i < 32; ++i) { if (i) value += ':'; value += ch; value += ch; }
    return value;
}
SdpSession description(const IceAgent::Snapshot& snapshot, SdpDescriptionType type, char fp = 'A',
                       SdpSetupRole answerRole = SdpSetupRole::Active, bool candidates = true)
{
    SdpSession session;
    session.connection = SdpConnection{};
    if (!snapshot.localCandidates.empty() && snapshot.localCandidates[0].udpEndpoint()->address().is_v6())
        session.connection = SdpConnection{"IP6", "::"};
    session.common.iceUfrag = snapshot.localCredentials.ufrag;
    session.common.icePwd = snapshot.localCredentials.password;
    session.common.iceOptions = {"trickle"};
    session.common.fingerprints = {{"sha-256", fingerprint(fp)}};
    session.common.setup = type == SdpDescriptionType::Offer ? SdpSetupRole::ActPass : answerRole;
    session.groups = {{"BUNDLE", {"video", "data"}}};
    SdpMedia video;
    video.mid = "video"; video.formats = {"102"}; video.rtcpMux = true;
    video.codecs = {{102, "H264", 90000, 1, std::nullopt}};
    if (candidates) { video.candidates = snapshot.localCandidates; video.endOfCandidates = true; }
    SdpMedia data;
    data.mid = "data"; data.kind = "application"; data.protocol = "UDP/DTLS/SCTP";
    data.formats = {"webrtc-datachannel"}; data.sctpPort = 5000;
    if (type == SdpDescriptionType::Offer) { data.port = 0; data.bundleOnly = true; }
    session.media = {video, data};
    CHECK(session.validateWebRtc(type));
    return session;
}
const UdpSocket::Bytes dtls{22, 0xfe, 0xfd, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 42};
const UdpSocket::Bytes rtp{0x80, 111, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 11, 22, 33};
const UdpSocket::Bytes rtcp{0x80, 201, 0, 1, 0, 0, 0, 1};

void close(net::io_context& io, IceTransport& a, IceTransport* b = nullptr)
{
    unsigned done = 0;
    a.close([&](auto ec) { CHECK(!ec); ++done; });
    if (b) b->close([&](auto ec) { CHECK(!ec); ++done; });
    until(io, [&] { return done == (b ? 2u : 1u); });
    CHECK(a.snapshot().state == IceTransportState::Closed && !a.isReady() && !a.snapshot().dtls);
    runFor(io, 5ms);
}
void gather(net::io_context& io, IceTransport& a, IceTransport& b)
{
    unsigned done = 0;
    a.gather([&](auto ec) { CHECK(!ec); ++done; });
    b.gather([&](auto ec) { CHECK(!ec); ++done; });
    until(io, [&] { return done == 2 && a.snapshot().ice.gatheringState == IceGatheringState::Complete &&
        b.snapshot().ice.gatheringState == IceGatheringState::Complete; });
}
void exchange(IceTransport& a, IceTransport& b)
{
    const auto first = a.snapshot().ice, second = b.snapshot().ice;
    a.setRemoteCredentials(second.localCredentials); b.setRemoteCredentials(first.localCredentials);
    for (const auto& candidate : second.localCandidates) a.addRemoteCandidate(candidate);
    for (const auto& candidate : first.localCandidates) b.addRemoteCandidate(candidate);
    a.endOfRemoteCandidates(second.localCredentials.ufrag); b.endOfRemoteCandidates(first.localCredentials.ufrag);
    a.start(); b.start();
}
void classification()
{
    CHECK(classifyIcePacket(nullptr, 0) == IcePacketType::Unknown);
    CHECK(classifyIcePacket(nullptr, 10) == IcePacketType::Unknown);
    CHECK(classifyIcePacket({}) == IcePacketType::Unknown);
    for (unsigned first = 0; first < 256; ++first)
    {
        UdpSocket::Bytes bytes{static_cast<std::uint8_t>(first), 111};
        IcePacketType expected = IcePacketType::Unknown;
        if (first <= 3) expected = IcePacketType::Stun;
        else if (first >= 16 && first <= 19) expected = IcePacketType::Zrtp;
        else if (first >= 20 && first <= 63) expected = IcePacketType::Dtls;
        else if (first >= 64 && first <= 79) expected = IcePacketType::TurnChannelData;
        else if (first >= 128 && first <= 191) expected = IcePacketType::Rtp;
        CHECK(classifyIcePacket(bytes) == expected);
    }
    for (unsigned second = 0; second < 256; ++second)
        CHECK(classifyIcePacket({0x80, static_cast<std::uint8_t>(second)}) ==
            (second >= 192 && second <= 223 ? IcePacketType::Rtcp : IcePacketType::Rtp));
    CHECK(classifyIcePacket({0x80}) == IcePacketType::Unknown);
    CHECK(classifyIcePacket(dtls) == IcePacketType::Dtls);
    CHECK(classifyIcePacket(rtp) == IcePacketType::Rtp && classifyIcePacket(rtcp) == IcePacketType::Rtcp);
}

void sdpAndData(bool ipv6 = false, bool preferClient = true)
{
    net::io_context io;
    unsigned readyA = 0, readyB = 0, selectedA = 0, received = 0, sent = 0;
    bool insideCallback = false;
    IceTransport::Callbacks aCallbacks, bCallbacks;
    IceTransport* first = nullptr;
    aCallbacks.onSelectedPair = [&](auto pair) { CHECK(pair.valid && pair.nominated); ++selectedA; };
    aCallbacks.onDtlsReady = [&](auto ready) {
        CHECK(first && first->isReady());
        CHECK(first->snapshot().dtls && ready.parameters.transportMid == "video");
        CHECK(ready.parameters.remoteFingerprints[0].value == fingerprint('B'));
        CHECK(ready.parameters.localSetup == (preferClient ? SdpSetupRole::Passive : SdpSetupRole::Active));
        CHECK(ready.generation == 0); ++readyA;
    };
    bCallbacks.onDtlsReady = [&](auto ready) {
        CHECK(ready.parameters.remoteFingerprints[0].value == fingerprint('A'));
        CHECK(ready.parameters.remoteSetup == SdpSetupRole::ActPass);
        CHECK(ready.parameters.localSetup == (preferClient ? SdpSetupRole::Active : SdpSetupRole::Passive)); ++readyB;
    };
    bCallbacks.onPacket = [&](auto packet) {
        CHECK(!insideCallback); insideCallback = true;
        CHECK(packet.generation == 0 && packet.component == 1);
        CHECK(packet.datagram.remoteEndpoint == first->selectedPair()->localEndpoint);
        CHECK(packet.datagram.receivedAt != UdpSocket::Clock::time_point{});
        CHECK((packet.type == IcePacketType::Dtls && packet.datagram.payload == dtls) ||
              (packet.type == IcePacketType::Rtp && packet.datagram.payload == rtp) ||
              (packet.type == IcePacketType::Rtcp && packet.datagram.payload == rtcp));
        ++received; insideCallback = false;
    };
    auto answerConfig = config(StunIceRole::Controlled, ipv6); answerConfig.preferDtlsClient = preferClient;
    IceTransport a(io, config(StunIceRole::Controlling, ipv6), aCallbacks), b(io, answerConfig, bCallbacks); first = &a;
    bool rejectedBeforeReady = false;
    a.asyncSendDtls(dtls, 1, [&](auto ec, auto size) { CHECK(ec == net::error::not_connected && size == 0); rejectedBeforeReady = true; });
    gather(io, a, b); CHECK(rejectedBeforeReady);
    bool offerApplied = false, answerApplied = false;
    b.setRemoteDescription(description(a.snapshot().ice, SdpDescriptionType::Offer), "data", SdpDescriptionType::Offer,
        [&](auto ec) { CHECK(!ec); offerApplied = true; });
    const auto role = preferClient ? SdpSetupRole::Active : SdpSetupRole::Passive;
    a.setRemoteDescription(description(b.snapshot().ice, SdpDescriptionType::Answer, 'B', role), "data", SdpDescriptionType::Answer,
        [&](auto ec) { CHECK(!ec); answerApplied = true; });
    a.start(); b.start();
    until(io, [&] { return offerApplied && answerApplied && readyA == 1 && readyB == 1 &&
        a.snapshot().state == IceTransportState::Completed && b.snapshot().state == IceTransportState::Completed; });
    CHECK(selectedA == 1 && a.snapshot().ice.role == StunIceRole::Controlling);
    auto done = [&](auto ec, auto size) { CHECK(!ec && size > 0); ++sent; };
    a.asyncSendDtls(dtls, 1, done); a.asyncSendRtp(rtp, 1, done); a.asyncSendRtcp(rtcp, 1, done);
    until(io, [&] { return sent == 3 && received == 3; });
    CHECK(a.snapshot().stats.sentPackets == 3 && a.snapshot().stats.sentBytes == dtls.size() + rtp.size() + rtcp.size());
    CHECK(b.snapshot().stats.receivedPackets == 3 && b.snapshot().stats.receivedBytes == a.snapshot().stats.sentBytes);
    unsigned errors = 0;
    a.asyncSend({}, 1, [&](auto ec, auto size) { CHECK(ec == net::error::invalid_argument && size == 0); ++errors; });
    a.asyncSend(StunMessage::bindingRequest().encode(), 1, [&](auto ec, auto) { CHECK(ec == net::error::invalid_argument); ++errors; });
    a.asyncSendDtls(rtp, 1, [&](auto ec, auto) { CHECK(ec == net::error::invalid_argument); ++errors; });
    a.asyncSendDtls(dtls, 2, [&](auto ec, auto) { CHECK(ec == net::error::not_connected); ++errors; });
    a.asyncSendDtls(UdpSocket::Bytes(65508, 22), 1, [&](auto ec, auto) { CHECK(ec == net::error::message_size); ++errors; });
    until(io, [&] { return errors == 5; });
    CHECK(a.snapshot().stats.failedSends == 6 && received == 3);
    // 同一代更换 fingerprint 拒绝，快照/就绪回调保持原身份。
    auto changed = description(b.snapshot().ice, SdpDescriptionType::Answer, 'C', role, false);
    a.setRemoteDescription(changed, "data", SdpDescriptionType::Answer, [&](auto ec) { CHECK(ec == net::error::already_started); ++errors; });
    until(io, [&] { return errors == 6; });
    CHECK(a.snapshot().dtls->remoteFingerprints[0].value == fingerprint('B') && readyA == 1);
    changed.common.fingerprints.clear();
    a.setRemoteDescription(changed, "data", SdpDescriptionType::Answer, [&](auto ec) { CHECK(ec == net::error::invalid_argument); ++errors; });
    a.restart(a.snapshot().ice.localCredentials, StunIceRole::Controlling, [&](auto ec) { CHECK(ec == net::error::invalid_argument); ++errors; });
    until(io, [&] { return errors == 8; });
    CHECK(a.isReady() && a.snapshot().generation == 0 && readyA == 1);
    close(io, a, &b);
}

void lateMetadataAndRestart()
{
    net::io_context io;
    unsigned ready = 0;
    IceTransport::Callbacks callbacks;
    callbacks.onDtlsReady = [&](auto value) { CHECK(value.generation == ready); ++ready; };
    IceTransport a(io, config(), callbacks), b(io, config(StunIceRole::Controlled));
    gather(io, a, b); exchange(a, b);
    until(io, [&] { return a.isReady() && b.isReady(); });
    CHECK(ready == 0 && !a.snapshot().dtls);
    auto remote = description(b.snapshot().ice, SdpDescriptionType::Answer, 'B', SdpSetupRole::Passive, false);
    unsigned applied = 0;
    a.setRemoteDescription(remote, "data", SdpDescriptionType::Answer, [&](auto ec) { CHECK(!ec); ++applied; });
    until(io, [&] { return applied == 1 && ready == 1; });
    remote.common.fingerprints[0].algorithm = "SHA-256";
    remote.common.fingerprints[0].value = fingerprint('b'); // 大小写等价不重新建立 DTLS
    a.setRemoteDescription(remote, "video", SdpDescriptionType::Answer, [&](auto ec) { CHECK(!ec); ++applied; });
    until(io, [&] { return applied == 2; }); CHECK(ready == 1);
    const auto before = a.snapshot();
    unsigned restarted = 0;
    a.restart({}, StunIceRole::Controlling, [&](auto ec) { CHECK(!ec); ++restarted; });
    b.restart({}, StunIceRole::Controlled, [&](auto ec) { CHECK(!ec); ++restarted; });
    until(io, [&] { return restarted == 2; });
    CHECK(a.snapshot().generation == 1 && !a.isReady() && !a.snapshot().dtls);
    CHECK(a.snapshot().ice.localCredentials.ufrag != before.ice.localCredentials.ufrag);
    CHECK(a.snapshot().ice.localCandidates[0].udpEndpoint() == before.ice.localCandidates[0].udpEndpoint());
    bool rejected = false;
    a.setRemoteCredentials(b.snapshot().ice.localCredentials);
    a.addRemoteCandidate(b.snapshot().ice.localCandidates[0], [&](auto ec) { CHECK(ec); rejected = true; }, {}, before.ice.remoteCredentials->ufrag);
    a.setRemoteDescription(description(b.snapshot().ice, SdpDescriptionType::Answer, 'C', SdpSetupRole::Passive), "data", SdpDescriptionType::Answer);
    b.setRemoteDescription(description(a.snapshot().ice, SdpDescriptionType::Offer, 'D'), "data", SdpDescriptionType::Offer);
    a.start(); b.start();
    until(io, [&] { return rejected && ready == 2 && a.isReady() && b.isReady(); });
    CHECK(a.snapshot().dtls->remoteFingerprints[0].value == fingerprint('C'));
    close(io, a, &b);
}

void receiveFilteringAndComponents()
{
    net::io_context io;
    auto transportConfig = config(); transportConfig.maxDatagramSize = 32;
    transportConfig.ice.components = {1, 2};
    transportConfig.ice.bindings.push_back({{net::ip::make_address("127.0.0.1"), 0}, 2, 65535});
    auto peerConfig = transportConfig.ice; peerConfig.role = StunIceRole::Controlled;
    unsigned received = 0;
    IceTransport::Callbacks callbacks;
    callbacks.onPacket = [&](auto value) { CHECK(value.component == 2 && value.type == IcePacketType::Rtcp); ++received; };
    IceTransport a(io, transportConfig, callbacks);
    IceAgent peer(io, peerConfig);
    a.gather(); peer.gather();
    until(io, [&] { return a.snapshot().ice.localCandidates.size() == 2 && peer.snapshot().localCandidates.size() == 2; });
    a.setRemoteCredentials(peer.snapshot().localCredentials); peer.setRemoteCredentials(a.snapshot().ice.localCredentials);
    for (auto candidate : peer.snapshot().localCandidates) a.addRemoteCandidate(candidate);
    for (auto candidate : a.snapshot().ice.localCandidates) peer.addRemoteCandidate(candidate);
    a.start(); peer.start();
    until(io, [&] { return a.isReady(1) && a.isReady(2) && peer.snapshot().selectedPairs.size() == 2; });
    peer.asyncSend({4, 1}); peer.asyncSend({16, 1}); peer.asyncSend({64, 1}); peer.asyncSend({});
    peer.asyncSend(UdpSocket::Bytes(33, 22)); peer.asyncSend(rtcp, 2);
    until(io, [&] { return a.snapshot().stats.discardedPackets == 5 && received == 1; });
    CHECK(a.snapshot().stats.receivedPackets == 1);
    bool closed = false;
    peer.close([&](auto ec) { CHECK(!ec); closed = true; }); close(io, a); until(io, [&] { return closed; });
}

void cancellationAndClose()
{
    net::io_context io;
    IceTransport a(io, config()), b(io, config(StunIceRole::Controlled));
    gather(io, a, b); exchange(a, b); until(io, [&] { return a.isReady() && b.isReady(); });
    unsigned sent = 0, closed = 0;
    for (unsigned i = 0; i < 20; ++i) a.asyncSend(dtls, 1, [&](auto, auto) { ++sent; });
    a.close([&](auto ec) { CHECK(!ec && sent == 20); ++closed; });
    a.close([&](auto ec) { CHECK(!ec && sent == 20); ++closed; });
    b.close([&](auto ec) { CHECK(!ec); ++closed; });
    until(io, [&] { return closed == 3; });

    IceTransport resolving(io, config());
    auto candidate = IceCandidate::host({net::ip::make_address("127.0.0.1"), 9999}, "dns");
    candidate.address = "ice-transport.invalid";
    unsigned canceled = 0;
    resolving.addRemoteCandidate(candidate, [&](auto ec) { CHECK(ec == net::error::operation_aborted); ++canceled; });
    resolving.start([&](auto ec) { CHECK(ec == net::error::operation_aborted); ++canceled; });
    resolving.close([&](auto ec) { CHECK(!ec); ++closed; });
    until(io, [&] { return closed == 4; }); CHECK(canceled == 2);

    auto limitedConfig = config(); limitedConfig.maxPendingOperations = 1;
    IceTransport limited(io, limitedConfig);
    unsigned completed = 0;
    limited.gather([&](auto ec) { CHECK(!ec); ++completed; });
    limited.start([&](auto ec) { CHECK(ec == net::error::no_buffer_space); ++completed; });
    until(io, [&] { return completed == 2; }); close(io, limited);

    IceTransport restarting(io, config());
    unsigned notifications = 0;
    restarting.restart({}, StunIceRole::Controlled, [&](auto ec) { CHECK(ec == net::error::operation_aborted); ++notifications; });
    restarting.close([&](auto ec) { CHECK(!ec); ++notifications; });
    until(io, [&] { return notifications == 2; });
    CHECK(!restarting.isReady() && restarting.snapshot().state == IceTransportState::Closed);
}

void concurrentLifecycle()
{
    net::io_context io;
    auto work = net::make_work_guard(io);
    IceTransport transport(io, config());
    std::atomic<unsigned> count{0};
    transport.gather([&](auto ec) { CHECK(!ec); ++count; });
    transport.start([&](auto ec) { CHECK(!ec); ++count; });
    std::thread first([&] { io.run(); }), second([&] { io.run(); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (count < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    transport.close([&](auto ec) { CHECK(!ec); ++count; });
    work.reset(); first.join(); second.join();
    CHECK(count == 3 && transport.snapshot().state == IceTransportState::Closed);
}
} // namespace

int main()
{
    try
    {
        classification(); sdpAndData(); sdpAndData(true); sdpAndData(false, false);
        lateMetadataAndRestart(); receiveFilteringAndComponents(); cancellationAndClose(); concurrentLifecycle();
        std::cout << "PASS: ICE transport SDP/BUNDLE, DTLS parameters, demux, data, stats, restart and lifecycle\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
