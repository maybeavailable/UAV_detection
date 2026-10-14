#include"webrtc/dtlstransport.h"

#include<openssl/ec.h>
#include<openssl/pem.h>
#include<openssl/x509.h>

#include<algorithm>
#include<chrono>
#include<iostream>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
namespace net = boost::asio;
using Bytes = DtlsTransport::Bytes;
using Endpoint = UdpSocket::Endpoint;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

namespace
{
template<class Predicate> void until(net::io_context& io, Predicate predicate, std::chrono::milliseconds duration = 4000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
void runFor(net::io_context& io, std::chrono::milliseconds duration) { io.restart(); io.run_for(duration); }
IceDtlsReady ready(const Endpoint& local, const Endpoint& remote, SdpSetupRole role,
                   std::vector<SdpFingerprint> fingerprints, std::uint64_t generation = 0)
{
    IceDtlsReady value;
    value.generation = generation; value.pair.id = 1; value.pair.valid = true; value.pair.nominated = true;
    value.pair.local.component = value.pair.remote.component = 1;
    value.pair.localEndpoint = local; value.pair.remoteEndpoint = remote;
    value.parameters.localSetup = role;
    value.parameters.remoteSetup = role == SdpSetupRole::Active ? SdpSetupRole::Passive : SdpSetupRole::ActPass;
    value.parameters.remoteFingerprints = std::move(fingerprints); value.parameters.transportMid = "data";
    return value;
}
IceTransportPacket packet(Bytes bytes, Endpoint source, std::uint64_t generation = 0)
{
    const auto type = classifyIcePacket(bytes);
    return {generation, 1, type, {std::move(bytes), source, UdpSocket::Clock::now()}};
}
struct Link
{
    net::io_context io;
    const Endpoint first{net::ip::make_address("127.0.0.1"), 10001}, second{net::ip::make_address("127.0.0.1"), 10002};
    std::unique_ptr<DtlsTransport> a, b;
    std::shared_ptr<const DtlsCertificate> firstCertificate = DtlsCertificate::generate(), secondCertificate = DtlsCertificate::generate();
    std::optional<DtlsSrtpKeyMaterial> firstKeys, secondKeys;
    std::vector<Bytes> receivedA, receivedB, wire;
    std::vector<Bytes> rtpAtB, rtcpAtB;
    std::vector<RtcpNackEvent> nacksAtA;
    std::vector<RtcpKeyFrameEvent> keyFramesAtA;
    std::vector<TwccFeedbackEvent> twccAtA;
    std::vector<RtcpReportEvent> reportsAtA;
    std::vector<std::chrono::steady_clock::time_point> rtpSendTimesAtA;
    std::vector<UdpSocket::SendHandler> heldMedia;
    unsigned connectedA{0}, connectedB{0}, errors{0};
    bool dropFirst{false}, corruptNextApplication{false};
    bool dropNextMedia{false}, holdMedia{false};
    bool failNextPliAtB{false};
    Bytes firstApplication;
    explicit Link(DtlsTransport::Config firstConfig = {}, DtlsTransport::Config secondConfig = {})
    {
        firstConfig.certificate = firstCertificate; secondConfig.certificate = secondCertificate;
        DtlsTransport::Callbacks firstCallbacks, secondCallbacks;
        firstCallbacks.onState = [&, paced = firstConfig.enableSrtp && firstConfig.pacer.enabled](auto state) {
            if (paced && state == DtlsState::New) CHECK(a->snapshot().pacer->generation == a->snapshot().generation);
        };
        secondCallbacks.onState = [&, paced = secondConfig.enableSrtp && secondConfig.pacer.enabled](auto state) {
            if (paced && state == DtlsState::New) CHECK(b->snapshot().pacer->generation == b->snapshot().generation);
        };
        firstCallbacks.onConnected = [&, enabled = firstConfig.enableSrtp](auto snapshot) {
            CHECK(snapshot.peerFingerprint); if (enabled && snapshot.srtpProfile) CHECK(snapshot.srtp->state == SrtpState::Ready && snapshot.srtcp->state == SrtpState::Ready); ++connectedA;
        };
        secondCallbacks.onConnected = [&, enabled = secondConfig.enableSrtp](auto snapshot) {
            CHECK(snapshot.peerFingerprint); if (enabled && snapshot.srtpProfile) CHECK(snapshot.srtp->state == SrtpState::Ready && snapshot.srtcp->state == SrtpState::Ready); ++connectedB;
        };
        firstCallbacks.onData = [&](Bytes bytes) { receivedA.push_back(std::move(bytes)); };
        firstCallbacks.onNack = [&](auto event) { nacksAtA.push_back(std::move(event)); };
        firstCallbacks.onKeyFrameRequest = [&](auto event) { keyFramesAtA.push_back(std::move(event)); };
        firstCallbacks.onTwccFeedback = [&](auto event) { twccAtA.push_back(std::move(event)); };
        firstCallbacks.onReceiverReport = [&](auto event) { reportsAtA.push_back(std::move(event)); };
        secondCallbacks.onData = [&](Bytes bytes) { receivedB.push_back(std::move(bytes)); };
        secondCallbacks.onRtpPacket = [&](auto packet) { CHECK(packet.type == IcePacketType::Rtp); rtpAtB.push_back(std::move(packet.datagram.payload)); };
        secondCallbacks.onRtcpPacket = [&](auto packet) { CHECK(packet.type == IcePacketType::Rtcp); rtcpAtB.push_back(std::move(packet.datagram.payload)); };
        firstCallbacks.onSrtpKeys = [&](auto keys) { firstKeys = std::move(keys); };
        secondCallbacks.onSrtpKeys = [&](auto keys) { secondKeys = std::move(keys); };
        firstCallbacks.onError = secondCallbacks.onError = [&](auto, auto message) { ++errors; std::cerr << "expected/diagnostic: " << message << '\n'; };
        a = std::make_unique<DtlsTransport>(io, [&, mtu = firstConfig.mtu](Bytes bytes, auto done) {
            CHECK(bytes.size() <= mtu);
            const auto size = bytes.size(); wire.push_back(bytes);
            if (classifyIcePacket(bytes) == IcePacketType::Rtp) rtpSendTimesAtA.push_back(std::chrono::steady_clock::now());
            if (classifyIcePacket(bytes) == IcePacketType::Rtp || classifyIcePacket(bytes) == IcePacketType::Rtcp)
            {
                if (holdMedia) { heldMedia.push_back(std::move(done)); return; }
                if (dropNextMedia) { dropNextMedia = false; done({}, size); return; }
            }
            if (!bytes.empty() && bytes[0] == 23)
            {
                if (firstApplication.empty()) firstApplication = bytes;
                if (corruptNextApplication) { bytes.back() ^= 0x80; corruptNextApplication = false; }
            }
            if (dropFirst) dropFirst = false;
            else b->handlePacket(packet(std::move(bytes), first, a->snapshot().generation));
            done({}, size);
        }, firstConfig, firstCallbacks);
        b = std::make_unique<DtlsTransport>(io, [&, mtu = secondConfig.mtu](Bytes bytes, auto done) {
            CHECK(bytes.size() <= mtu);
            const auto size = bytes.size(); wire.push_back(bytes);
            if (failNextPliAtB && classifyIcePacket(bytes) == IcePacketType::Rtcp && bytes[1] == 206)
            { failNextPliAtB = false; done(net::error::network_unreachable, 0); return; }
            a->handlePacket(packet(std::move(bytes), second, b->snapshot().generation)); done({}, size);
        }, secondConfig, secondCallbacks);
    }
    void start(std::vector<SdpFingerprint> expectedAtA = {}, std::vector<SdpFingerprint> expectedAtB = {}, std::uint64_t generation = 0)
    {
        if (expectedAtA.empty()) expectedAtA = {secondCertificate->fingerprint()};
        if (expectedAtB.empty()) expectedAtB = {firstCertificate->fingerprint()};
        b->start(ready(second, first, SdpSetupRole::Passive, std::move(expectedAtB), generation), [](auto ec) { CHECK(!ec); });
        a->start(ready(first, second, SdpSetupRole::Active, std::move(expectedAtA), generation), [](auto ec) { CHECK(!ec); });
    }
    void connected()
    {
        until(io, [&] { return a->snapshot().state == DtlsState::Connected && b->snapshot().state == DtlsState::Connected; });
        CHECK(a->snapshot().protocol == "DTLSv1.2" && b->snapshot().protocol == "DTLSv1.2");
        CHECK(a->snapshot().maxPlaintextSize > 0 && a->snapshot().maxPlaintextSize < 1200);
    }
    void close()
    {
        unsigned closed = 0;
        a->close([&](auto ec) { CHECK(!ec); ++closed; }); b->close([&](auto ec) { CHECK(!ec); ++closed; });
        until(io, [&] { return closed == 2; });
        CHECK(a->snapshot().state == DtlsState::Closed && b->snapshot().state == DtlsState::Closed);
        runFor(io, 5ms);
    }
};

void certificates()
{
    const auto certificate = DtlsCertificate::generate();
    CHECK(certificate->fingerprint().algorithm == "sha-256" && certificate->fingerprint().value.size() == 95);
    CHECK(certificate->fingerprint("SHA-512").value.size() == 191);
    CHECK(certificate->fingerprint("sha-1").value.size() == 59);
    const auto pem = certificate->certificatePem();
    BIO* certBio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* x509 = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    CHECK(x509);
    unsigned char value[EVP_MAX_MD_SIZE]{}; unsigned size = 0;
    CHECK(X509_digest(x509, EVP_sha256(), value, &size) == 1 && size == 32);
    std::string hex;
    static constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < size; ++i) { if (i) hex += ':'; hex += digits[value[i] >> 4]; hex += digits[value[i] & 15]; }
    CHECK(certificate->fingerprint().value == hex);
    X509_free(x509); BIO_free(certBio);
    bool rejected = false;
    try { DtlsCertificate::fromPem(pem, "invalid private key"); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);

    // 独立生成 PEM 身份，测试导入、私钥匹配以及证书摘要兼容性。
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    EVP_PKEY* key = nullptr;
    CHECK(context && EVP_PKEY_keygen_init(context) > 0 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) > 0);
    CHECK(EVP_PKEY_keygen(context, &key) > 0); EVP_PKEY_CTX_free(context);
    X509* external = X509_new(); CHECK(external);
    CHECK(X509_set_version(external, 2) == 1 && ASN1_INTEGER_set(X509_get_serialNumber(external), 1) == 1);
    CHECK(X509_gmtime_adj(X509_getm_notBefore(external), -60) && X509_gmtime_adj(X509_getm_notAfter(external), 3600));
    CHECK(X509_set_pubkey(external, key) == 1);
    auto* name = X509_get_subject_name(external);
    CHECK(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("test"), -1, -1, 0) == 1);
    CHECK(X509_set_issuer_name(external, name) == 1 && X509_sign(external, key, EVP_sha256()) > 0);
    BIO* externalCert = BIO_new(BIO_s_mem()); BIO* externalKey = BIO_new(BIO_s_mem());
    CHECK(PEM_write_bio_X509(externalCert, external) == 1 && PEM_write_bio_PrivateKey(externalKey, key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    char* certData = nullptr; char* keyData = nullptr;
    const auto certSize = BIO_get_mem_data(externalCert, &certData), keySize = BIO_get_mem_data(externalKey, &keyData);
    const auto imported = DtlsCertificate::fromPem(std::string_view(certData, certSize), std::string_view(keyData, keySize));
    CHECK(imported->certificatePem() == std::string(certData, certSize));
    rejected = false;
    try { DtlsCertificate::fromPem(pem, std::string_view(keyData, keySize)); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    BIO_free(externalCert); BIO_free(externalKey); X509_free(external); EVP_PKEY_free(key);
}

void handshakeAndData(DtlsSrtpProfile profile, bool losePacket = false)
{
    DtlsTransport::Config config; config.srtpProfiles = {profile}; config.requireSrtp = true;
    Link link(config, config); link.dropFirst = losePacket;
    bool earlySend = false;
    link.a->asyncSend({1}, [&](auto ec, auto size) { CHECK(ec == net::error::not_connected && size == 0); earlySend = true; });
    link.start(); link.connected(); CHECK(earlySend && link.errors == 0);
    CHECK(link.firstKeys && link.secondKeys && link.firstKeys->profile == profile);
    CHECK(link.firstKeys->localMasterKey == link.secondKeys->remoteMasterKey);
    CHECK(link.firstKeys->remoteMasterKey == link.secondKeys->localMasterKey);
    CHECK(link.firstKeys->localMasterSalt == link.secondKeys->remoteMasterSalt);
    CHECK(link.firstKeys->remoteMasterSalt == link.secondKeys->localMasterSalt);
    const auto saltSize = profile == DtlsSrtpProfile::Aes128CmSha1_80 || profile == DtlsSrtpProfile::Aes128CmSha1_32 ? 14u : 12u;
    CHECK(link.firstKeys->localMasterSalt.size() == saltSize);
    if (losePacket) CHECK(link.a->snapshot().stats.retransmissions > 0);
    const Bytes message{'S', 'C', 'T', 'P', 0, 255, 1, 2, 3, 4, 5, 6};
    unsigned completed = 0;
    link.a->asyncSend(message, [&](auto ec, auto size) { CHECK(!ec && size == message.size()); ++completed; });
    link.b->asyncSend({9, 8, 7}, [&](auto ec, auto size) { CHECK(!ec && size == 3); ++completed; });
    until(link.io, [&] { return completed == 2 && link.receivedA.size() == 1 && link.receivedB.size() == 1; });
    CHECK(link.receivedB[0] == message && link.receivedA[0] == Bytes({9, 8, 7}));
    CHECK(std::search(link.firstApplication.begin(), link.firstApplication.end(), message.begin(), message.end()) == link.firstApplication.end());
    // 重放已认证的旧 application record，不应重复交付。
    link.b->handlePacket(packet(link.firstApplication, link.first)); runFor(link.io, 20ms);
    CHECK(link.receivedB.size() == 1);
    link.corruptNextApplication = true;
    link.a->asyncSend({99, 98, 97}, [&](auto ec, auto) { CHECK(!ec); ++completed; });
    until(link.io, [&] { return completed == 3; }); runFor(link.io, 20ms);
    CHECK(link.receivedB.size() == 1 && link.b->snapshot().state == DtlsState::Connected);
    link.a->asyncSend({55}, [&](auto ec, auto) { CHECK(!ec); ++completed; });
    until(link.io, [&] { return completed == 4 && link.receivedB.size() == 2; });
    unsigned exported = 0; Bytes firstExport, secondExport, emptyContext, rawSrtp;
    link.a->exportKeyingMaterial("EXPORTER-uav-test", 32, std::nullopt, [&](auto ec, auto bytes) { CHECK(!ec); firstExport = bytes; ++exported; });
    link.b->exportKeyingMaterial("EXPORTER-uav-test", 32, std::nullopt, [&](auto ec, auto bytes) { CHECK(!ec); secondExport = bytes; ++exported; });
    link.a->exportKeyingMaterial("EXPORTER-uav-test", 32, Bytes{}, [&](auto ec, auto bytes) { CHECK(!ec); emptyContext = bytes; ++exported; });
    const auto keySize = link.firstKeys->localMasterKey.size();
    link.a->exportKeyingMaterial("EXTRACTOR-dtls_srtp", 2 * (keySize + saltSize), std::nullopt,
        [&](auto ec, auto bytes) { CHECK(!ec); rawSrtp = bytes; ++exported; });
    until(link.io, [&] { return exported == 4; });
    CHECK(firstExport == secondExport && firstExport != emptyContext);
    CHECK(Bytes(rawSrtp.begin(), rawSrtp.begin() + keySize) == link.firstKeys->localMasterKey);
    CHECK(Bytes(rawSrtp.begin() + keySize, rawSrtp.begin() + 2 * keySize) == link.firstKeys->remoteMasterKey);
    CHECK(Bytes(rawSrtp.begin() + 2 * keySize, rawSrtp.begin() + 2 * keySize + saltSize) == link.firstKeys->localMasterSalt);
    unsigned rejected = 0;
    link.a->asyncSend({}, [&](auto ec, auto) { CHECK(ec == net::error::invalid_argument); ++rejected; });
    link.a->asyncSend(Bytes(link.a->snapshot().maxPlaintextSize + 1, 1), [&](auto ec, auto) { CHECK(ec == net::error::message_size); ++rejected; });
    until(link.io, [&] { return rejected == 2; });
    link.close();
}

void fingerprintPolicy()
{
    Link wrong;
    const auto other = DtlsCertificate::generate();
    wrong.start({other->fingerprint()});
    until(wrong.io, [&] { return wrong.a->snapshot().state == DtlsState::Failed; });
    CHECK(wrong.connectedA == 0 && !wrong.firstKeys && wrong.receivedA.empty()); wrong.close();

    Link strongest;
    strongest.start({strongest.secondCertificate->fingerprint(), other->fingerprint("sha-512")});
    until(strongest.io, [&] { return strongest.a->snapshot().state == DtlsState::Failed; });
    CHECK(!strongest.firstKeys); strongest.close(); // 不得因 SHA512 不匹配而降级到匹配的 SHA256

    Link alternatives;
    alternatives.start({other->fingerprint(), alternatives.secondCertificate->fingerprint()});
    alternatives.connected(); alternatives.close();

    Link sha1;
    bool rejected = false;
    sha1.a->start(ready(sha1.first, sha1.second, SdpSetupRole::Active, {sha1.secondCertificate->fingerprint("sha-1")}),
        [&](auto ec) { CHECK(ec == net::error::invalid_argument); rejected = true; });
    until(sha1.io, [&] { return rejected; }); CHECK(sha1.a->snapshot().state == DtlsState::New); sha1.close();
}

void timeoutAndReset()
{
    net::io_context io;
    DtlsTransport::Config config; config.handshakeTimeout = 80ms;
    DtlsTransport silent(io, [](Bytes bytes, auto done) { done({}, bytes.size()); }, config);
    const auto remote = DtlsCertificate::generate();
    const Endpoint first(net::ip::make_address("127.0.0.1"), 10001), second(net::ip::make_address("127.0.0.1"), 10002);
    silent.start(ready(first, second, SdpSetupRole::Active, {remote->fingerprint()}));
    until(io, [&] { return silent.snapshot().state == DtlsState::Failed; });
    bool closed = false; silent.close([&](auto ec) { CHECK(!ec); closed = true; }); until(io, [&] { return closed; });

    Link link;
    link.start(); link.connected();
    const auto oldKeys = link.firstKeys->localMasterKey;
    unsigned reset = 0;
    link.a->reset(1, [&](auto ec) { CHECK(!ec); ++reset; }); link.b->reset(1, [&](auto ec) { CHECK(!ec); ++reset; });
    until(link.io, [&] { return reset == 2; });
    CHECK(link.a->snapshot().state == DtlsState::New && !link.a->snapshot().peerFingerprint);
    link.a->handlePacket(packet(link.firstApplication.empty() ? Bytes{22} : link.firstApplication, link.second, 0));
    link.start({}, {}, 1); link.connected();
    CHECK(link.connectedA == 2 && link.firstKeys->generation == 1 && link.firstKeys->localMasterKey != oldKeys);
    link.close();
}

void queuesAndOptionalSrtp()
{
    DtlsTransport::Config config; config.srtpProfiles.clear(); config.maxPendingMessages = 8;
    Link link(config, config); link.start(); link.connected(); CHECK(!link.firstKeys && !link.secondKeys);
    unsigned success = 0, overflow = 0;
    for (unsigned i = 0; i < 20; ++i) link.a->asyncSend({1, 2}, [&](auto ec, auto size) {
        if (ec) { CHECK(ec == net::error::no_buffer_space && size == 0); ++overflow; }
        else { CHECK(size == 2); ++success; }
    });
    until(link.io, [&] { return success + overflow == 20; }); CHECK(success == 8 && overflow == 12);
    link.close();

    DtlsTransport::Config required; required.requireSrtp = true;
    Link mismatch(required, config); mismatch.start();
    until(mismatch.io, [&] { return mismatch.a->snapshot().state == DtlsState::Failed; });
    CHECK(!mismatch.firstKeys && mismatch.connectedA == 0); mismatch.close();
}
void managedMedia(DtlsSrtpProfile profile)
{
    DtlsTransport::Config cfg; cfg.enableSrtp = true; cfg.srtpProfiles = {profile};
    cfg.srtp.maxStreams = 1; cfg.srtp.maxPacketSize = cfg.srtcp.maxPacketSize = 1200;
    Link link(cfg, cfg);
    const Bytes rtp{0x80, 96, 0, 7, 0, 1, 2, 3, 1, 2, 3, 4, 1, 3, 5, 7};
    const Bytes rtcp{0x80, 201, 0, 1, 1, 2, 3, 4};
    bool early = false;
    link.a->asyncSendRtp(rtp, [&](auto ec, auto n) { CHECK(ec == net::error::not_connected && n == 0); early = true; });
    link.start(); link.connected(); CHECK(early);
    CHECK(link.a->snapshot().srtp->overhead == srtpProfileInfo(profile)->rtpOverhead());
    CHECK(link.a->snapshot().srtcp->overhead == srtpProfileInfo(profile)->rtcpOverhead());
    unsigned sent = 0;
    link.a->asyncSendRtp(rtp, 0, [&](auto ec, auto n) { CHECK(!ec && n == rtp.size()); ++sent; });
    link.a->asyncSendRtcp(rtcp, 0, [&](auto ec, auto n) { CHECK(!ec && n == rtcp.size()); ++sent; });
    until(link.io, [&] { return sent == 2 && link.rtpAtB.size() == 1 && link.rtcpAtB.size() == 1; });
    CHECK(link.rtpAtB[0] == rtp && link.rtcpAtB[0] == rtcp && link.receivedB.empty());
    Bytes oldWire;
    for (const auto& bytes : link.wire) if (classifyIcePacket(bytes) == IcePacketType::Rtp) oldWire = bytes;
    CHECK(oldWire != rtp && oldWire.size() == rtp.size() + srtpProfileInfo(profile)->rtpOverhead());
    link.b->handlePacket(packet(oldWire, link.first)); runFor(link.io, 5ms);
    CHECK(link.rtpAtB.size() == 1 && link.b->snapshot().srtp->stats.replayFailures == 1);
    const auto before = link.b->snapshot().stats.discardedMediaPackets;
    link.b->handlePacket(packet(oldWire, Endpoint(net::ip::make_address("127.0.0.1"), 20000))); runFor(link.io, 5ms);
    CHECK(link.b->snapshot().stats.discardedMediaPackets == before + 1 && link.b->snapshot().srtp->stats.replayFailures == 1);
    auto next = rtp; next[3] = 8; link.dropNextMedia = true;
    link.a->asyncSendRtp(next, [&](auto ec, auto) { CHECK(!ec); ++sent; }); until(link.io, [&] { return sent == 3; });
    auto tampered = link.wire.back(); tampered.back() ^= 1;
    link.b->handlePacket(packet(tampered, link.first)); runFor(link.io, 5ms);
    CHECK(link.rtpAtB.size() == 1 && link.b->snapshot().srtp->stats.authenticationFailures == 1);
    link.b->handlePacket(packet(link.wire.back(), link.first)); runFor(link.io, 5ms);
    CHECK(link.rtpAtB.size() == 2 && link.rtpAtB.back() == next);
    unsigned rejected = 0;
    auto other = rtp; other[8] = 9;
    link.a->asyncSendRtp(other, [&](auto ec, auto n) { CHECK(ec == SrtpError::StreamLimit && n == 0); ++rejected; });
    auto huge = rtp; huge.resize(link.a->snapshot().srtp->maxPlaintextSize + 1);
    link.a->asyncSendRtp(huge, [&](auto ec, auto n) { CHECK(ec == SrtpError::PacketTooLarge && n == 0); ++rejected; });
    link.a->asyncSendRtcp(rtp, [&](auto ec, auto n) { CHECK(ec == SrtpError::InvalidPacket && n == 0); ++rejected; });
    until(link.io, [&] { return rejected == 3; });
    unsigned reset = 0;
    link.a->reset(1, [&](auto ec) { CHECK(!ec); ++reset; }); link.b->reset(1, [&](auto ec) { CHECK(!ec); ++reset; });
    until(link.io, [&] { return reset == 2; });
    CHECK(link.a->snapshot().srtp->state == SrtpState::New && link.a->snapshot().srtcp->generation == 1);
    link.b->handlePacket(packet(oldWire, link.first, 0));
    link.start({}, {}, 1); link.connected();
    link.b->handlePacket(packet(oldWire, link.first, 1)); runFor(link.io, 5ms);
    CHECK(link.rtpAtB.size() == 2 && link.b->snapshot().srtp->stats.authenticationFailures == 2);
    link.a->asyncSendRtp(rtp, 0, [&](auto ec, auto n) { CHECK(ec == SrtpError::GenerationMismatch && n == 0); ++rejected; });
    link.a->asyncSendRtp(rtp, 1, [&](auto ec, auto) { CHECK(!ec); ++sent; });
    until(link.io, [&] { return sent == 4 && rejected == 4 && link.rtpAtB.size() == 3; });
    CHECK(link.rtpAtB.size() == 3 && link.b->snapshot().srtp->stats.authenticationFailures == 2);

    link.holdMedia = true; bool cancelled = false;
    next[3] = 9; link.a->asyncSendRtp(next, [&](auto ec, auto n) { CHECK(ec == net::error::operation_aborted && n == 0); cancelled = true; });
    until(link.io, [&] { return link.heldMedia.size() == 1; });
    unsigned closed = 0; link.a->close([&](auto ec) { CHECK(!ec && cancelled); ++closed; }); link.b->close([&](auto ec) { CHECK(!ec); ++closed; });
    runFor(link.io, 5ms); CHECK(cancelled && closed == 1);
    link.heldMedia[0](net::error::operation_aborted, 0); until(link.io, [&] { return closed == 2; });
    CHECK(link.a->snapshot().srtp->state == SrtpState::Closed && link.b->snapshot().srtcp->state == SrtpState::Closed && link.errors == 0);
}
void managedFeedback(DtlsSrtpProfile profile)
{
    DtlsTransport::Config cfg; cfg.enableSrtp = true; cfg.srtpProfiles = {profile};
    cfg.rtcp.reportInterval = 100ms; cfg.rtcp.twccInterval = 10ms; cfg.rtcp.nackDelay = 30ms; cfg.rtcp.pliInterval = 30ms;
    RtcpMediaParameters media; media.payloadType = 96; media.clockRate = 90000;
    media.nack = media.pli = media.fir = media.transportCc = true;
    media.sendTwccExtension = media.receiveTwccExtension = 5; cfg.rtcp.parameters.media = {media};
    cfg.rtcp.parameters.reducedSize = true;
    Link link(cfg, cfg); link.start(); link.connected();
    Bytes p{0x80, 96, 0, 10, 0, 0, 0, 1, 0, 0, 0, 42, 1, 2, 3}; unsigned sent = 0;
    const auto send = [&] { link.a->asyncSendRtp(p, [&](auto ec, auto n) { CHECK(!ec && n == p.size()); ++sent; }); };
    send(); until(link.io, [&] { return sent == 1 && link.rtpAtB.size() == 1; });
    link.dropNextMedia = true; p[3] = 11; send(); until(link.io, [&] { return sent == 2; });
    p[3] = 12; send(); until(link.io, [&] { return sent == 3 && link.rtpAtB.size() == 3 && !link.nacksAtA.empty(); });
    CHECK(link.rtpAtB.back()[3] == 11 && rtpTransportSequence(link.rtpAtB.back(), 5) == 1);
    CHECK(link.nacksAtA[0].feedback.mediaSsrc == 42 && link.nacksAtA[0].cacheMisses.empty());
    Bytes dropped;
    unsigned copies = 0;
    for (const auto& wire : link.wire) if (classifyIcePacket(wire) == IcePacketType::Rtp && wire[3] == 11)
    { if (copies++) CHECK(wire == dropped); else dropped = wire; }
    CHECK(copies >= 2 && link.a->snapshot().srtp->stats.protectedPackets == 3);
    CHECK(link.a->snapshot().pacer->releasedPackets >= 4); // NACK repair also traverses the pacer
    until(link.io, [&] {
        const bool rr = std::any_of(link.reportsAtA.begin(), link.reportsAtA.end(), [](const auto& e) {
            return e.report.cumulativeLost == 0 && e.report.highestSequence == 12;
        });
        const bool missing = std::any_of(link.twccAtA.begin(), link.twccAtA.end(), [](const auto& e) { return e.notReceived == 1; });
        const bool late = std::any_of(link.twccAtA.begin(), link.twccAtA.end(), [](const auto& e) { return e.received == 1 && e.packets[0].rtpSequence == 11; });
        return rr && missing && late && link.a->snapshot().rtcp->sentSenderReports > 0 && link.b->snapshot().rtcp->sentReceiverReports > 0;
    });
    bool requested = false; link.b->requestPli(42, 0, [&](auto ec) { CHECK(!ec); requested = true; });
    until(link.io, [&] { return requested && link.keyFramesAtA.size() == 1; });
    CHECK(link.keyFramesAtA[0].feedback.mediaSsrc == 42);
    runFor(link.io, 35ms); link.failNextPliAtB = true;
    bool failedPli = false; link.b->requestPli(42, 0, [&](auto ec) { CHECK(ec == net::error::network_unreachable); failedPli = true; });
    until(link.io, [&] { return failedPli; }); CHECK(link.keyFramesAtA.size() == 1);
    const auto oldGenerationWire = dropped;
    unsigned reset = 0; link.a->reset(1, [&](auto ec) { CHECK(!ec); ++reset; }); link.b->reset(1, [&](auto ec) { CHECK(!ec); ++reset; });
    until(link.io, [&] { return reset == 2; });
    CHECK(link.a->snapshot().rtcp->cachedPackets == 0 && link.a->snapshot().rtcp->twccHistory == 0);
    link.start({}, {}, 1); link.connected();
    link.b->handlePacket(packet(oldGenerationWire, link.first, 0)); runFor(link.io, 30ms);
    CHECK(link.b->snapshot().rtcp->receiveStreams == 0 && link.b->snapshot().rtcp->sentNacks == 0 && link.b->snapshot().rtcp->sentTwcc == 0);
    p[3] = 10; send(); until(link.io, [&] { return sent == 4 && link.rtpAtB.size() == 4; });
    CHECK(rtpTransportSequence(link.rtpAtB.back(), 5) == 0 && link.a->snapshot().rtcp->generation == 1);
    link.close(); CHECK(link.errors == 0);
}
Bytes pacedRtp(unsigned seq, std::uint32_t ssrc = 42, unsigned pt = 96)
{
    Bytes bytes{0x80, static_cast<std::uint8_t>(pt), static_cast<std::uint8_t>(seq >> 8), static_cast<std::uint8_t>(seq),
        0, 0, 0, 1, static_cast<std::uint8_t>(ssrc >> 24), static_cast<std::uint8_t>(ssrc >> 16), static_cast<std::uint8_t>(ssrc >> 8), static_cast<std::uint8_t>(ssrc)};
    bytes.resize(176, 1); return bytes; // +8 byte TWCC extension +16 byte GCM tag =200 byte UDP payload
}
DtlsTransport::Config pacedConfig()
{
    DtlsTransport::Config cfg; cfg.enableSrtp = true; cfg.srtpProfiles = {DtlsSrtpProfile::AeadAes128Gcm};
    cfg.pacer.rateBps = 16000; cfg.pacer.processInterval = 5ms; cfg.pacer.maxQueueDelay = 2s;
    cfg.pacer.drainLargeQueues = false;
    RtcpMediaParameters m; m.payloadType = 96; m.clockRate = 90000; m.transportCc = true;
    m.sendTwccExtension = m.receiveTwccExtension = 5;
    auto audio = m; audio.payloadType = 111; audio.clockRate = 48000; audio.audio = true;
    cfg.rtcp.parameters.media = {m, audio}; cfg.rtcp.twccInterval = 10ms; return cfg;
}
void pacingAndTimestamps()
{
    auto cfg = pacedConfig(); Link link(cfg, cfg); link.start(); link.connected();
    bool paused = false; link.a->setPacingPaused(true, [&](auto ec) { CHECK(!ec); paused = true; }); until(link.io, [&] { return paused; });
    unsigned sent = 0;
    for (const auto& p : {pacedRtp(1, 10), pacedRtp(1, 11), pacedRtp(1, 20, 111)})
        link.a->asyncSendRtp(p, [&, size = p.size()](auto ec, auto n) { CHECK(!ec && n == size); ++sent; });
    until(link.io, [&] { return link.a->snapshot().pacer->queuedPackets == 3; });
    CHECK(link.a->snapshot().srtp->stats.protectedPackets == 0 && link.a->snapshot().rtcp->twccHistory == 0 && link.a->snapshot().pendingWireDatagrams == 0);
    CHECK(link.a->snapshot().pacer->queuedBytes == 600 && sent == 0);
    bool control = false; link.a->asyncSendRtcp({0x80, 201, 0, 1, 0, 0, 3, 9}, [&](auto ec, auto n) { CHECK(!ec && n == 8); control = true; });
    until(link.io, [&] { return control && !link.rtcpAtB.empty(); }); CHECK(link.rtpAtB.empty()); // RTCP bypasses media pacing
    runFor(link.io, 30ms); const auto resumeAt = std::chrono::steady_clock::now(); bool resumed = false;
    link.a->setPacingPaused(false, [&](auto ec) { CHECK(!ec); resumed = true; });
    until(link.io, [&] { return resumed && sent == 3 && link.rtpAtB.size() == 3; });
    CHECK((link.rtpAtB[0][1] & 127) == 111 && link.rtpAtB[1][11] == 10 && link.rtpAtB[2][11] == 11);
    for (unsigned i = 0; i < 3; ++i) CHECK(rtpTransportSequence(link.rtpAtB[i], 5) == i);
    CHECK(link.rtpSendTimesAtA[1] - link.rtpSendTimesAtA[0] >= 80ms && link.rtpSendTimesAtA[2] - link.rtpSendTimesAtA[1] >= 80ms);
    until(link.io, [&] { return std::any_of(link.twccAtA.begin(), link.twccAtA.end(), [](const auto& e) { return e.received > 0; }); });
    for (const auto& e : link.twccAtA) for (const auto& p : e.packets) CHECK(p.sentAt >= resumeAt);
    bool updated = false; link.a->setPacingRate(64000, [&](auto ec) { CHECK(!ec); updated = true; }); until(link.io, [&] { return updated; });
    CHECK(link.a->snapshot().pacer->rateBps == 64000);
    link.close(); CHECK(link.a->snapshot().pacer->queuedPackets == 0 && link.errors == 0);
}
void pacingExpiryAndBounds()
{
    auto cfg = pacedConfig(); cfg.pacer.rateBps = 0; cfg.pacer.maxQueueDelay = 30ms; cfg.pacer.maxQueuePackets = 1;
    Link link(cfg, cfg); link.start(); link.connected(); unsigned expired = 0, full = 0;
    const auto p = pacedRtp(7);
    link.a->asyncSendRtp(p, [&](auto ec, auto n) { CHECK(ec == net::error::timed_out && n == 0); ++expired; });
    link.a->asyncSendRtp(pacedRtp(8), [&](auto ec, auto n) { CHECK(ec == net::error::no_buffer_space && n == 0); ++full; });
    until(link.io, [&] { return expired == 1 && full == 1; });
    CHECK(link.rtpAtB.empty() && link.a->snapshot().srtp->stats.protectedPackets == 0 && link.a->snapshot().rtcp->twccHistory == 0);
    CHECK(link.a->snapshot().pacer->expiredPackets == 1 && link.a->snapshot().pendingMessages == 0);
    bool rate = false; link.a->setPacingRate(3000000, [&](auto ec) { CHECK(!ec); rate = true; }); until(link.io, [&] { return rate; });
    unsigned sent = 0; link.a->asyncSendRtp(p, [&](auto ec, auto n) { CHECK(!ec && n == p.size()); ++sent; });
    until(link.io, [&] { return sent == 1 && link.rtpAtB.size() == 1; });
    CHECK(rtpTransportSequence(link.rtpAtB[0], 5) == 0); // expired/full packets consumed neither TWCC nor SRTP sequence
    link.close();

    cfg = pacedConfig(); cfg.pacer.rateBps = 0; Link queued(cfg, cfg); queued.start(); queued.connected(); unsigned cancelled = 0;
    for (unsigned i = 0; i < 3; ++i) queued.a->asyncSendRtp(pacedRtp(i), [&](auto ec, auto n) { CHECK(ec == net::error::operation_aborted && n == 0); ++cancelled; });
    until(queued.io, [&] { return queued.a->snapshot().pacer->queuedPackets == 3; }); queued.close();
    CHECK(cancelled == 3 && queued.a->snapshot().pacer->queuedPackets == 0 && queued.a->snapshot().pendingMessages == 0);
}
void pacingPaddingBudgets()
{
    auto cfg = pacedConfig(); cfg.pacer.rateBps = 32000; cfg.pacer.paddingRateBps = 8000;
    Link link(cfg, cfg); link.start(); link.connected();
    bool paused = false; link.a->setPacingPaused(true, [&](auto ec) { CHECK(!ec); paused = true; });
    until(link.io, [&] { return paused; });
    const auto media = pacedRtp(1);
    auto padding = pacedRtp(2); padding[0] |= 0x20;
    std::fill(padding.begin() + 12, padding.end(), 0); padding.back() = 164; // padding-only RTP
    unsigned sent = 0;
    for (const auto& p : {media, padding}) link.a->asyncSendRtp(p, [&, size = p.size()](auto ec, auto n) { CHECK(!ec && n == size); ++sent; });
    until(link.io, [&] { return link.a->snapshot().pacer->queuedPackets == 2; });
    bool resumed = false; link.a->setPacingPaused(false, [&](auto ec) { CHECK(!ec); resumed = true; });
    until(link.io, [&] { return resumed && sent == 1; });
    CHECK(link.a->snapshot().pacer->paddingBudgetBytes < 0 && link.a->snapshot().pacer->queuedPackets == 1);
    until(link.io, [&] { return sent == 2 && link.rtpAtB.size() == 2; });
    CHECK(link.rtpSendTimesAtA[1] - link.rtpSendTimesAtA[0] >= 160ms);
    CHECK(rtpTransportSequence(link.rtpAtB[0], 5) == 0 && rtpTransportSequence(link.rtpAtB[1], 5) == 1);
    bool rates = false; link.a->setPacingRates(16000, 64000, [&](auto ec) { CHECK(!ec); rates = true; });
    until(link.io, [&] { return rates; }); CHECK(link.a->snapshot().pacer->paddingRateBps == 16000);
    bool invalid = false; link.a->setPacingRates(64000, 999, [&](auto ec) { CHECK(ec == net::error::invalid_argument); invalid = true; });
    until(link.io, [&] { return invalid; });
    CHECK(link.a->snapshot().pacer->rateBps == 16000 && link.a->snapshot().pacer->paddingRateBps == 16000);
    link.close(); CHECK(link.errors == 0);
}
void pacingRestartAndLateCompletion()
{
    auto cfg = pacedConfig(); cfg.pacer.rateBps = 8000;
    Link link(cfg, cfg); link.start(); link.connected(); link.holdMedia = true; unsigned cancelled = 0;
    for (unsigned i = 0; i < 3; ++i) link.a->asyncSendRtp(pacedRtp(i + 1), [&](auto ec, auto n) { CHECK(ec == net::error::operation_aborted && n == 0); ++cancelled; });
    until(link.io, [&] { return link.heldMedia.size() == 1 && link.a->snapshot().pacer->queuedPackets == 2; });
    unsigned reset = 0; link.a->reset(1, [&](auto ec) { CHECK(!ec); ++reset; }); link.b->reset(1, [&](auto ec) { CHECK(!ec); ++reset; });
    until(link.io, [&] { return reset == 2 && cancelled == 3; });
    CHECK(link.a->snapshot().pacer->generation == 1 && link.a->snapshot().pacer->queuedPackets == 0 && link.a->snapshot().rtcp->twccHistory == 0);
    link.heldMedia[0]({}, 200); until(link.io, [&] { return link.a->snapshot().pendingWireDatagrams == 0; });
    CHECK(cancelled == 3 && link.a->snapshot().rtcp->sendStreams == 0 && link.a->snapshot().pendingMessages == 0);
    link.holdMedia = false; link.start({}, {}, 1); link.connected();
    bool sent = false; link.a->asyncSendRtp(pacedRtp(1), [&](auto ec, auto n) { CHECK(!ec && n == 176); sent = true; });
    until(link.io, [&] { return sent && link.rtpAtB.size() == 1; });
    CHECK(rtpTransportSequence(link.rtpAtB[0], 5) == 0 && link.a->snapshot().pacer->releasedPackets == 1);
    link.close();
}
void mediaConfigurationAndFailure()
{
    DtlsTransport::Config bad; bad.enableSrtp = true; bad.srtp.component = 2;
    net::io_context io; bool rejected = false;
    try { DtlsTransport transport(io, [](auto bytes, auto done) { done({}, bytes.size()); }, bad); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    DtlsTransport::Config tooSmall; tooSmall.enableSrtp = true; tooSmall.srtcp.maxPacketSize = 12;
    Link failure(tooSmall, tooSmall); failure.start();
    until(failure.io, [&] { return failure.a->snapshot().state == DtlsState::Failed || failure.b->snapshot().state == DtlsState::Failed; });
    const auto snap = failure.a->snapshot().state == DtlsState::Failed ? failure.a->snapshot() : failure.b->snapshot();
    CHECK(snap.srtp->state == SrtpState::Failed && snap.srtcp->state == SrtpState::Failed && !snap.peerFingerprint); failure.close();

    DtlsTransport::Config optional; optional.enableSrtp = true; optional.srtpProfiles.clear();
    Link noProfile(optional, optional); noProfile.start(); noProfile.connected();
    CHECK(noProfile.a->snapshot().srtp->state == SrtpState::New);
    bool notReady = false; noProfile.a->asyncSendRtp({0x80, 96}, [&](auto ec, auto n) { CHECK(ec == SrtpError::NotReady && n == 0); notReady = true; });
    until(noProfile.io, [&] { return notReady; }); noProfile.close();
}
void mediaQueueLimits()
{
    for (bool byteLimit : {false, true})
    {
        DtlsTransport::Config cfg; cfg.enableSrtp = true;
        if (byteLimit) cfg.maxPendingBytes = 1200; else cfg.maxPendingMessages = 8;
        Link link(cfg, cfg); link.start(); link.connected(); link.holdMedia = true;
        const unsigned accepted = byteLimit ? 1 : 8, attempts = byteLimit ? 2 : 12;
        unsigned cancelled = 0, full = 0;
        for (unsigned i = 0; i < attempts; ++i)
        {
            Bytes rtp{0x80, 96, 0, static_cast<std::uint8_t>(i), 0, 1, 2, 3, 1, 2, 3, 4, 1, 3, 5, 7};
            if (byteLimit) rtp.resize(1000);
            link.a->asyncSendRtp(rtp, [&](auto ec, auto n) {
                CHECK(n == 0);
                if (ec == net::error::no_buffer_space) ++full;
                else { CHECK(ec == net::error::operation_aborted); ++cancelled; }
            });
        }
        until(link.io, [&] { return full == attempts - accepted && link.heldMedia.size() == accepted; });
        CHECK(link.a->snapshot().pendingMessages == accepted);
        unsigned closed = 0;
        link.a->close([&](auto ec) { CHECK(!ec && cancelled == accepted); ++closed; });
        link.b->close([&](auto ec) { CHECK(!ec); ++closed; }); runFor(link.io, 5ms);
        CHECK(cancelled == accepted && closed == 1);
        for (auto& done : link.heldMedia) done(net::error::operation_aborted, 0);
        until(link.io, [&] { return closed == 2; }); CHECK(link.a->snapshot().pendingMessages == 0);
    }
}

SdpSession description(const IceTransport::Snapshot& ice, const SdpFingerprint& fingerprint, SdpDescriptionType type)
{
    SdpSession session; session.connection = SdpConnection{};
    session.common.iceUfrag = ice.ice.localCredentials.ufrag; session.common.icePwd = ice.ice.localCredentials.password;
    session.common.fingerprints = {fingerprint}; session.common.setup = type == SdpDescriptionType::Offer ? SdpSetupRole::ActPass : SdpSetupRole::Active;
    SdpMedia media; media.kind = "application"; media.protocol = "UDP/DTLS/SCTP"; media.formats = {"webrtc-datachannel"};
    media.mid = "data"; media.sctpPort = 5000; media.candidates = ice.ice.localCandidates; media.endOfCandidates = true;
    session.media = {media}; CHECK(session.validateWebRtc(type)); return session;
}
void realIceIntegration()
{
    net::io_context io;
    std::shared_ptr<DtlsTransport> firstDtls, secondDtls;
    std::shared_ptr<IceTransport> firstIce, secondIce;
    IceTransport::Callbacks firstCallbacks, secondCallbacks;
    firstCallbacks.onDtlsReady = [&](auto ready) { firstDtls->start(std::move(ready)); };
    secondCallbacks.onDtlsReady = [&](auto ready) { secondDtls->start(std::move(ready)); };
    firstCallbacks.onPacket = [&](auto packet) { if (packet.type == IcePacketType::Dtls) firstDtls->handlePacket(std::move(packet)); };
    secondCallbacks.onPacket = [&](auto packet) { if (packet.type == IcePacketType::Dtls) secondDtls->handlePacket(std::move(packet)); };
    firstCallbacks.onState = [&](auto) { if (firstDtls) firstDtls->handleIceState(firstIce->snapshot()); };
    secondCallbacks.onState = [&](auto) { if (secondDtls) secondDtls->handleIceState(secondIce->snapshot()); };
    IceTransport::Config firstConfig, secondConfig;
    firstConfig.ice.bindings = secondConfig.ice.bindings = {{{net::ip::make_address("127.0.0.1"), 0}, 1, 65535}};
    firstConfig.ice.checkInterval = secondConfig.ice.checkInterval = 10ms;
    secondConfig.ice.role = StunIceRole::Controlled;
    firstIce = std::make_shared<IceTransport>(io, firstConfig, firstCallbacks);
    secondIce = std::make_shared<IceTransport>(io, secondConfig, secondCallbacks);
    unsigned received = 0;
    DtlsTransport::Callbacks secondDtlsCallbacks;
    secondDtlsCallbacks.onData = [&](auto bytes) { CHECK(bytes == Bytes({1, 2, 3, 4})); ++received; };
    firstDtls = std::make_shared<DtlsTransport>(io, [weak = std::weak_ptr<IceTransport>(firstIce)](Bytes bytes, auto done) {
        if (auto ice = weak.lock()) ice->asyncSendDtls(std::move(bytes), 1, std::move(done)); else done(net::error::operation_aborted, 0);
    });
    secondDtls = std::make_shared<DtlsTransport>(io, [weak = std::weak_ptr<IceTransport>(secondIce)](Bytes bytes, auto done) {
        if (auto ice = weak.lock()) ice->asyncSendDtls(std::move(bytes), 1, std::move(done)); else done(net::error::operation_aborted, 0);
    }, DtlsTransport::Config{}, secondDtlsCallbacks);
    firstIce->gather(); secondIce->gather();
    until(io, [&] { return !firstIce->snapshot().ice.localCandidates.empty() && !secondIce->snapshot().ice.localCandidates.empty(); });
    firstIce->setRemoteDescription(description(secondIce->snapshot(), secondDtls->snapshot().localFingerprint, SdpDescriptionType::Answer), "data", SdpDescriptionType::Answer);
    secondIce->setRemoteDescription(description(firstIce->snapshot(), firstDtls->snapshot().localFingerprint, SdpDescriptionType::Offer), "data", SdpDescriptionType::Offer);
    firstIce->start(); secondIce->start();
    until(io, [&] { return firstDtls->snapshot().state == DtlsState::Connected && secondDtls->snapshot().state == DtlsState::Connected; });
    CHECK(firstDtls->snapshot().localSetup == SdpSetupRole::Passive && secondDtls->snapshot().localSetup == SdpSetupRole::Active);
    bool sent = false;
    firstDtls->asyncSend({1, 2, 3, 4}, [&](auto ec, auto size) { CHECK(!ec && size == 4); sent = true; });
    until(io, [&] { return sent && received == 1; });
    // ICE restart 的状态通知使旧 SSL 失效，再使用原证书建立新 association。
    unsigned restarted = 0;
    firstIce->restart({}, StunIceRole::Controlling, [&](auto ec) { CHECK(!ec); ++restarted; });
    secondIce->restart({}, StunIceRole::Controlled, [&](auto ec) { CHECK(!ec); ++restarted; });
    until(io, [&] { return restarted == 2 && firstDtls->snapshot().generation == 1 && secondDtls->snapshot().generation == 1; });
    CHECK(firstDtls->snapshot().state == DtlsState::New);
    firstIce->setRemoteDescription(description(secondIce->snapshot(), secondDtls->snapshot().localFingerprint, SdpDescriptionType::Answer), "data", SdpDescriptionType::Answer);
    secondIce->setRemoteDescription(description(firstIce->snapshot(), firstDtls->snapshot().localFingerprint, SdpDescriptionType::Offer), "data", SdpDescriptionType::Offer);
    firstIce->start(); secondIce->start();
    until(io, [&] { return firstDtls->snapshot().state == DtlsState::Connected && secondDtls->snapshot().state == DtlsState::Connected; });
    sent = false;
    firstDtls->asyncSend({1, 2, 3, 4}, [&](auto ec, auto size) { CHECK(!ec && size == 4); sent = true; });
    until(io, [&] { return sent && received == 2; });
    unsigned closed = 0;
    firstDtls->close([&](auto ec) { CHECK(!ec); ++closed; }); secondDtls->close([&](auto ec) { CHECK(!ec); ++closed; });
    until(io, [&] { return closed == 2; });
    firstIce->close([&](auto ec) { CHECK(!ec); ++closed; }); secondIce->close([&](auto ec) { CHECK(!ec); ++closed; });
    until(io, [&] { return closed == 4; }); runFor(io, 5ms);
}
} // namespace
int main()
{
    try
    {
        certificates(); handshakeAndData(DtlsSrtpProfile::AeadAes128Gcm, true);
        handshakeAndData(DtlsSrtpProfile::Aes128CmSha1_80);
        handshakeAndData(DtlsSrtpProfile::AeadAes256Gcm);
        handshakeAndData(DtlsSrtpProfile::Aes128CmSha1_32);
        fingerprintPolicy(); timeoutAndReset(); queuesAndOptionalSrtp(); realIceIntegration();
        for (auto profile : {DtlsSrtpProfile::AeadAes128Gcm, DtlsSrtpProfile::AeadAes256Gcm, DtlsSrtpProfile::Aes128CmSha1_80, DtlsSrtpProfile::Aes128CmSha1_32}) managedMedia(profile);
        managedFeedback(DtlsSrtpProfile::AeadAes128Gcm); managedFeedback(DtlsSrtpProfile::Aes128CmSha1_80);
        pacingAndTimestamps(); pacingExpiryAndBounds(); pacingPaddingBudgets(); pacingRestartAndLateCompletion();
        mediaConfigurationAndFailure(); mediaQueueLimits();
        std::cout << "PASS: DTLS certificates, fingerprints, handshakes, retransmission, data, SRTP keys and real ICE\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
