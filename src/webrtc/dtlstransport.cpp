#include"webrtc/dtlstransport.h"
#include"log/log.h"
#include"common/rtcp_packet.h"
#include"common/rtp_packet.h"

#include<boost/asio/post.hpp>
#include<boost/asio/steady_timer.hpp>
#include<boost/asio/strand.hpp>
#include<openssl/bn.h>
#include<openssl/crypto.h>
#include<openssl/ec.h>
#include<openssl/err.h>
#include<openssl/pem.h>
#include<openssl/rand.h>
#include<openssl/ssl.h>
#include<openssl/srtp.h>
#include<openssl/x509.h>

#include<algorithm>
#include<array>
#include<climits>
#include<cstring>
#include<deque>
#include<mutex>
#include<map>
#include<set>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace net = boost::asio;
namespace
{
const char* stateName(DtlsState state)
{
    switch (state)
    {
    case DtlsState::New: return "New"; case DtlsState::Connecting: return "Connecting"; case DtlsState::Connected: return "Connected";
    case DtlsState::Failed: return "Failed"; case DtlsState::Closing: return "Closing"; case DtlsState::Closed: return "Closed";
    }
    return "Unknown";
}
using Bytes = DtlsTransport::Bytes;
using ErrorCode = DtlsTransport::ErrorCode;
using Clock = std::chrono::steady_clock;
template<class T, void(*Free)(T*)> using Handle = std::unique_ptr<T, decltype(Free)>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void complete(const DtlsTransport::Completion& done, ErrorCode ec = {}) { if (done) done(ec); }
std::string lower(std::string_view value)
{
    std::string text(value);
    for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return text;
}
const EVP_MD* hash(std::string_view algorithm)
{
    const auto name = lower(algorithm);
    if (name == "sha-512") return EVP_sha512();
    if (name == "sha-384") return EVP_sha384();
    if (name == "sha-256") return EVP_sha256();
    if (name == "sha-224") return EVP_sha224();
    if (name == "sha-1") return EVP_sha1();
    return nullptr;
}
Bytes digest(const Bytes& data, const EVP_MD* md)
{
    Bytes result(EVP_MAX_MD_SIZE);
    unsigned size = 0;
    require(EVP_Digest(data.data(), data.size(), result.data(), &size, md, nullptr) == 1, "DTLS digest failed");
    result.resize(size); return result;
}
std::string hex(const Bytes& bytes)
{
    static constexpr char chars[] = "0123456789ABCDEF";
    std::string text;
    for (auto byte : bytes)
    { if (!text.empty()) text += ':'; text += chars[byte >> 4]; text += chars[byte & 15]; }
    return text;
}
std::optional<Bytes> unhex(std::string_view text, std::size_t size)
{
    if (size == 0 || text.size() != size * 3 - 1) return std::nullopt;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    Bytes bytes;
    for (std::size_t i = 0; i < size; ++i)
    {
        const int a = nibble(text[i * 3]), b = nibble(text[i * 3 + 1]);
        if (a < 0 || b < 0 || (i + 1 < size && text[i * 3 + 2] != ':')) return std::nullopt;
        bytes.push_back(static_cast<std::uint8_t>(a * 16 + b));
    }
    return bytes;
}

// 非阻塞 datagram BIO：每次 read/write 都保留一个 UDP 报文，不能用 stream memory BIO 代替。
struct DatagramBio
{
    std::deque<Bytes> packets;
    std::size_t bytes{0}, maxBytes{0}, maxPackets{0}, mtu{1200};
    bool fatal{false};
};
int bioCreate(BIO* bio)
{
    try { BIO_set_data(bio, new DatagramBio); BIO_set_init(bio, 1); BIO_set_shutdown(bio, 1); return 1; }
    catch (...) { return 0; }
}
int bioDestroy(BIO* bio)
{
    if (bio) { delete static_cast<DatagramBio*>(BIO_get_data(bio)); BIO_set_data(bio, nullptr); BIO_set_init(bio, 0); }
    return 1;
}
int bioRead(BIO* bio, char* output, int size)
{
    BIO_clear_retry_flags(bio);
    auto* state = static_cast<DatagramBio*>(BIO_get_data(bio));
    if (!output || size <= 0) return 0;
    if (state->packets.empty()) { BIO_set_retry_read(bio); return -1; }
    const auto& packet = state->packets.front();
    const auto count = std::min(packet.size(), static_cast<std::size_t>(size));
    std::memcpy(output, packet.data(), count);
    state->bytes -= packet.size(); state->packets.pop_front();
    return static_cast<int>(count); // 与 UDP 一样，缓冲区不足时丢弃本报文余下部分
}
int bioWrite(BIO* bio, const char* input, int size)
{
    BIO_clear_retry_flags(bio);
    auto* state = static_cast<DatagramBio*>(BIO_get_data(bio));
    if (!input || size <= 0) return 0;
    if (static_cast<std::size_t>(size) > state->mtu) { state->fatal = true; return -1; }
    if (state->packets.size() >= state->maxPackets || static_cast<std::size_t>(size) > state->maxBytes - state->bytes)
    { BIO_set_retry_write(bio); return -1; }
    try
    {
        state->packets.emplace_back(reinterpret_cast<const std::uint8_t*>(input), reinterpret_cast<const std::uint8_t*>(input) + size);
        state->bytes += size; return size;
    }
    catch (...) { state->fatal = true; return -1; }
}
long bioCtrl(BIO* bio, int command, long value, void*)
{
    auto* state = static_cast<DatagramBio*>(BIO_get_data(bio));
    switch (command)
    {
    case BIO_CTRL_FLUSH: case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT: case BIO_CTRL_DGRAM_SET_CONNECTED: return 1;
    case BIO_CTRL_PENDING: return state->packets.empty() ? 0 : static_cast<long>(state->packets.front().size());
    case BIO_CTRL_WPENDING: return static_cast<long>(state->bytes);
    case BIO_CTRL_DGRAM_QUERY_MTU: case BIO_CTRL_DGRAM_GET_MTU: case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
        return static_cast<long>(state->mtu);
    case BIO_CTRL_DGRAM_SET_MTU: if (value > 0) state->mtu = static_cast<std::size_t>(value); return value;
    case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD: return 0; // mtu 为 UDP payload 预算，不再扣 IP/UDP 头
    case BIO_CTRL_GET_CLOSE: return BIO_get_shutdown(bio);
    case BIO_CTRL_SET_CLOSE: BIO_set_shutdown(bio, static_cast<int>(value)); return 1;
    default: return 0;
    }
}
const BIO_METHOD* datagramMethod()
{
    static const Handle<BIO_METHOD, BIO_meth_free> method([] {
        auto* result = BIO_meth_new(BIO_TYPE_DGRAM, "ICE DTLS datagram queue");
        if (!result) throw std::runtime_error("cannot create DTLS BIO method");
        if (!BIO_meth_set_create(result, bioCreate) || !BIO_meth_set_destroy(result, bioDestroy) ||
            !BIO_meth_set_read(result, bioRead) || !BIO_meth_set_write(result, bioWrite) || !BIO_meth_set_ctrl(result, bioCtrl))
        { BIO_meth_free(result); throw std::runtime_error("cannot configure DTLS BIO method"); }
        return result;
    }(), BIO_meth_free);
    return method.get();
}
std::pair<std::size_t, std::size_t> srtpSizes(DtlsSrtpProfile profile)
{
    switch (profile)
    {
    case DtlsSrtpProfile::AeadAes128Gcm: return {16, 12};
    case DtlsSrtpProfile::AeadAes256Gcm: return {32, 12};
    case DtlsSrtpProfile::Aes128CmSha1_80: case DtlsSrtpProfile::Aes128CmSha1_32: return {16, 14};
    }
    throw std::invalid_argument("unknown DTLS SRTP profile");
}
} // namespace

