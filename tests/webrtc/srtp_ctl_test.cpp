#include"webrtc/srtp_ctl.h"
#include"webrtc/srtcp_ctl.h"
#include"webrtc/peerconnection.h"

#include<srtp2/srtp.h>

#include<algorithm>
#include<atomic>
#include<chrono>
#include<cstring>
#include<iostream>
#include<mutex>
#include<stdexcept>
#include<thread>

using namespace webrtc;
using Bytes = UdpSocket::Bytes;
using namespace std::chrono_literals;
namespace net = boost::asio;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
const std::vector<DtlsSrtpProfile> profiles{DtlsSrtpProfile::Aes128CmSha1_80, DtlsSrtpProfile::Aes128CmSha1_32,
    DtlsSrtpProfile::AeadAes128Gcm, DtlsSrtpProfile::AeadAes256Gcm};
Bytes hex(const std::string& s)
{
    Bytes out;
    for (std::size_t i = 0; i < s.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}
DtlsSrtpKeyMaterial keys(DtlsSrtpProfile profile, unsigned seed = 1, std::uint64_t generation = 0)
{
    DtlsSrtpKeyMaterial k; k.profile = profile; k.generation = generation;
    const auto info = *srtpProfileInfo(profile);
    for (std::size_t i = 0; i < info.masterKeySize; ++i) { k.localMasterKey.push_back(seed + i); k.remoteMasterKey.push_back(seed + 71 + i); }
    for (std::size_t i = 0; i < info.masterSaltSize; ++i) { k.localMasterSalt.push_back(seed + 101 + i); k.remoteMasterSalt.push_back(seed + 171 + i); }
    return k;
}
DtlsSrtpKeyMaterial reverse(DtlsSrtpKeyMaterial k)
{ k.localMasterKey.swap(k.remoteMasterKey); k.localMasterSalt.swap(k.remoteMasterSalt); return k; }
void write32(Bytes& b, std::size_t p, std::uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) b[p + i] = static_cast<std::uint8_t>(n >> (24 - 8 * i)); }
Bytes rtp(std::uint32_t ssrc, unsigned seq)
{
    Bytes p{0x80, 96, static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq), 0, 1, 2, 3, 0, 0, 0, 0, 1, 3, 5, 7, 9, 11};
    write32(p, 8, ssrc); return p;
}
Bytes rtcp(std::uint32_t ssrc, unsigned serial)
{
    Bytes p{0x80, 201, 0, 1, 0, 0, 0, 0, 0x81, 206, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    write32(p, 4, ssrc); write32(p, 12, ssrc); write32(p, 16, serial); return p; // RR + PLI
}
template<class Ctl> Bytes packet(std::uint32_t ssrc, unsigned seq)
{ return std::is_same<Ctl, SrtcpCtl>::value ? rtcp(ssrc, seq) : rtp(ssrc, seq); }
template<class Ctl> void rejected(Ctl& ctl, Bytes p, SrtpError expected, bool protecting = false)
{
    const auto original = p;
    CHECK((protecting ? ctl.protect(p) : ctl.unprotect(p)) == expected); CHECK(p == original);
}
struct Native
{
    srtp_t session{nullptr}; bool control;
    Native(const DtlsSrtpKeyMaterial& k, bool sender, bool rtcpMode, bool authOnly = false) : control(rtcpMode)
    {
        srtp_policy_t policy{};
        const auto profile = k.profile == DtlsSrtpProfile::AeadAes128Gcm ? srtp_profile_aead_aes_128_gcm :
            k.profile == DtlsSrtpProfile::AeadAes256Gcm ? srtp_profile_aead_aes_256_gcm :
            k.profile == DtlsSrtpProfile::Aes128CmSha1_32 ? srtp_profile_aes128_cm_sha1_32 : srtp_profile_aes128_cm_sha1_80;
        CHECK(srtp_crypto_policy_set_from_profile_for_rtp(&policy.rtp, profile) == srtp_err_status_ok);
        CHECK(srtp_crypto_policy_set_from_profile_for_rtcp(&policy.rtcp, profile) == srtp_err_status_ok);
        if (authOnly) policy.rtcp.sec_serv = sec_serv_auth;
        policy.ssrc.type = sender ? ssrc_any_outbound : ssrc_any_inbound; policy.window_size = 128;
        auto master = k.localMasterKey; master.insert(master.end(), k.localMasterSalt.begin(), k.localMasterSalt.end());
        policy.key = master.data(); CHECK(srtp_create(&session, &policy) == srtp_err_status_ok);
    }
    ~Native() { if (session) srtp_dealloc(session); }
    Bytes process(Bytes p, bool protect)
    {
        std::vector<std::uint32_t> aligned((p.size() + SRTP_MAX_TRAILER_LEN + 7) / 4);
        std::memcpy(aligned.data(), p.data(), p.size()); int length = static_cast<int>(p.size());
        const auto result = control ? (protect ? srtp_protect_rtcp(session, aligned.data(), &length) : srtp_unprotect_rtcp(session, aligned.data(), &length)) :
            (protect ? srtp_protect(session, aligned.data(), &length) : srtp_unprotect(session, aligned.data(), &length));
        CHECK(result == srtp_err_status_ok);
        const auto* first = reinterpret_cast<const std::uint8_t*>(aligned.data()); return Bytes(first, first + length);
    }
};
void knownRtpVectors()
{
    // libSRTP v2.8.1 test/srtp_driver.c::srtp_validate / srtp_validate_gcm 的固定向量。
    const auto plain = hex("800f1234decafbadcafebabeabababababababababababababababab");
    for (const auto profile : {DtlsSrtpProfile::Aes128CmSha1_80, DtlsSrtpProfile::Aes128CmSha1_32, DtlsSrtpProfile::AeadAes128Gcm})
    {
        auto k = keys(profile);
        const auto combined = profile == DtlsSrtpProfile::AeadAes128Gcm ? hex("000102030405060708090a0b0c0d0e0fa0a1a2a3a4a5a6a7a8a9aaab") :
            hex("e1f97a0d3e018be0d64fa32c06de41390ec675ad498afeebb6960b3aabe6");
        k.localMasterKey.assign(combined.begin(), combined.begin() + 16); k.localMasterSalt.assign(combined.begin() + 16, combined.end());
        const auto expected = profile == DtlsSrtpProfile::AeadAes128Gcm ?
            hex("800f1234decafbadcafebabec5002ede04cfdd2eb91159e0880aa06ed2976826f796b201df3131a127e8a392") : profile == DtlsSrtpProfile::Aes128CmSha1_32 ?
            hex("800f1234decafbadcafebabe4e55dc4ce79978d88ca4d215949d2402b78d6acc") :
            hex("800f1234decafbadcafebabe4e55dc4ce79978d88ca4d215949d2402b78d6acc99ea179b8dbb");
        SrtpCtl a, b; CHECK(!a.setKeys(k) && !b.setKeys(reverse(k)));
        auto p = plain; CHECK(!a.protect(p) && p == expected); CHECK(!b.unprotect(p) && p == plain);
    }
}
template<class Ctl> void roundtrip(DtlsSrtpProfile profile)
{
    const auto k = keys(profile); Ctl a, b;
    CHECK(!a.setKeys(k) && !b.setKeys(reverse(k)));
    const auto info = *srtpProfileInfo(profile); const bool control = std::is_same<Ctl, SrtcpCtl>::value;
    CHECK(a.snapshot().overhead == (control ? info.rtcpOverhead() : info.rtpOverhead()));
    std::vector<Bytes> plain, wire;
    for (unsigned seq : {65534u, 65535u, 0u, 1u})
    {
        auto p = packet<Ctl>(0x12345678, seq); plain.push_back(p); CHECK(!a.protect(p)); wire.push_back(p);
        CHECK(p.size() == plain.back().size() + a.snapshot().overhead);
        const auto clear = control ? 8 : 12; CHECK(std::equal(p.begin(), p.begin() + clear, plain.back().begin()));
        CHECK(!std::equal(p.begin() + clear, p.begin() + plain.back().size(), plain.back().begin() + clear));
    }
    for (auto i : {0u, 2u, 1u, 3u}) { auto p = wire[i]; CHECK(!b.unprotect(p) && p == plain[i]); }
    rejected(b, wire[1], SrtpError::ReplayDetected);
    auto reversePacket = packet<Ctl>(0x12345678, 100); const auto reversePlain = reversePacket;
    CHECK(!b.protect(reversePacket) && !a.unprotect(reversePacket) && reversePacket == reversePlain);
    auto authentic = packet<Ctl>(0x87654321, 100); CHECK(!a.protect(authentic));
    for (auto offset : {std::size_t(control ? 4 : 8), std::size_t(control ? 9 : 13), authentic.size() - 1})
    { auto bad = authentic; bad[offset] ^= 1; rejected(b, bad, SrtpError::AuthenticationFailed); }
    auto p = authentic; CHECK(!b.unprotect(p)); // 认证失败不能消耗合法包的 index
    CHECK(b.snapshot().stats.authenticationFailures == 3 && b.snapshot().inboundStreams == 2);
    Native nativeSender(k, true, control), nativeReceiver(k, false, control);
    const auto original = packet<Ctl>(0x55443322, 100);
    auto nativeWire = nativeSender.process(original, true), ourWire = original;
    CHECK(!a.protect(ourWire) && ourWire == nativeWire);
    CHECK(nativeReceiver.process(ourWire, false) == original);
    CHECK(!b.unprotect(nativeWire) && nativeWire == original);
    auto minimal = packet<Ctl>(0x44444444, 1); minimal.resize(control ? 8 : 12); const auto minimumPlain = minimal;
    CHECK(!a.protect(minimal) && !b.unprotect(minimal) && minimal == minimumPlain);
    if (control)
    {
        Native authOnly(k, true, true, true); auto unencrypted = authOnly.process(rtcp(0x55555555, 1), true);
        rejected(b, unencrypted, SrtpError::InvalidPacket);
    }
}
template<class Ctl> void replayAndRekey()
{
    const auto profile = DtlsSrtpProfile::AeadAes128Gcm;
    auto k = keys(profile); Ctl a, b;
    CHECK(!a.setKeys(k) && !b.setKeys(reverse(k)));
    std::vector<Bytes> wire;
    for (unsigned i = 0; i <= 300; ++i) { auto p = packet<Ctl>(1, i); CHECK(!a.protect(p)); wire.push_back(std::move(p)); }
    auto first = wire[0], last = wire.back(); CHECK(!b.unprotect(first) && !b.unprotect(last));
    rejected(b, wire[1], SrtpError::ReplayTooOld);
    CHECK(!a.setKeys(k) && !b.setKeys(reverse(k))); rejected(b, wire.back(), SrtpError::ReplayDetected);
    if constexpr (std::is_same<Ctl, SrtpCtl>::value) rejected(a, packet<Ctl>(1, 300), SrtpError::ReplayDetected, true);
    auto conflicting = keys(profile, 2); CHECK(a.setKeys(conflicting) == SrtpError::ConflictingKeys);
    auto reused = k; reused.generation = 1; CHECK(a.setKeys(reused) == SrtpError::ReusedKey && a.snapshot().generation == 0);
    CHECK(!a.reset(1) && !b.reset(1)); CHECK(a.snapshot().state == SrtpState::New && a.snapshot().outboundStreams == 0);
    CHECK(a.setKeys(k) == SrtpError::GenerationMismatch); rejected(a, packet<Ctl>(1, 301), SrtpError::NotReady, true);
    const auto fresh = keys(profile, 3, 1); CHECK(!a.setKeys(fresh) && !b.setKeys(reverse(fresh)));
    auto delayed = wire.back(); const auto saved = delayed; CHECK(b.unprotect(delayed, 0) == SrtpError::GenerationMismatch && delayed == saved);
    rejected(b, wire.back(), SrtpError::AuthenticationFailed);
    auto p = packet<Ctl>(1, 0), original = p; CHECK(!a.protect(p, 1) && !b.unprotect(p, 1) && p == original);
    reused.generation = 2; CHECK(a.setKeys(reused) == SrtpError::ReusedKey);
    auto swapped = reverse(fresh); swapped.generation = 2; CHECK(a.setKeys(swapped) == SrtpError::ReusedKey);
    auto bad = fresh; bad.remoteMasterSalt.pop_back(); CHECK(a.setKeys(bad) == SrtpError::InvalidArgument && a.snapshot().state == SrtpState::Ready);
    CHECK(a.reset(1) == SrtpError::GenerationMismatch);
    a.close(); a.close(); CHECK(a.snapshot().state == SrtpState::Closed && a.setKeys(fresh) == SrtpError::Closed);
    rejected(a, packet<Ctl>(1, 1), SrtpError::Closed, true);
    typename Ctl::Config limited; limited.maxKeyGenerations = 1; Ctl once(limited);
    CHECK(!once.setKeys(k) && !once.setKeys(k)); CHECK(once.setKeys(fresh) == SrtpError::KeyHistoryFull);
}
template<class Ctl> void boundsAndMalformed()
{
    const auto k = keys(DtlsSrtpProfile::Aes128CmSha1_32);
    typename Ctl::Config c; c.maxPacketSize = 64; c.maxStreams = 1;
    Ctl a(c), b(c), sender; CHECK(!a.setKeys(k) && !b.setKeys(reverse(k)) && !sender.setKeys(k));
    for (unsigned i = 1; i <= 8; ++i)
    {
        auto bad = packet<Ctl>(i, 1); CHECK(!sender.protect(bad)); bad.back() ^= 1;
        rejected(b, bad, SrtpError::AuthenticationFailed);
    }
    CHECK(b.snapshot().inboundStreams == 0);
    auto p = packet<Ctl>(99, 1); const auto plain = p; CHECK(!a.protect(p) && !b.unprotect(p) && p == plain);
    rejected(a, packet<Ctl>(100, 2), SrtpError::StreamLimit, true);
    p = packet<Ctl>(101, 2); CHECK(!sender.protect(p)); rejected(b, p, SrtpError::StreamLimit);
    rejected(a, {}, SrtpError::InvalidPacket, true); rejected(b, Bytes(8), SrtpError::InvalidPacket);
    p = packet<Ctl>(99, 3); p[0] = 0; rejected(a, p, SrtpError::InvalidPacket, true);
    p = packet<Ctl>(99, 3); p.resize(65); rejected(a, p, SrtpError::PacketTooLarge, true);
    rejected(b, Bytes(65), SrtpError::PacketTooLarge);
    p = packet<Ctl>(99, 3); CHECK(!a.protect(p)); // oversized/invalid 输入不能提前消耗序号
    if constexpr (std::is_same<Ctl, SrtpCtl>::value)
    {
        Ctl extensions; CHECK(!extensions.setKeys(k));
        auto e = hex("b1600001000000010102030405060708bede00011000abcd1112131400000004"); // CSRC + ext + padding
        Ctl receiver; CHECK(!receiver.setKeys(reverse(k))); const auto original = e;
        CHECK(!extensions.protect(e) && !receiver.unprotect(e) && e == original);
        auto broken = original; broken.back() = 0; rejected(extensions, broken, SrtpError::InvalidPacket, true);
        broken = original; broken[18] = 0xff; rejected(extensions, broken, SrtpError::InvalidPacket, true);
        broken = rtp(10, 1); broken[1] = 64; rejected(extensions, broken, SrtpError::InvalidPacket, true);
        typename Ctl::Config nonMux; nonMux.rtcpMux = false; Ctl generic(nonMux); CHECK(!generic.setKeys(k) && !generic.protect(broken));
    }
    else
    {
        Ctl rtcpOnly; CHECK(!rtcpOnly.setKeys(k));
        auto malformed = rtcp(1, 1); malformed[3] = 20; rejected(rtcpOnly, malformed, SrtpError::InvalidPacket, true);
        malformed = rtcp(1, 1); malformed[8] |= 0x20; malformed.back() = 0; rejected(rtcpOnly, malformed, SrtpError::InvalidPacket, true);
        auto pli = hex("81ce00020102030405060708"); Ctl receive; CHECK(!receive.setKeys(reverse(k))); const auto original = pli;
        CHECK(!rtcpOnly.protect(pli) && !receive.unprotect(pli) && pli == original);
    }
    auto invalid = k; invalid.profile = static_cast<DtlsSrtpProfile>(999); CHECK(a.setKeys(invalid) == SrtpError::UnsupportedProfile);
    invalid = k; invalid.component = 2; CHECK(a.setKeys(invalid) == SrtpError::InvalidArgument);
    invalid = k; invalid.remoteMasterKey = invalid.localMasterKey; invalid.remoteMasterSalt = invalid.localMasterSalt;
    Ctl identity; CHECK(identity.setKeys(invalid) == SrtpError::InvalidArgument);
}
template<class Ctl> void concurrency()
{
    Ctl sender, receiver; const auto k = keys(DtlsSrtpProfile::AeadAes256Gcm);
    CHECK(!sender.setKeys(k) && !receiver.setKeys(reverse(k)));
    std::atomic<unsigned> successes{0}; std::vector<std::thread> workers;
    for (unsigned thread = 1; thread <= 4; ++thread) workers.emplace_back([&, thread] {
        for (unsigned seq = 1; seq <= 32; ++seq)
        {
            auto p = packet<Ctl>(thread, seq), original = p;
            CHECK(!sender.protect(p) && !receiver.unprotect(p) && p == original); ++successes;
        }
    });
    for (auto& thread : workers) thread.join();
    CHECK(successes == 128 && sender.snapshot().outboundStreams == 4 && receiver.snapshot().inboundStreams == 4);
}
template<class Predicate> void until(net::io_context& io, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
template<class Action> void perform(net::io_context& io, Action action)
{
    bool done = false; action([&](auto ec) { CHECK(!ec); done = true; }); until(io, [&] { return done; });
}
void integration(bool managed = false)
{
    net::io_context io; SrtpCtl rtpA, rtpB; SrtcpCtl rtcpA, rtcpB;
    std::unique_ptr<PeerConnection> a, b;
    unsigned keysA = 0, keysB = 0, errors = 0, received = 0;
    std::vector<Bytes> plaintext;
    std::shared_ptr<DataChannel> localChannel, remoteChannel;
    unsigned dataMessages = 0;
    std::vector<RtcpKeyFrameEvent> keyFrameEvents;
    std::vector<TwccFeedbackEvent> twccEvents;
    std::vector<TransportPacketsFeedback> transportEvents;
    std::vector<RtcpReportEvent> reportEvents;
    PeerConnection::Config c; c.media = {PeerMedia::video("video", {{96, "VP8", 90000, 1, {}}})};
    c.dtls.enableSrtp = managed; c.enableDataChannels = managed;
    if (managed)
    {
        c.media[0].feedback = {{std::nullopt, "nack"}, {std::nullopt, "nack pli"}, {std::nullopt, "ccm fir"}, {std::nullopt, "transport-cc"}};
        c.media[0].extmaps = {{5, std::nullopt, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", std::nullopt}};
        c.dtls.rtcp.reportInterval = 50ms; c.dtls.rtcp.twccInterval = 10ms;
    }
    c.ice.ice.bindings = {{{net::ip::make_address("127.0.0.1"), 0}, 1, 65535}};
    c.ice.ice.checkInterval = 5ms;
    PeerConnection::Callbacks ha, hb;
    ha.onIceCandidate = [&](auto candidate) { b->addIceCandidate(std::move(candidate), [](auto ec) { CHECK(!ec); }); };
    hb.onIceCandidate = [&](auto candidate) { a->addIceCandidate(std::move(candidate), [](auto ec) { CHECK(!ec); }); };
    ha.onSrtpKeys = [&](auto k) {
        if (managed) { CHECK(a->snapshot().dtls->srtp->state == SrtpState::Ready && a->snapshot().dtls->srtcp->state == SrtpState::Ready); }
        else { CHECK(!rtpA.setKeys(k) && !rtcpA.setKeys(k)); }
        ++keysA;
    };
    hb.onSrtpKeys = [&](auto k) {
        if (managed) { CHECK(b->snapshot().dtls->srtp->state == SrtpState::Ready && b->snapshot().dtls->srtcp->state == SrtpState::Ready); }
        else { CHECK(!rtpB.setKeys(k) && !rtcpB.setKeys(k)); }
        ++keysB;
    };
    ha.onConnectionState = [&](auto state) {
        const auto snapshot = a->snapshot();
        if (managed) { if (state == PeerConnectionState::Connected) CHECK(snapshot.dtls->srtp->state == SrtpState::Ready); return; }
        if (snapshot.ice && snapshot.ice->generation > rtpA.snapshot().generation)
        { CHECK(!rtpA.reset(snapshot.ice->generation) && !rtcpA.reset(snapshot.ice->generation)); }
        if (state == PeerConnectionState::Closed) { rtpA.close(); rtcpA.close(); }
    };
    hb.onConnectionState = [&](auto state) {
        const auto snapshot = b->snapshot();
        if (managed) { if (state == PeerConnectionState::Connected) CHECK(snapshot.dtls->srtcp->state == SrtpState::Ready); return; }
        if (snapshot.ice && snapshot.ice->generation > rtpB.snapshot().generation)
        { CHECK(!rtpB.reset(snapshot.ice->generation) && !rtcpB.reset(snapshot.ice->generation)); }
        if (state == PeerConnectionState::Closed) { rtpB.close(); rtcpB.close(); }
    };
    hb.onSecurePacket = [&](auto packet) {
        const auto result = packet.type == IcePacketType::Rtp ? rtpB.unprotect(packet.datagram.payload, packet.generation) : rtcpB.unprotect(packet.datagram.payload, packet.generation);
        CHECK(!result); plaintext.push_back(std::move(packet.datagram.payload)); ++received;
    };
    ha.onSecurePacket = [&](auto packet) {
        const auto result = packet.type == IcePacketType::Rtp ? rtpA.unprotect(packet.datagram.payload, packet.generation) : rtcpA.unprotect(packet.datagram.payload, packet.generation);
        CHECK(!result); plaintext.push_back(std::move(packet.datagram.payload)); ++received;
    };
    if (managed)
    {
        ha.onSecurePacket = hb.onSecurePacket = [](auto) { CHECK(false); };
        ha.onRtpPacket = hb.onRtpPacket = [&](auto packet) {
            plaintext.push_back(std::move(packet.datagram.payload)); ++received;
        };
        ha.onRtcpPacket = hb.onRtcpPacket = [&](auto packet) {
            if (packet.datagram.payload == rtcp(0x05060708, 1)) { plaintext.push_back(std::move(packet.datagram.payload)); ++received; }
        };
        ha.onKeyFrameRequest = [&](auto event) { keyFrameEvents.push_back(std::move(event)); };
        ha.onTwccFeedback = [&](auto event) { twccEvents.push_back(std::move(event)); };
        ha.onTransportFeedback = [&](auto event) { transportEvents.push_back(std::move(event)); };
        ha.onReceiverReport = [&](auto event) { reportEvents.push_back(std::move(event)); };
        hb.onDataChannel = [&](auto channel) {
            remoteChannel = channel; DataChannel::Callbacks handlers;
            handlers.onMessage = [&](auto message) { CHECK(message.data == Bytes({'o','k'})); ++dataMessages; };
            channel->setCallbacks(handlers);
        };
    }
    ha.onError = hb.onError = [&](auto, auto message) { std::cerr << message << '\n'; ++errors; };
    a = std::make_unique<PeerConnection>(io, c, ha); b = std::make_unique<PeerConnection>(io, c, hb);
    if (managed)
    {
        DataChannelConfig channel; channel.label = "commands";
        a->createDataChannel(channel, [&](auto ec, auto result) { CHECK(!ec); localChannel = result; });
        until(io, [&] { return bool(localChannel); });
    }
    auto negotiate = [&](bool restart) {
        std::optional<PeerSessionDescription> offer, answer;
        a->createOffer({restart}, [&](auto ec, auto d) { CHECK(!ec && d); offer = std::move(d); }); until(io, [&] { return offer.has_value(); });
        perform(io, [&](auto done) { a->setLocalDescription(*offer, done); });
        perform(io, [&](auto done) { b->setRemoteDescription(*offer, done); });
        b->createAnswer([&](auto ec, auto d) { CHECK(!ec && d); answer = std::move(d); }); until(io, [&] { return answer.has_value(); });
        perform(io, [&](auto done) { b->setLocalDescription(*answer, done); });
        perform(io, [&](auto done) { a->setRemoteDescription(*answer, done); });
        until(io, [&] {
            if (a->snapshot().connectionState != PeerConnectionState::Connected || b->snapshot().connectionState != PeerConnectionState::Connected) return false;
            if (!managed) return rtpA.snapshot().state == SrtpState::Ready && rtpB.snapshot().state == SrtpState::Ready;
            return a->snapshot().dtls->srtp->state == SrtpState::Ready && b->snapshot().dtls->srtcp->state == SrtpState::Ready &&
                remoteChannel && localChannel->snapshot().state == DataChannelState::Open && remoteChannel->snapshot().state == DataChannelState::Open;
        });
    };
    Bytes oldWire;
    for (unsigned round = 0; round < 2; ++round)
    {
        negotiate(round != 0); CHECK(keysA == round + 1 && keysB == round + 1);
        const auto generation = a->snapshot().ice->generation; CHECK(generation == round);
        auto p = rtp(0x01020304, 7), q = rtcp(0x05060708, 1); auto originalP = p; const auto originalQ = q;
        if (!managed)
        {
            CHECK(!rtpA.protect(p, generation) && !rtcpB.protect(q, generation));
            if (round == 0) oldWire = p;
            else { auto delayed = oldWire; CHECK(rtpB.unprotect(delayed, 0) == SrtpError::GenerationMismatch && delayed == oldWire); }
        }
        const auto before = received; unsigned sent = 0;
        if (managed)
        {
            perform(io, [&](auto done) { a->setPacingRates(64000, 16000, done); });
            CHECK(a->snapshot().dtls->pacer->paddingRateBps == 16000);
            perform(io, [&](auto done) { a->setPacingRate(64000, done); });
            CHECK(a->snapshot().dtls->pacer->paddingRateBps == 16000);
            perform(io, [&](auto done) { a->setPacingPaused(true, done); });
            CHECK(a->snapshot().dtls->pacer->paused && a->snapshot().dtls->pacer->rateBps == 64000);
            CHECK(setRtpTransportSequence(originalP, 5, 0));
            const auto originalSendSize = p.size();
            a->asyncSendRtp(std::move(p), [&, originalSendSize](auto ec, auto bytes) { CHECK(!ec && bytes == originalSendSize); ++sent; });
            b->asyncSendRtcp(std::move(q), [&](auto ec, auto bytes) { CHECK(!ec && bytes == originalQ.size()); ++sent; });
            localChannel->sendText("ok");
            until(io, [&] { return a->snapshot().dtls->pacer->queuedPackets == 1 && sent == 1; });
            CHECK(a->snapshot().dtls->srtp->stats.protectedPackets == round); // queueing doesn't consume crypto sequence
            perform(io, [&](auto done) { a->setPacingPaused(false, done); });
        }
        else
        {
            a->asyncSendSecurePacket(std::move(p), [&](auto ec, auto bytes) { CHECK(!ec && bytes > 0); ++sent; });
            b->asyncSendSecurePacket(std::move(q), [&](auto ec, auto bytes) { CHECK(!ec && bytes > 0); ++sent; });
        }
        until(io, [&] { return sent == 2 && received == before + 2; });
        CHECK(std::find(plaintext.begin(), plaintext.end(), originalP) != plaintext.end());
        CHECK(std::find(plaintext.begin(), plaintext.end(), originalQ) != plaintext.end());
        if (managed)
        {
            until(io, [&] { return dataMessages == round + 1 && twccEvents.size() == round + 1 && transportEvents.size() == round + 1 && reportEvents.size() >= round + 1; });
            CHECK(transportEvents.back().generation == round && transportEvents.back().ReceivedWithSendInfo().size() == 1);
            CHECK(transportEvents.back().packet_feedbacks[0].sent_packet.send_time == twccEvents.back().packets[0].sentAt);
            CHECK(twccEvents.back().generation == round && twccEvents.back().received == 1 && twccEvents.back().packets[0].rtpSequence == 7);
            CHECK(a->snapshot().negotiatedMedia[0].feedback.size() == 4 && a->snapshot().negotiatedMedia[0].extmaps[0].id == 5);
            perform(io, [&](auto done) { b->requestPli(0x01020304, done); });
            until(io, [&] { return keyFrameEvents.size() == round + 1; });
            CHECK(keyFrameEvents.back().generation == round && keyFrameEvents.back().feedback.mediaSsrc == 0x01020304);
        }
    }
    CHECK(errors == 0);
    unsigned closed = 0; a->close([&](auto ec) { CHECK(!ec); ++closed; }); b->close([&](auto ec) { CHECK(!ec); ++closed; });
    until(io, [&] { return closed == 2; });
    if (managed) CHECK(a->snapshot().dtls->srtp->state == SrtpState::Closed && b->snapshot().dtls->srtcp->state == SrtpState::Closed && localChannel->snapshot().state == DataChannelState::Closed);
    else CHECK(rtpA.snapshot().state == SrtpState::Closed && rtcpB.snapshot().state == SrtpState::Closed);
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        CHECK(argc == 2); const std::string group = argv[1];
        if (group == "rtp")
        {
            knownRtpVectors(); for (auto p : profiles) roundtrip<SrtpCtl>(p);
            replayAndRekey<SrtpCtl>(); boundsAndMalformed<SrtpCtl>(); concurrency<SrtpCtl>();
        }
        else if (group == "rtcp")
        {
            for (auto p : profiles) roundtrip<SrtcpCtl>(p);
            replayAndRekey<SrtcpCtl>(); boundsAndMalformed<SrtcpCtl>(); concurrency<SrtcpCtl>();
        }
        else { CHECK(group == "integration"); integration(); integration(true); }
        std::cout << "PASS: " << group << " SRTP/SRTCP tests\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