struct DtlsCertificate::Impl
{
    Handle<X509, X509_free> cert{nullptr, X509_free};
    Handle<EVP_PKEY, EVP_PKEY_free> key{nullptr, EVP_PKEY_free};
    Bytes der;
    std::string pem;
    void encode()
    {
        const auto size = i2d_X509(cert.get(), nullptr);
        require(size > 0, "cannot encode DTLS certificate");
        der.resize(size); auto* output = der.data();
        require(i2d_X509(cert.get(), &output) == size, "cannot encode DTLS certificate");
        Handle<BIO, BIO_free_all> bio(BIO_new(BIO_s_mem()), BIO_free_all);
        require(bio && PEM_write_bio_X509(bio.get(), cert.get()) == 1, "cannot export DTLS certificate");
        char* data = nullptr; const auto length = BIO_get_mem_data(bio.get(), &data);
        pem.assign(data, static_cast<std::size_t>(length));
    }
};
DtlsCertificate::DtlsCertificate(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
std::shared_ptr<const DtlsCertificate> DtlsCertificate::generate(unsigned days)
{
    if (days == 0 || days > 365) throw std::invalid_argument("invalid DTLS certificate lifetime");
    auto identity = std::make_shared<Impl>();
    Handle<EVP_PKEY_CTX, EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* key = nullptr;
    require(context && EVP_PKEY_keygen_init(context.get()) > 0 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) > 0 &&
        EVP_PKEY_keygen(context.get(), &key) > 0, "cannot generate DTLS key");
    identity->key.reset(key); identity->cert.reset(X509_new());
    require(bool(identity->cert), "cannot allocate DTLS certificate");
    std::array<unsigned char, 16> serial{};
    require(RAND_bytes(serial.data(), static_cast<int>(serial.size())) == 1, "cannot generate DTLS serial");
    serial[0] &= 0x7f; serial[0] |= 1;
    Handle<BIGNUM, BN_free> number(BN_bin2bn(serial.data(), static_cast<int>(serial.size()), nullptr), BN_free);
    require(number && BN_to_ASN1_INTEGER(number.get(), X509_get_serialNumber(identity->cert.get())), "cannot set DTLS serial");
    require(X509_set_version(identity->cert.get(), 2) == 1 &&
        X509_gmtime_adj(X509_getm_notBefore(identity->cert.get()), -300) &&
        X509_gmtime_adj(X509_getm_notAfter(identity->cert.get()), static_cast<long>(days) * 86400) &&
        X509_set_pubkey(identity->cert.get(), identity->key.get()) == 1, "cannot configure DTLS certificate");
    auto* name = X509_get_subject_name(identity->cert.get());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("UAV WebRTC"), -1, -1, 0) == 1 &&
        X509_set_issuer_name(identity->cert.get(), name) == 1 &&
        X509_sign(identity->cert.get(), identity->key.get(), EVP_sha256()) > 0, "cannot sign DTLS certificate");
    identity->encode();
    return std::shared_ptr<const DtlsCertificate>(new DtlsCertificate(std::move(identity)));
}
std::shared_ptr<const DtlsCertificate> DtlsCertificate::fromPem(std::string_view certificate, std::string_view privateKey)
{
    if (certificate.empty() || privateKey.empty() || certificate.size() > 1024 * 1024 || privateKey.size() > 1024 * 1024)
        throw std::invalid_argument("invalid DTLS PEM size");
    Handle<BIO, BIO_free_all> certBio(BIO_new_mem_buf(certificate.data(), static_cast<int>(certificate.size())), BIO_free_all);
    Handle<BIO, BIO_free_all> keyBio(BIO_new_mem_buf(privateKey.data(), static_cast<int>(privateKey.size())), BIO_free_all);
    auto identity = std::make_shared<Impl>();
    // 不启动 OpenSSL 的默认交互式密码提示；加密私钥由调用方解密后传入。
    auto noPassword = [](char*, int, int, void*) -> int { return 0; };
    identity->cert.reset(certBio ? PEM_read_bio_X509(certBio.get(), nullptr, noPassword, nullptr) : nullptr);
    identity->key.reset(keyBio ? PEM_read_bio_PrivateKey(keyBio.get(), nullptr, noPassword, nullptr) : nullptr);
    if (!identity->cert || !identity->key || X509_check_private_key(identity->cert.get(), identity->key.get()) != 1 ||
        (EVP_PKEY_base_id(identity->key.get()) != EVP_PKEY_EC && EVP_PKEY_base_id(identity->key.get()) != EVP_PKEY_RSA) ||
        EVP_PKEY_bits(identity->key.get()) < (EVP_PKEY_base_id(identity->key.get()) == EVP_PKEY_RSA ? 2048 : 256))
    { ERR_clear_error(); throw std::invalid_argument("invalid, mismatched or unsupported DTLS certificate/private key"); }
    identity->encode(); return std::shared_ptr<const DtlsCertificate>(new DtlsCertificate(std::move(identity)));
}
SdpFingerprint DtlsCertificate::fingerprint(std::string_view algorithm) const
{
    const auto* md = hash(algorithm);
    if (!md) throw std::invalid_argument("unsupported DTLS fingerprint algorithm");
    return {lower(algorithm), hex(digest(impl_->der, md))};
}
std::string DtlsCertificate::certificatePem() const { return impl_->pem; }
const char* srtpProfileString(DtlsSrtpProfile profile)
{
    switch (profile)
    {
    case DtlsSrtpProfile::AeadAes128Gcm: return "SRTP_AEAD_AES_128_GCM";
    case DtlsSrtpProfile::AeadAes256Gcm: return "SRTP_AEAD_AES_256_GCM";
    case DtlsSrtpProfile::Aes128CmSha1_80: return "SRTP_AES128_CM_SHA1_80";
    case DtlsSrtpProfile::Aes128CmSha1_32: return "SRTP_AES128_CM_SHA1_32";
    }
    return nullptr;
}

struct DtlsTransport::Impl : std::enable_shared_from_this<DtlsTransport::Impl>
{
    struct Write
    {
        Bytes payload;
        SendHandler done;
        std::size_t pending{0};
        bool encoded{false}, finished{false};
    };
    struct MediaWrite
    {
        SendHandler done;
        std::size_t plaintextSize{0}, ciphertextSize{0};
        bool finished{false};
        Bytes trackedPlaintext, trackedCiphertext;
        Clock::time_point sentAt;
        Bytes payload; // plaintext until paced release; retransmissions are already ciphertext
        std::uint64_t id{0}, version{0}, generation{0};
        std::size_t reservationSize{0};
        bool control{false}, retransmission{false}, submitted{false};
    };
    net::strand<net::io_context::executor_type> strand;
    net::steady_timer timer;
    net::steady_timer rtcpTimer;
    net::steady_timer pacerTimer;
    Sender sender;
    Config config;
    Callbacks callbacks;
    std::unique_ptr<SrtpCtl> srtp;
    std::unique_ptr<SrtcpCtl> srtcp;
    std::unique_ptr<RtcpSession> rtcp;
    std::unique_ptr<Pacer> pacer;
    std::map<std::uint64_t, std::shared_ptr<MediaWrite>> pacedWrites;
    std::uint64_t nextMediaId{0}, pacerTimerVersion{0};
    bool rtcpArmed{false};
    std::uint64_t rtcpSendErrors{0}, rtcpTimerVersion{0};
    Handle<SSL_CTX, SSL_CTX_free> context{nullptr, SSL_CTX_free};
    Handle<SSL, SSL_free> ssl{nullptr, SSL_free};
    DatagramBio* input{nullptr};
    DatagramBio* output{nullptr};
    Snapshot current;
    mutable std::mutex snapshotMutex;
    Snapshot published;
    std::optional<IceDtlsReady> ready;
    const EVP_MD* preferredHash{nullptr};
    std::string preferredAlgorithm;
    std::vector<Bytes> expected;
    std::deque<IceTransportPacket> early;
    std::size_t earlyBytes{0}, pendingBytes{0}, wirePending{0};
    std::deque<std::shared_ptr<Write>> writes;
    std::set<std::shared_ptr<MediaWrite>> mediaWrites;
    std::size_t mediaBytes{0};
    std::vector<Completion> closeCallbacks;
    std::uint64_t session{0}, timerVersion{0};
    bool verified{false}, closing{false};
    Clock::time_point deadline;

    Impl(net::io_context& io, Sender send, Config options, Callbacks handlers)
        : strand(net::make_strand(io)), timer(strand), rtcpTimer(strand), pacerTimer(strand), sender(std::move(send)), config(std::move(options)), callbacks(std::move(handlers))
    {
        if (!sender || config.component == 0 || config.component > 256 || config.mtu < 576 || config.mtu > 65507 ||
            config.maxPlaintextSize == 0 || config.maxPlaintextSize > 16384 ||
            config.maxPendingBytes < config.mtu || config.maxPendingBytes > 64 * 1024 * 1024 ||
            config.maxPendingMessages == 0 || config.maxPendingMessages > 4096 ||
            config.maxBufferedDatagrams == 0 || config.maxBufferedDatagrams > 4096 ||
            config.maxBufferedBytes < config.mtu || config.maxBufferedBytes > 64 * 1024 * 1024 ||
            config.handshakeTimeout < std::chrono::milliseconds(20) || config.handshakeTimeout > std::chrono::minutes(5) ||
            (config.requireSrtp && config.srtpProfiles.empty())) throw std::invalid_argument("invalid DTLS transport configuration");
        if (config.enableSrtp)
        {
            if (config.srtp.component != config.component || config.srtcp.component != config.component || !config.srtp.rtcpMux || !config.srtcp.rtcpMux)
                throw std::invalid_argument("DTLS media controllers require the same component and RTCP mux");
            srtp = std::make_unique<SrtpCtl>(config.srtp); srtcp = std::make_unique<SrtcpCtl>(config.srtcp);
            if (config.pacer.enabled) pacer = std::make_unique<Pacer>(config.pacer);
            if (config.rtcp.enabled)
            {
                config.rtcp.maxDatagramSize = std::min(config.rtcp.maxDatagramSize, config.mtu - 20);
                rtcp = std::make_unique<RtcpSession>(config.rtcp, RtcpSession::Callbacks{
                    callbacks.onReceiverReport, callbacks.onNack, callbacks.onKeyFrameRequest, callbacks.onTwccFeedback});
            }
        }
        if (!config.certificate) config.certificate = DtlsCertificate::generate();
        current.component = config.component; current.localFingerprint = config.certificate->fingerprint();
        context.reset(SSL_CTX_new(DTLS_method()));
        require(bool(context), "cannot create DTLS context");
        require(SSL_CTX_set_min_proto_version(context.get(), DTLS1_2_VERSION) == 1 &&
            SSL_CTX_set_max_proto_version(context.get(), DTLS1_2_VERSION) == 1 &&
            SSL_CTX_use_certificate(context.get(), config.certificate->impl_->cert.get()) == 1 &&
            SSL_CTX_use_PrivateKey(context.get(), config.certificate->impl_->key.get()) == 1 &&
            SSL_CTX_check_private_key(context.get()) == 1, "cannot configure DTLS identity");
        require(SSL_CTX_set_cipher_list(context.get(), "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384") == 1, "cannot configure DTLS ciphers");
        SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET | SSL_OP_NO_QUERY_MTU);
        SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, verifyCertificate);
        SSL_CTX_set_read_ahead(context.get(), 1);
        std::string profiles;
        std::set<DtlsSrtpProfile> seen;
        for (auto profile : config.srtpProfiles)
        {
            const auto* name = srtpProfileString(profile);
            if (!name || !seen.insert(profile).second) throw std::invalid_argument("invalid DTLS SRTP profiles");
            if (!profiles.empty()) profiles += ':';
            profiles += name;
        }
        if (!profiles.empty()) require(SSL_CTX_set_tlsext_use_srtp(context.get(), profiles.c_str()) == 0, "cannot configure DTLS SRTP profiles");
        publish();
    }
    void publish()
    {
        current.pendingMessages = writes.size() + mediaWrites.size(); current.pendingWireDatagrams = wirePending;
        if (srtp) current.srtp = srtp->snapshot();
        if (srtcp) current.srtcp = srtcp->snapshot();
        if (rtcp) current.rtcp = rtcp->snapshot();
        if (pacer) current.pacer = pacer->snapshot(Clock::now());
        std::lock_guard<std::mutex> lock(snapshotMutex); published = current;
    }
    void state(DtlsState value)
    {
        if (current.state == value) { publish(); return; }
        const auto previous = current.state; current.state = value; publish();
        UAV_LOG_INFO << "[WebRTC][DTLS " << this << "][g=" << current.generation << "][c=" << config.component << "] state "
            << stateName(previous) << " -> " << stateName(value);
        if (callbacks.onState) callbacks.onState(value);
    }
    void finishWrite(std::shared_ptr<Write> write, ErrorCode ec)
    {
        if (write->finished) return;
        write->finished = true;
        pendingBytes -= write->payload.size();
        if (!writes.empty() && writes.front() == write) writes.pop_front();
        if (!ec) { ++current.stats.sentMessages; current.stats.sentBytes += write->payload.size(); }
        publish(); if (write->done) write->done(ec, ec ? 0 : write->payload.size());
    }
    void cancelWrites(ErrorCode ec)
    {
        ++pacerTimerVersion; pacerTimer.cancel();
        if (pacer) for (const auto& packet : pacer->clear(Clock::now()))
        {
            const auto found = pacedWrites.find(packet.id);
            if (found != pacedWrites.end())
            {
                auto write = found->second; pacedWrites.erase(found);
                mediaBytes -= write->reservationSize; mediaWrites.erase(write); finishMediaWrite(write, ec);
            }
        }
        while (!writes.empty()) finishWrite(writes.front(), ec);
        for (const auto& write : mediaWrites) finishMediaWrite(write, ec);
        publish();
    }
    void finishMediaWrite(const std::shared_ptr<MediaWrite>& write, ErrorCode ec)
    {
        if (write->finished) return;
        write->finished = true;
        auto done = std::move(write->done);
        if (done) done(ec, ec ? 0 : write->plaintextSize);
    }
    void clearSsl()
    {
        ++pacerTimerVersion; pacerTimer.cancel();
        ++rtcpTimerVersion; rtcpTimer.cancel(); rtcpArmed = false;
        if (rtcp) rtcp->invalidate();
        ++session; ++timerVersion; timer.cancel(); ssl.reset(); input = nullptr; output = nullptr;
        ready.reset(); expected.clear(); preferredHash = nullptr; verified = false;
        early.clear(); earlyBytes = 0;
        current.peerFingerprint.reset(); current.srtpProfile.reset(); current.localSetup.reset();
        current.cipher.clear(); current.protocol.clear(); current.maxPlaintextSize = 0;
    }
    void fail(ErrorCode ec, const char* message)
    {
        if (closing || current.state == DtlsState::Failed || current.state == DtlsState::Closed) return;
        UAV_LOG_ERROR << "[WebRTC][DTLS " << this << "][g=" << current.generation << "][c=" << config.component << "] failed: "
            << message << " error=" << ec.value() << ":" << ec.message();
        if (srtp) { srtp->invalidate(); srtcp->invalidate(); }
        clearSsl(); state(DtlsState::Failed); cancelWrites(ec);
        if (callbacks.onError) callbacks.onError(ec, message);
        ERR_clear_error();
    }
    template<class Function> void guarded(Function function)
    {
        try { function(); }
        catch (const std::exception&) { fail(net::error::fault, "DTLS operation failed"); }
    }
    bool certificateMatches(X509* certificate)
    {
        if (!certificate || !preferredHash || expected.empty()) return false;
        unsigned size = 0; std::array<unsigned char, EVP_MAX_MD_SIZE> actual{};
        if (X509_digest(certificate, preferredHash, actual.data(), &size) != 1) return false;
        bool match = false;
        for (const auto& value : expected) if (value.size() == size && CRYPTO_memcmp(value.data(), actual.data(), size) == 0) match = true;
        return match;
    }
    static int verifyCertificate(int, X509_STORE_CTX* store)
    {
        try
        {
            auto* connection = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
            auto* self = connection ? static_cast<Impl*>(SSL_get_app_data(connection)) : nullptr;
            if (!self) return 0;
            if (X509_STORE_CTX_get_error_depth(store) != 0) return 1; // 身份依据 SDP，不要求公共 CA 链
            self->verified = self->certificateMatches(X509_STORE_CTX_get_current_cert(store));
            if (!self->verified) X509_STORE_CTX_set_error(store, X509_V_ERR_APPLICATION_VERIFICATION);
            return self->verified ? 1 : 0;
        }
        catch (...) { return 0; }
    }
    bool selectFingerprints(const std::vector<SdpFingerprint>& fingerprints)
    {
        if (fingerprints.empty() || fingerprints.size() > 64) return false;
        std::vector<Bytes> selected;
        const EVP_MD* selectedHash = nullptr;
        std::string algorithm;
        int best = 0;
        for (const auto& fingerprint : fingerprints)
        {
            const auto name = lower(fingerprint.algorithm);
            const auto* md = hash(name);
            if (!md || (name == "sha-1" && !config.allowSha1Fingerprint)) continue;
            const auto size = EVP_MD_size(md);
            auto value = unhex(fingerprint.value, static_cast<std::size_t>(size));
            if (!value) return false;
            if (size > best) { best = size; selected.clear(); selectedHash = md; algorithm = name; }
            if (size == best) selected.push_back(std::move(*value));
        }
        if (selected.empty()) return false;
        std::sort(selected.begin(), selected.end());
        expected = std::move(selected); preferredHash = selectedHash; preferredAlgorithm = std::move(algorithm);
        return true;
    }
    void flush(std::shared_ptr<Write> write = {})
    {
        while (output && !output->packets.empty())
        {
            if (wirePending >= config.maxPendingMessages)
            { fail(net::error::no_buffer_space, "DTLS encrypted send queue full"); return; }
            auto packet = std::move(output->packets.front()); output->packets.pop_front(); output->bytes -= packet.size();
            ++wirePending; if (write) ++write->pending;
            const auto version = session;
            const auto size = packet.size();
            const auto self = shared_from_this();
            auto done = [self, write, version, size](ErrorCode ec, std::size_t count) {
                net::post(self->strand, [self, write, version, size, ec, count]() mutable {
                    --self->wirePending;
                    if (write) --write->pending;
                    if (!ec && count != size) ec = net::error::message_size;
                    if (!ec) ++self->current.stats.sentDatagrams;
                    if (version == self->session && ec && !self->closing)
                        self->fail(ec, "DTLS underlying datagram send failed");
                    if (write && !write->finished && write->encoded && write->pending == 0) self->finishWrite(write, ec);
                    self->publish();
                    if (self->closing) self->finishClose();
                    else if (version == self->session) self->guarded([&] { self->drive(); });
                });
            };
            try { sender(std::move(packet), done); }
            catch (const std::exception&) { done(net::error::fault, 0); }
        }
        publish();
    }
    bool sslResult(int result, const char* message)
    {
        const auto error = SSL_get_error(ssl.get(), result); // 紧跟 SSL 调用，避免其他 OpenSSL 操作影响 error queue
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return true;
        if (error == SSL_ERROR_ZERO_RETURN) { beginClose(); return false; }
        flush(); // OpenSSL 已生成的 fatal alert 仍需交给对端，然后销毁失败会话
        fail(net::error::access_denied, message); return false;
    }
    DtlsSrtpKeyMaterial srtpKeys(DtlsSrtpProfile profile)
    {
        const auto sizes = srtpSizes(profile);
        const auto keySize = sizes.first, saltSize = sizes.second;
        Bytes exported(2 * (keySize + saltSize));
        static constexpr char label[] = "EXTRACTOR-dtls_srtp";
        require(SSL_export_keying_material(ssl.get(), exported.data(), exported.size(), label, sizeof(label) - 1,
            nullptr, 0, 0) == 1, "DTLS SRTP exporter failed");
        const bool client = *current.localSetup == SdpSetupRole::Active;
        const auto localIndex = client ? 0u : 1u, remoteIndex = client ? 1u : 0u;
        DtlsSrtpKeyMaterial keys;
        keys.generation = current.generation; keys.component = config.component; keys.profile = profile;
        keys.localMasterKey.assign(exported.begin() + localIndex * keySize, exported.begin() + (localIndex + 1) * keySize);
        keys.remoteMasterKey.assign(exported.begin() + remoteIndex * keySize, exported.begin() + (remoteIndex + 1) * keySize);
        keys.localMasterSalt.assign(exported.begin() + 2 * keySize + localIndex * saltSize,
            exported.begin() + 2 * keySize + (localIndex + 1) * saltSize);
        keys.remoteMasterSalt.assign(exported.begin() + 2 * keySize + remoteIndex * saltSize,
            exported.begin() + 2 * keySize + (remoteIndex + 1) * saltSize);
        OPENSSL_cleanse(exported.data(), exported.size()); return keys;
    }
    void connected()
    {
        Handle<X509, X509_free> peer(SSL_get1_peer_certificate(ssl.get()), X509_free);
        if (!verified || !certificateMatches(peer.get()))
        { fail(net::error::access_denied, "DTLS peer certificate fingerprint mismatch"); return; }
        std::optional<DtlsSrtpKeyMaterial> keys;
        const auto* selected = SSL_get_selected_srtp_profile(ssl.get());
        if (selected)
        {
            for (auto profile : config.srtpProfiles)
                if (std::strcmp(selected->name, srtpProfileString(profile)) == 0) { current.srtpProfile = profile; break; }
            if (!current.srtpProfile) { fail(net::error::operation_not_supported, "unsupported negotiated SRTP profile"); return; }
            keys = srtpKeys(*current.srtpProfile);
        }
        if (config.requireSrtp && !keys) { fail(net::error::operation_not_supported, "DTLS peer did not negotiate SRTP"); return; }
        unsigned size = 0; Bytes value(EVP_MAX_MD_SIZE);
        require(X509_digest(peer.get(), preferredHash, value.data(), &size) == 1, "cannot digest DTLS peer certificate");
        value.resize(size); current.peerFingerprint = SdpFingerprint{preferredAlgorithm, hex(value)};
        current.cipher = SSL_get_cipher_name(ssl.get()); current.protocol = SSL_get_version(ssl.get());
        current.maxPlaintextSize = std::min(config.maxPlaintextSize, DTLS_get_data_mtu(ssl.get()));
        if (srtp && keys)
        {
            auto ec = srtp->setKeys(*keys);
            if (!ec) ec = srtcp->setKeys(*keys);
            if (ec) { fail(ec, "could not install DTLS-SRTP media keys"); return; }
        }
        state(DtlsState::Connected);
        armRtcp();
        UAV_LOG_INFO << "[WebRTC][DTLS " << this << "][g=" << current.generation << "][c=" << config.component << "] handshake verified"
            << " role=" << (*current.localSetup == SdpSetupRole::Active ? "client" : "server") << " cipher=" << current.cipher
            << " fingerprint_algorithm=" << preferredAlgorithm << " srtp=" << (current.srtpProfile ? srtpProfileString(*current.srtpProfile) : "none")
            << " media_protection=" << config.enableSrtp << " max_plaintext=" << current.maxPlaintextSize
            << " handshake_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - (deadline - config.handshakeTimeout)).count();
        if (callbacks.onSrtpKeys && keys) callbacks.onSrtpKeys(std::move(*keys));
        if (callbacks.onConnected) callbacks.onConnected(current);
    }
    void pumpWrites()
    {
        if (current.state != DtlsState::Connected || writes.empty()) return;
        auto write = writes.front();
        if (write->encoded) return;
        ERR_clear_error();
        std::size_t size = 0;
        const int result = SSL_write_ex(ssl.get(), write->payload.data(), write->payload.size(), &size);
        if (result != 1 && !sslResult(result, "DTLS application encryption failed")) return;
        if (!ssl) return;
        if (result == 1)
        {
            if (size != write->payload.size()) { fail(net::error::fault, "DTLS partial application write"); return; }
            write->encoded = true;
        }
        flush(write);
        if (write->encoded && !write->finished && write->pending == 0) finishWrite(write, {});
    }
    void drive()
    {
        if (!ssl || closing || current.state == DtlsState::Failed) return;
        if (current.state == DtlsState::Connecting)
        {
            ERR_clear_error(); const int result = SSL_do_handshake(ssl.get());
            if (result != 1 && !sslResult(result, "DTLS handshake or fingerprint verification failed")) return;
            flush();
            if (!ssl) return;
            if (result == 1) connected();
        }
        if (current.state == DtlsState::Connected && ssl)
        {
            std::array<std::uint8_t, 16384> buffer{};
            // SSL 内部可能已经缓存同一 UDP 报文的后续 records，不能只调用一次 read。
            for (unsigned iteration = 0; iteration < 256; ++iteration)
            {
                ERR_clear_error(); std::size_t size = 0;
                const int result = SSL_read_ex(ssl.get(), buffer.data(), buffer.size(), &size);
                if (result != 1)
                {
                    if (!sslResult(result, "DTLS record authentication failed")) return;
                    break;
                }
                ++current.stats.receivedMessages; current.stats.receivedBytes += size; publish();
                if (callbacks.onData) callbacks.onData(Bytes(buffer.begin(), buffer.begin() + size));
            }
            flush();
            if (ssl) pumpWrites();
        }
        armTimer();
    }
    void armTimer()
    {
        ++timerVersion; timer.cancel();
        if (!ssl || closing || current.state == DtlsState::Failed) return;
        timeval timeout{};
        auto when = Clock::time_point::max();
        if (DTLSv1_get_timeout(ssl.get(), &timeout))
            when = Clock::now() + std::max(std::chrono::microseconds(1000),
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(timeout.tv_sec)) +
                std::chrono::microseconds(timeout.tv_usec));
        if (current.state == DtlsState::Connecting) when = std::min(when, deadline);
        if (when == Clock::time_point::max()) return;
        timer.expires_at(when);
        const auto version = timerVersion;
        const auto weak = weak_from_this();
        timer.async_wait([weak, version](ErrorCode ec) {
            if (const auto self = weak.lock())
            {
                if (ec || version != self->timerVersion || !self->ssl || self->closing) return;
                self->guarded([&] {
                    if (self->current.state == DtlsState::Connecting && Clock::now() >= self->deadline)
                    { self->fail(net::error::timed_out, "DTLS handshake timed out"); return; }
                    ERR_clear_error();
                    const auto result = DTLSv1_handle_timeout(self->ssl.get());
                    if (result < 0) { self->fail(net::error::timed_out, "DTLS handshake retransmission failed"); return; }
                    if (result > 0)
                    {
                        ++self->current.stats.retransmissions;
                        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG))
                            << "[WebRTC][DTLS " << self.get() << "][g=" << self->current.generation << "] handshake retransmission=" << self->current.stats.retransmissions;
                    }
                    self->flush(); self->drive();
                });
            }
        });
    }
    ErrorCode start(IceDtlsReady value)
    {
        if (closing || current.state == DtlsState::Closed) return net::error::operation_aborted;
        if (value.generation < current.generation) return net::error::invalid_argument;
        if (value.generation == current.generation && current.state == DtlsState::Failed) return net::error::access_denied;
        const auto local = value.parameters.localSetup, remote = value.parameters.remoteSetup;
        if (value.pair.local.component != config.component || !value.pair.valid || !value.pair.nominated ||
            value.pair.remoteEndpoint.port() == 0 || value.pair.remoteEndpoint.address().is_unspecified() ||
            (local != SdpSetupRole::Active && local != SdpSetupRole::Passive) ||
            (remote != SdpSetupRole::ActPass && remote != SdpSetupRole::Active && remote != SdpSetupRole::Passive) ||
            (remote != SdpSetupRole::ActPass && remote == local)) return net::error::invalid_argument;
        const auto oldExpected = expected; const auto* oldHash = preferredHash; const auto oldAlgorithm = preferredAlgorithm;
        if (!selectFingerprints(value.parameters.remoteFingerprints)) return net::error::invalid_argument;
        if (ready && value.generation == current.generation)
        {
            const bool same = ready->pair.remoteEndpoint == value.pair.remoteEndpoint && ready->pair.localEndpoint == value.pair.localEndpoint &&
                ready->parameters.localSetup == local && oldExpected == expected && oldHash == preferredHash;
            if (!same) { expected = oldExpected; preferredHash = oldHash; preferredAlgorithm = oldAlgorithm; return net::error::already_started; }
            return {};
        }
        if (value.generation > current.generation)
        {
            auto selected = expected; const auto* md = preferredHash; auto algorithm = preferredAlgorithm;
            reset(value.generation); expected = std::move(selected); preferredHash = md; preferredAlgorithm = std::move(algorithm);
        }
        ready = std::move(value); current.localSetup = local;
        UAV_LOG_INFO << "[WebRTC][DTLS " << this << "][g=" << current.generation << "][c=" << config.component << "] starting handshake role="
            << (local == SdpSetupRole::Active ? "client" : "server") << " local=" << ready->pair.localEndpoint << " remote=" << ready->pair.remoteEndpoint
            << " mtu=" << config.mtu;
        ssl.reset(SSL_new(context.get())); require(bool(ssl), "cannot create DTLS session");
        SSL_set_app_data(ssl.get(), this);
        Handle<BIO, BIO_free_all> readBio(BIO_new(datagramMethod()), BIO_free_all), writeBio(BIO_new(datagramMethod()), BIO_free_all);
        require(readBio && writeBio, "cannot create DTLS BIO queues");
        input = static_cast<DatagramBio*>(BIO_get_data(readBio.get()));
        output = static_cast<DatagramBio*>(BIO_get_data(writeBio.get()));
        input->maxBytes = config.maxBufferedBytes; input->maxPackets = config.maxBufferedDatagrams; input->mtu = config.mtu;
        output->maxBytes = config.maxPendingBytes; output->maxPackets = config.maxPendingMessages; output->mtu = config.mtu;
        SSL_set_bio(ssl.get(), readBio.release(), writeBio.release());
        require(SSL_set_mtu(ssl.get(), config.mtu) > 0, "cannot set DTLS MTU");
        if (local == SdpSetupRole::Active) SSL_set_connect_state(ssl.get()); else SSL_set_accept_state(ssl.get());
        deadline = Clock::now() + config.handshakeTimeout;
        state(DtlsState::Connecting);
        auto buffered = std::move(early); early.clear(); earlyBytes = 0;
        for (auto& packet : buffered)
            if (packet.generation == current.generation && packet.datagram.remoteEndpoint == ready->pair.remoteEndpoint)
                queueInput(std::move(packet.datagram.payload));
            else ++current.stats.discardedDatagrams;
        drive(); return {};
    }
    void queueInput(Bytes bytes)
    {
        if (input->packets.size() >= input->maxPackets || bytes.size() > input->maxBytes - input->bytes)
        { ++current.stats.discardedDatagrams; return; }
        input->bytes += bytes.size(); input->packets.push_back(std::move(bytes));
    }
    void receive(IceTransportPacket packet)
    {
        if (packet.type == IcePacketType::Rtp || packet.type == IcePacketType::Rtcp) { receiveMedia(std::move(packet)); return; }
        if (packet.component != config.component || packet.type != IcePacketType::Dtls ||
            classifyIcePacket(packet.datagram.payload) != IcePacketType::Dtls || packet.datagram.payload.size() > 65507 ||
            packet.generation < current.generation || closing ||
            (current.state == DtlsState::Failed && packet.generation == current.generation) || current.state == DtlsState::Closed)
        { ++current.stats.discardedDatagrams; publish(); return; }
        if (packet.generation > current.generation) reset(packet.generation);
        if (ready && packet.datagram.remoteEndpoint != ready->pair.remoteEndpoint)
        { ++current.stats.discardedDatagrams; publish(); return; }
        ++current.stats.receivedDatagrams;
        if (!ssl)
        {
            if (early.size() >= config.maxBufferedDatagrams || packet.datagram.payload.size() > config.maxBufferedBytes - earlyBytes)
                ++current.stats.discardedDatagrams;
            else { earlyBytes += packet.datagram.payload.size(); early.push_back(std::move(packet)); }
            publish(); return;
        }
        queueInput(std::move(packet.datagram.payload)); drive(); publish();
    }
    void send(Bytes bytes, SendHandler done)
    {
        ErrorCode ec;
        if (closing || current.state == DtlsState::Closed || current.state == DtlsState::Failed) ec = net::error::operation_aborted;
        else if (current.state != DtlsState::Connected) ec = net::error::not_connected;
        else if (bytes.empty()) ec = net::error::invalid_argument;
        else if (bytes.size() > current.maxPlaintextSize) ec = net::error::message_size;
        else if (writes.size() + mediaWrites.size() >= config.maxPendingMessages || bytes.size() > config.maxPendingBytes - pendingBytes - mediaBytes) ec = net::error::no_buffer_space;
        if (ec) { if (done) done(ec, 0); return; }
        auto write = std::make_shared<Write>(); write->payload = std::move(bytes); write->done = std::move(done);
        pendingBytes += write->payload.size(); writes.push_back(std::move(write)); publish(); drive();
    }
    void flushRtcp(Completion done = {})
    {
        if (!rtcp || closing || current.state != DtlsState::Connected) return;
        const auto self = shared_from_this(); const auto version = session;
        auto packets = rtcp->takeOutput();
        struct Flush { std::size_t remaining; ErrorCode error; Completion done; };
        std::shared_ptr<Flush> flush;
        if (done)
        {
            if (packets.empty()) { complete(done); return; }
            flush = std::make_shared<Flush>(Flush{packets.size(), {}, std::move(done)});
        }
        for (auto& packet : packets)
            sendMedia(std::move(packet.bytes), !packet.retransmission, current.generation,
                [self, version, flush](ErrorCode ec, std::size_t) {
                    if (ec && !self->closing && version == self->session)
                    {
                        const auto n = ++self->rtcpSendErrors;
                        UAV_LOG_IF(LogLevel::WARN, (n & (n - 1)) == 0) << "[WebRTC][RTCP][g=" << self->current.generation
                            << "] output failed error=" << ec.message() << " failures=" << n;
                    }
                    if (flush)
                    {
                        if (ec && !flush->error) flush->error = ec;
                        if (--flush->remaining == 0) complete(flush->done, flush->error);
                    }
                }, packet.retransmission);
    }
    void armRtcp()
    {
        if (!rtcp || rtcpArmed || closing || current.state != DtlsState::Connected || config.rtcp.parameters.media.empty() ||
            srtcp->snapshot().state != SrtpState::Ready) return;
        rtcpArmed = true; const auto version = ++rtcpTimerVersion;
        rtcpTimer.expires_after(config.rtcp.tickInterval); const auto self = shared_from_this();
        rtcpTimer.async_wait([self, version](ErrorCode ec) {
            if (version != self->rtcpTimerVersion) return;
            self->rtcpArmed = false;
            if (ec || self->closing || self->current.state != DtlsState::Connected) return;
            self->guarded([&] { self->rtcp->poll(Clock::now(), rtcpNtpNow()); self->flushRtcp(); self->publish(); self->armRtcp(); });
        });
    }
    void configureRtcp(RtcpParameters parameters, Completion done)
    {
        ErrorCode ec;
        if (closing || current.state == DtlsState::Closed) ec = net::error::operation_aborted;
        else if (!rtcp && !pacer) ec = net::error::operation_not_supported;
        else if (!validateRtcpParameters(parameters) || (rtcp && !rtcp->configure(parameters))) ec = net::error::invalid_argument;
        else
        {
            config.rtcp.parameters = std::move(parameters);
            ++rtcpTimerVersion; rtcpTimer.cancel(); rtcpArmed = false; armRtcp(); publish();
            UAV_LOG_DEBUG << "[WebRTC][RTCP][g=" << current.generation << "] configured payloads=" << config.rtcp.parameters.media.size()
                << " reduced_size=" << config.rtcp.parameters.reducedSize;
        }
        complete(done, ec);
    }
    void receiveMedia(IceTransportPacket packet)
    {
        if (!srtp || closing || current.state != DtlsState::Connected || !ready || packet.component != config.component ||
            packet.generation != current.generation || packet.datagram.remoteEndpoint != ready->pair.remoteEndpoint ||
            classifyIcePacket(packet.datagram.payload) != packet.type)
        {
            ++current.stats.discardedMediaPackets; publish();
            const auto n = current.stats.discardedMediaPackets;
            UAV_LOG_IF(LogLevel::DEBUG, (n & (n - 1)) == 0 && LogManager::getInstance().shouldLog(LogLevel::DEBUG))
                << "[WebRTC][DTLS " << this << "][g=" << current.generation << "] discarded media before authentication packets=" << n
                << " packet_generation=" << packet.generation << " component=" << packet.component;
            return;
        }
        const auto ec = packet.type == IcePacketType::Rtcp ? srtcp->unprotect(packet.datagram.payload, packet.generation) : srtp->unprotect(packet.datagram.payload, packet.generation);
        if (ec)
        {
            ++current.stats.discardedMediaPackets; publish();
            if (ec == SrtpError::KeyExpired) fail(ec, "SRTP media key expired");
            return;
        }
        if (rtcp)
        {
            if (packet.type == IcePacketType::Rtcp)
            {
                if (!rtcp->receivedRtcp(packet.datagram.payload, packet.datagram.receivedAt)) { publish(); return; }
                flushRtcp();
            }
            else rtcp->receivedRtp(packet.datagram.payload, packet.datagram.receivedAt);
        }
        publish();
        const auto& handler = packet.type == IcePacketType::Rtcp ? callbacks.onRtcpPacket : callbacks.onRtpPacket;
        if (handler) handler(std::move(packet));
    }
    void releaseMedia(const std::shared_ptr<MediaWrite>& write, ErrorCode ec, std::size_t size = 0)
    {
        if (!mediaWrites.erase(write)) return;
        mediaBytes -= write->reservationSize;
        if (write->submitted)
        {
            --wirePending;
            if (!ec && size != write->ciphertextSize) ec = net::error::message_size;
        }
        if (closing || write->version != session) ec = net::error::operation_aborted;
        if (!ec && !write->finished && rtcp && !write->trackedPlaintext.empty())
            rtcp->sentRtp(write->trackedPlaintext, std::move(write->trackedCiphertext), write->sentAt);
        publish(); finishMediaWrite(write, ec);
        if (closing) finishClose();
    }
    void dispatchMedia(const std::shared_ptr<MediaWrite>& write)
    {
        if (closing || write->version != session || write->generation != current.generation || current.state != DtlsState::Connected)
        { releaseMedia(write, net::error::operation_aborted); return; }
        auto bytes = std::move(write->payload); ErrorCode ec;
        if (!write->control && !write->retransmission && rtcp)
        {
            if (!rtcp->prepareRtp(bytes)) { releaseMedia(write, SrtpError::InvalidPacket); return; }
            write->trackedPlaintext = bytes;
        }
        if (!write->retransmission) ec = write->control ? srtcp->protect(bytes, current.generation) : srtp->protect(bytes, current.generation);
        if (ec)
        {
            releaseMedia(write, ec);
            if (ec == SrtpError::KeyExpired) fail(ec, "SRTP media key expired");
            return;
        }
        write->ciphertextSize = bytes.size();
        if (bytes.size() != write->reservationSize) { releaseMedia(write, net::error::message_size); return; }
        if (!write->trackedPlaintext.empty()) write->trackedCiphertext = bytes;
        // Timestamp at paced egress, after crypto, immediately before submitting to the UDP Sender.
        write->sentAt = Clock::now(); write->submitted = true; ++wirePending;
        const auto self = shared_from_this();
        SendHandler finish = [self, write](ErrorCode result, std::size_t size) {
            net::post(self->strand, [self, write, result, size] { self->releaseMedia(write, result, size); });
        };
        publish();
        try { sender(std::move(bytes), finish); } catch (const std::exception&) { finish(net::error::fault, 0); }
    }
    void pumpPacer()
    {
        if (!pacer || closing || current.state != DtlsState::Connected) return;
        const auto oldRate = pacer->snapshot(Clock::now()).adjustedRateBps;
        ++pacerTimerVersion; pacerTimer.cancel();
        const auto expired = pacer->expire(Clock::now());
        for (const auto& packet : expired)
        {
            const auto found = pacedWrites.find(packet.id); if (found == pacedWrites.end()) continue;
            auto write = found->second; pacedWrites.erase(found); releaseMedia(write, net::error::timed_out);
        }
        if (!expired.empty())
        {
            const auto n = pacer->snapshot(Clock::now()).expiredPackets;
            UAV_LOG_IF(LogLevel::WARN, n == expired.size() || (n & (n - 1)) == 0) << "[WebRTC][Pacer][g=" << current.generation << "] expired RTP packets=" << n;
        }
        // Limit each batch so a high pacing rate cannot monopolize the transport strand.
        for (unsigned i = 0; i < 64 && !closing && current.state == DtlsState::Connected; ++i)
        {
            const auto packet = pacer->pop(Clock::now()); if (!packet) break;
            const auto found = pacedWrites.find(packet->id); if (found == pacedWrites.end()) continue;
            auto write = found->second; pacedWrites.erase(found); dispatchMedia(write);
        }
        const auto pacing = pacer->snapshot(Clock::now());
        if (pacing.adjustedRateBps != oldRate)
            UAV_LOG_DEBUG << "[WebRTC][Pacer][g=" << current.generation << "] adjusted_rate_bps=" << pacing.adjustedRateBps
                << " pacing_rate_bps=" << pacing.rateBps << " queued_bytes=" << pacing.queuedBytes;
        publish();
        if (closing || current.state != DtlsState::Connected) return;
        const auto wakeup = pacer->nextWakeup(Clock::now()); if (!wakeup) return;
        pacerTimer.expires_at(*wakeup); const auto version = pacerTimerVersion; const auto weak = weak_from_this();
        pacerTimer.async_wait([weak, version](ErrorCode ec) {
            if (const auto self = weak.lock())
                if (!ec && version == self->pacerTimerVersion) self->guarded([&] { self->pumpPacer(); });
        });
    }
    void configurePacingRates(std::uint64_t rate, std::optional<std::uint64_t> padding, Completion done)
    {
        ErrorCode ec;
        if (closing || current.state == DtlsState::Closed || current.state == DtlsState::Failed) ec = net::error::operation_aborted;
        else if (!pacer) ec = net::error::operation_not_supported;
        else if (!pacer->setRates(rate, padding.value_or(config.pacer.paddingRateBps), Clock::now())) ec = net::error::invalid_argument;
        else
        {
            config.pacer.rateBps = rate;
            if (padding) config.pacer.paddingRateBps = *padding;
            pumpPacer(); publish();
            UAV_LOG_DEBUG << "[WebRTC][Pacer][g=" << current.generation << "] pacing_rate_bps=" << rate
                << " padding_rate_bps=" << pacer->snapshot(Clock::now()).paddingRateBps;
        }
        complete(done, ec);
    }
    void sendMedia(Bytes bytes, bool control, std::optional<std::uint64_t> generation, SendHandler done, bool retransmission = false)
    {
        ErrorCode ec;
        const auto plainSize = bytes.size();
        if (closing || current.state == DtlsState::Closed || current.state == DtlsState::Failed) ec = net::error::operation_aborted;
        else if (!srtp) ec = net::error::operation_not_supported;
        else if (current.state != DtlsState::Connected) ec = net::error::not_connected;
        else if (generation && *generation != current.generation) ec = SrtpError::GenerationMismatch;
        else
        {
            const auto snapshot = control ? srtcp->snapshot() : srtp->snapshot();
            const auto overhead = retransmission ? 0 : snapshot.overhead;
            if (snapshot.state != SrtpState::Ready) ec = SrtpError::NotReady;
            else if (!control && !retransmission && (!RtpPacketParser(bytes.data(), bytes.size()).valid() || (rtcp && !rtcp->reserveRtpExtensions(bytes)))) ec = SrtpError::InvalidPacket;
            else if (bytes.size() > (retransmission ? config.srtp.maxPacketSize : snapshot.maxPlaintextSize)) ec = SrtpError::PacketTooLarge;
            else if (bytes.size() + overhead > config.mtu) ec = SrtpError::PacketTooLarge;
            else if (writes.size() + mediaWrites.size() >= config.maxPendingMessages ||
                bytes.size() + overhead > config.maxPendingBytes - pendingBytes - mediaBytes) ec = net::error::no_buffer_space;
        }
        if (ec) { if (done) done(ec, 0); return; }
        // Reserve queue storage before consuming SRTP/TWCC sequences; actual crypto runs at egress.
        auto write = std::make_shared<MediaWrite>();
        write->plaintextSize = plainSize; write->done = std::move(done); write->payload = std::move(bytes);
        write->id = ++nextMediaId; write->version = session; write->generation = current.generation;
        write->control = control; write->retransmission = retransmission;
        write->reservationSize = write->payload.size() + (retransmission ? 0 : (control ? srtcp->snapshot() : srtp->snapshot()).overhead);
        mediaWrites.insert(write); mediaBytes += write->reservationSize;
        if (!control && pacer)
        {
            RtpPacketParser rtp(write->payload.data(), write->payload.size());
            Pacer::Priority priority = retransmission ? Pacer::Priority::Retransmission : Pacer::Priority::Video;
            if (!retransmission)
            {
                if (rtp.padding() && !rtp.payloadSize()) priority = Pacer::Priority::Padding;
                else for (const auto& m : config.rtcp.parameters.media)
                    if (m.payloadType == rtp.payloadType() && m.send && m.audio) { priority = Pacer::Priority::Audio; break; }
            }
            // SSRC/sequence/header fields stay in cleartext in cached SRTP; don't parse ciphertext padding.
            const auto ssrc = (std::uint32_t(write->payload[8]) << 24) | (std::uint32_t(write->payload[9]) << 16) |
                (std::uint32_t(write->payload[10]) << 8) | write->payload[11];
            const auto admission = pacer->enqueue({write->id, current.generation, ssrc, priority, write->reservationSize}, Clock::now());
            if (admission != Pacer::Admission::Accepted)
            { releaseMedia(write, admission == Pacer::Admission::QueueFull ? ErrorCode(net::error::no_buffer_space) : ErrorCode(net::error::invalid_argument)); return; }
            pacedWrites.emplace(write->id, write); publish(); pumpPacer();
        }
        else dispatchMedia(write);
    }
    ErrorCode reset(std::uint64_t generation)
    {
        if (closing || current.state == DtlsState::Closed) return net::error::operation_aborted;
        if (generation <= current.generation) return net::error::invalid_argument;
        UAV_LOG_INFO << "[WebRTC][DTLS " << this << "] reset generation " << current.generation << " -> " << generation;
        clearSsl(); current.generation = generation;
        if (rtcp) rtcp->reset(generation);
        if (srtp)
        {
            auto ec = srtp->reset(generation); if (!ec) ec = srtcp->reset(generation);
            if (ec) { fail(ec, "could not reset SRTP media generation"); return ec; }
        }
        cancelWrites(net::error::operation_aborted);
        if (pacer) pacer->reset(generation, Clock::now());
        state(DtlsState::New); return {};
    }
    void beginClose()
    {
        if (closing || current.state == DtlsState::Closed) return;
        closing = true; ++timerVersion; timer.cancel();
        ++rtcpTimerVersion; rtcpTimer.cancel(); rtcpArmed = false;
        if (srtp) { srtp->close(); srtcp->close(); }
        state(DtlsState::Closing); cancelWrites(net::error::operation_aborted);
        if (ssl && SSL_is_init_finished(ssl.get())) { ERR_clear_error(); SSL_shutdown(ssl.get()); flush(); }
        finishClose();
    }
    void finishClose()
    {
        if (!closing || wirePending || !mediaWrites.empty() || current.state == DtlsState::Closed) return;
        clearSsl(); state(DtlsState::Closed);
        UAV_LOG_INFO << "[WebRTC][DTLS " << this << "][g=" << current.generation << "] closed sent_records=" << current.stats.sentMessages
            << " received_records=" << current.stats.receivedMessages << " retransmissions=" << current.stats.retransmissions << " discarded_media=" << current.stats.discardedMediaPackets;
        auto handlers = std::move(closeCallbacks); closeCallbacks.clear();
        for (const auto& done : handlers) complete(done);
    }
    void close(Completion done)
    {
        if (current.state == DtlsState::Closed) { complete(done); return; }
        if (done) closeCallbacks.push_back(std::move(done));
        beginClose(); finishClose();
    }
};

DtlsTransport::DtlsTransport(net::io_context& io, Sender sender) : DtlsTransport(io, std::move(sender), Config{}) {}
DtlsTransport::DtlsTransport(net::io_context& io, Sender sender, Config config, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(io, std::move(sender), std::move(config), std::move(callbacks))) {}
DtlsTransport::~DtlsTransport() { close(); }
DtlsTransport::Snapshot DtlsTransport::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->snapshotMutex); return impl_->published;
}
std::shared_ptr<const DtlsCertificate> DtlsTransport::certificate() const { return impl_->config.certificate; }
void DtlsTransport::start(IceDtlsReady ready, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, ready = std::move(ready), done = std::move(done)]() mutable {
        ErrorCode ec;
        try { ec = self->start(std::move(ready)); }
        catch (const std::exception&) { self->fail(net::error::fault, "cannot initialize DTLS association"); ec = net::error::fault; }
        self->publish(); complete(done, ec);
    });
}
void DtlsTransport::handlePacket(IceTransportPacket packet)
{
    const auto self = impl_;
    net::post(self->strand, [self, packet = std::move(packet)]() mutable { self->guarded([&] { self->receive(std::move(packet)); }); });
}
void DtlsTransport::handleIceState(IceTransport::Snapshot state)
{
    const auto self = impl_;
    net::post(self->strand, [self, state = std::move(state)] {
        if (state.generation < self->current.generation) return;
        if (state.generation > self->current.generation) self->reset(state.generation);
        if (state.state == IceTransportState::Closed) self->close({});
        else if (state.state == IceTransportState::Failed) self->fail(net::error::connection_reset, "DTLS ICE transport failed");
    });
}
void DtlsTransport::asyncSend(Bytes plaintext, SendHandler done)
{
    const auto self = impl_;
    net::post(self->strand, [self, plaintext = std::move(plaintext), done = std::move(done)]() mutable {
        self->guarded([&] { self->send(std::move(plaintext), std::move(done)); });
    });
}
void DtlsTransport::asyncSendRtp(Bytes plaintext, SendHandler done)
{
    const auto self = impl_; net::post(self->strand, [self, plaintext = std::move(plaintext), done = std::move(done)]() mutable {
        self->sendMedia(std::move(plaintext), false, std::nullopt, std::move(done));
    });
}
void DtlsTransport::asyncSendRtp(Bytes plaintext, std::uint64_t generation, SendHandler done)
{
    const auto self = impl_; net::post(self->strand, [self, plaintext = std::move(plaintext), generation, done = std::move(done)]() mutable {
        self->sendMedia(std::move(plaintext), false, generation, std::move(done));
    });
}
void DtlsTransport::asyncSendRtcp(Bytes plaintext, SendHandler done)
{
    const auto self = impl_; net::post(self->strand, [self, plaintext = std::move(plaintext), done = std::move(done)]() mutable {
        self->sendMedia(std::move(plaintext), true, std::nullopt, std::move(done));
    });
}
void DtlsTransport::asyncSendRtcp(Bytes plaintext, std::uint64_t generation, SendHandler done)
{
    const auto self = impl_; net::post(self->strand, [self, plaintext = std::move(plaintext), generation, done = std::move(done)]() mutable {
        self->sendMedia(std::move(plaintext), true, generation, std::move(done));
    });
}
void DtlsTransport::exportKeyingMaterial(std::string label, std::size_t size, std::optional<Bytes> context,
                                       std::function<void(ErrorCode, Bytes)> done)
{
    const auto self = impl_;
    net::post(self->strand, [self, label = std::move(label), size, context = std::move(context), done = std::move(done)] {
        ErrorCode ec; Bytes result;
        if (self->current.state != DtlsState::Connected) ec = net::error::not_connected;
        else if (size == 0 || size > 4096 || label.empty() || label.size() > 256 ||
            !std::all_of(label.begin(), label.end(), [](unsigned char ch) { return ch >= 0x20 && ch <= 0x7e; }) ||
            (context && context->size() > 65535)) ec = net::error::invalid_argument;
        else
        {
            result.resize(size);
            if (SSL_export_keying_material(self->ssl.get(), result.data(), result.size(), label.data(), label.size(),
                context && !context->empty() ? context->data() : nullptr, context ? context->size() : 0, context ? 1 : 0) != 1)
            { OPENSSL_cleanse(result.data(), result.size()); result.clear(); ec = net::error::invalid_argument; ERR_clear_error(); }
        }
        if (done) done(ec, std::move(result));
    });
}
void DtlsTransport::configureRtcp(RtcpParameters parameters, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, parameters = std::move(parameters), done = std::move(done)]() mutable {
        self->configureRtcp(std::move(parameters), std::move(done));
    });
}
void DtlsTransport::requestPli(std::uint32_t mediaSsrc, std::uint64_t generation, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, mediaSsrc, generation, done = std::move(done)]() mutable {
        ErrorCode ec;
        if (self->closing || self->current.state != DtlsState::Connected) ec = net::error::not_connected;
        else if (generation != self->current.generation) ec = SrtpError::GenerationMismatch;
        else if (!self->rtcp || !self->rtcp->requestPli(mediaSsrc, Clock::now())) ec = net::error::operation_not_supported;
        else { self->flushRtcp(std::move(done)); self->publish(); return; }
        complete(done, ec);
    });
}
void DtlsTransport::reset(std::uint64_t generation, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, generation, done = std::move(done)] { complete(done, self->reset(generation)); });
}
void DtlsTransport::setPacingRate(std::uint64_t rateBps, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, rateBps, done = std::move(done)] {
        self->configurePacingRates(rateBps, std::nullopt, std::move(done));
    });
}
void DtlsTransport::setPacingRates(std::uint64_t rateBps, std::uint64_t paddingRateBps, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, rateBps, paddingRateBps, done = std::move(done)] {
        self->configurePacingRates(rateBps, paddingRateBps, std::move(done));
    });
}
void DtlsTransport::setPacingPaused(bool paused, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, paused, done = std::move(done)] {
        ErrorCode ec;
        if (self->closing || self->current.state == DtlsState::Closed || self->current.state == DtlsState::Failed) ec = net::error::operation_aborted;
        else if (!self->pacer) ec = net::error::operation_not_supported;
        else
        {
            self->pacer->setPaused(paused, Clock::now()); self->pumpPacer(); self->publish();
            UAV_LOG_DEBUG << "[WebRTC][Pacer][g=" << self->current.generation << "] paused=" << paused;
        }
        complete(done, ec);
    });
}
void DtlsTransport::close(Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, done = std::move(done)]() mutable { self->close(std::move(done)); });
}
} // namespace webrtc
