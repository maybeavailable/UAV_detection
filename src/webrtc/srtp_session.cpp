#include"webrtc/srtp_session.h"
#include"log/log.h"

#include<srtp2/srtp.h>
#include<openssl/crypto.h>
#include<openssl/evp.h>

#include<algorithm>
#include<array>
#include<cstring>
#include<mutex>
#include<new>
#include<set>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace
{
using ErrorCode = UdpSocket::ErrorCode;
using Digest = std::array<unsigned char, 32>;
class ErrorCategory final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "webrtc.srtp"; }
    std::string message(int value) const override
    {
        switch (static_cast<SrtpError>(value))
        {
        case SrtpError::NotReady: return "SRTP keys are not installed";
        case SrtpError::Closed: return "SRTP controller is closed";
        case SrtpError::InvalidArgument: return "invalid SRTP configuration or key material";
        case SrtpError::InvalidPacket: return "invalid RTP/RTCP packet";
        case SrtpError::AuthenticationFailed: return "SRTP authentication failed";
        case SrtpError::ReplayDetected: return "duplicate SRTP packet index";
        case SrtpError::ReplayTooOld: return "SRTP packet is outside the replay window";
        case SrtpError::KeyExpired: return "SRTP key or packet index exhausted; rekey required";
        case SrtpError::StreamLimit: return "SRTP SSRC limit reached";
        case SrtpError::PacketTooLarge: return "SRTP packet exceeds the datagram limit";
        case SrtpError::GenerationMismatch: return "stale or invalid SRTP generation";
        case SrtpError::ConflictingKeys: return "different SRTP keys in the same generation";
        case SrtpError::ReusedKey: return "SRTP key material was already used";
        case SrtpError::KeyHistoryFull: return "SRTP key generation limit reached";
        case SrtpError::UnsupportedProfile: return "SRTP profile is unsupported by the crypto backend";
        case SrtpError::CryptoFailure: return "SRTP crypto operation failed";
        case SrtpError::NoMemory: return "SRTP allocation failed";
        }
        return "unknown SRTP error";
    }
};
ErrorCode statusError(srtp_err_status_t status)
{
    switch (status)
    {
    case srtp_err_status_ok: return {};
    case srtp_err_status_bad_param: return SrtpError::InvalidArgument;
    case srtp_err_status_alloc_fail: return SrtpError::NoMemory;
    case srtp_err_status_auth_fail: return SrtpError::AuthenticationFailed;
    case srtp_err_status_replay_fail: return SrtpError::ReplayDetected;
    case srtp_err_status_replay_old: return SrtpError::ReplayTooOld;
    case srtp_err_status_key_expired: return SrtpError::KeyExpired;
    case srtp_err_status_no_such_op: return SrtpError::UnsupportedProfile;
    case srtp_err_status_parse_err: return SrtpError::InvalidPacket;
    default: return SrtpError::CryptoFailure;
    }
}
ErrorCode initializeLibrary()
{
    static std::once_flag once;
    static srtp_err_status_t result = srtp_err_status_init_fail;
    std::call_once(once, [] { result = srtp_init(); });
    // 不调用全局 srtp_shutdown：其它实例或应用自己的 libsrtp session 可能仍在使用。
    return statusError(result);
}
srtp_profile_t nativeProfile(DtlsSrtpProfile profile)
{
    switch (profile)
    {
    case DtlsSrtpProfile::AeadAes128Gcm: return srtp_profile_aead_aes_128_gcm;
    case DtlsSrtpProfile::AeadAes256Gcm: return srtp_profile_aead_aes_256_gcm;
    case DtlsSrtpProfile::Aes128CmSha1_80: return srtp_profile_aes128_cm_sha1_80;
    case DtlsSrtpProfile::Aes128CmSha1_32: return srtp_profile_aes128_cm_sha1_32;
    }
    return srtp_profile_reserved;
}
struct SessionDeleter { void operator()(srtp_ctx_t* p) const { if (p) srtp_dealloc(p); } };
using Session = std::unique_ptr<srtp_ctx_t, SessionDeleter>;
struct Secret
{
    std::array<unsigned char, 46> bytes{};
    ~Secret() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    void assign(const UdpSocket::Bytes& key, const UdpSocket::Bytes& salt)
    {
        std::copy(key.begin(), key.end(), bytes.begin());
        std::copy(salt.begin(), salt.end(), bytes.begin() + key.size());
    }
    bool digest(std::size_t size, Digest& out) const
    {
        unsigned length = 0;
        return EVP_Digest(bytes.data(), size, out.data(), &length, EVP_sha256(), nullptr) == 1 && length == out.size();
    }
};
struct Cleanse
{
    void* data; std::size_t size;
    ~Cleanse() { OPENSSL_cleanse(data, size); }
};
std::uint16_t read16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t read32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
bool rtpValid(const std::uint8_t* p, std::size_t size, bool plaintext, bool mux)
{
    if (size < 12 || (p[0] >> 6) != 2 || (mux && (p[1] & 127) >= 64 && (p[1] & 127) <= 95)) return false;
    std::size_t header = 12 + 4 * (p[0] & 15);
    if (header > size) return false;
    if (p[0] & 0x10)
    {
        if (size - header < 4) return false;
        header += 4 + 4 * std::size_t(read16(p + header + 2));
        if (header > size) return false;
    }
    return !plaintext || !(p[0] & 0x20) || (p[size - 1] != 0 && p[size - 1] <= size - header);
}
bool rtcpHeaderValid(const std::uint8_t* p, std::size_t size)
{
    if (size < 8 || size % 4 || (p[0] >> 6) != 2 || p[1] < 192 || p[1] > 223) return false;
    const auto firstSize = 4 * (std::size_t(read16(p + 2)) + 1);
    return firstSize >= 8 && firstSize <= size;
}
bool rtcpValid(const std::uint8_t* p, std::size_t size)
{
    if (!rtcpHeaderValid(p, size)) return false;
    for (std::size_t offset = 0; offset < size;)
    {
        if (size - offset < 4) return false;
        const auto* h = p + offset;
        if ((h[0] >> 6) != 2 || h[1] < 192 || h[1] > 223) return false;
        const auto bytes = 4 * (std::size_t(read16(h + 2)) + 1);
        if (bytes > size - offset) return false;
        auto content = bytes;
        if (h[0] & 0x20)
        {
            if (bytes != size - offset || h[bytes - 1] == 0 || h[bytes - 1] > bytes - 4) return false;
            content -= h[bytes - 1];
        }
        const auto count = h[0] & 31;
        // 核心固定字段/报告块边界；SDES/BYE/APP/反馈负载的语义由 RTCP 模块解释。
        const std::size_t minimum = h[1] == 200 ? 28 + 24 * count : h[1] == 201 ? 8 + 24 * count :
            h[1] == 204 || h[1] == 205 || h[1] == 206 ? 12 : h[1] == 207 ? 8 : 4;
        if (content < minimum) return false;
        offset += bytes;
    }
    return true;
}
} // namespace

const boost::system::error_category& srtpErrorCategory() { static ErrorCategory category; return category; }
boost::system::error_code make_error_code(SrtpError error) { return {static_cast<int>(error), srtpErrorCategory()}; }
std::optional<SrtpProfileInfo> srtpProfileInfo(DtlsSrtpProfile profile)
{
    switch (profile)
    {
    case DtlsSrtpProfile::AeadAes128Gcm: return SrtpProfileInfo{16, 12, 16, 16};
    case DtlsSrtpProfile::AeadAes256Gcm: return SrtpProfileInfo{32, 12, 16, 16};
    case DtlsSrtpProfile::Aes128CmSha1_80: return SrtpProfileInfo{16, 14, 10, 10};
    case DtlsSrtpProfile::Aes128CmSha1_32: return SrtpProfileInfo{16, 14, 4, 10}; // SRTCP 始终 SHA1-80
    }
    return std::nullopt;
}

namespace detail
{
struct SrtpSession::Impl
{
    SrtpConfig config;
    bool rtcp;
    mutable std::mutex mutex;
    SrtpSnapshot current;
    Session outbound, inbound;
    std::set<std::uint32_t> outboundSsrcs, inboundSsrcs;
    std::set<Digest> keyHistory;
    std::optional<std::pair<Digest, Digest>> currentKeys;
    std::size_t keyGenerations{0};
    std::vector<std::uint32_t> scratch; // 保证 libsrtp 要求的 32-bit 对齐

    Impl(SrtpConfig options, bool control) : config(std::move(options)), rtcp(control)
    {
        if (config.component == 0 || config.component > 256 || config.maxPacketSize < 12 || config.maxPacketSize > 65507 ||
            !config.maxStreams || config.maxStreams > 4096 || config.replayWindow < 64 || config.replayWindow >= 32768 ||
            !config.maxKeyGenerations || config.maxKeyGenerations > 65536) throw std::invalid_argument("invalid SRTP controller configuration");
        scratch.resize((config.maxPacketSize + SRTP_MAX_TRAILER_LEN + 4 + 3) / 4);
        current.component = config.component;
    }
    ~Impl() { clearContexts(); OPENSSL_cleanse(scratch.data(), scratch.size() * sizeof(scratch[0])); }
    void clearContexts()
    {
        outbound.reset(); inbound.reset(); outboundSsrcs.clear(); inboundSsrcs.clear();
        current.outboundStreams = current.inboundStreams = 0;
    }
    ErrorCode create(Session& target, srtp_policy_t policy, Secret& secret, bool sending)
    {
        policy.ssrc.type = sending ? ssrc_any_outbound : ssrc_any_inbound;
        policy.key = secret.bytes.data();
        srtp_t value = nullptr;
        const auto result = srtp_create(&value, &policy);
        if (result != srtp_err_status_ok) return statusError(result);
        target.reset(value); return {};
    }
    ErrorCode setKeys(const DtlsSrtpKeyMaterial& keys)
    {
        if (current.state == SrtpState::Closed) return SrtpError::Closed;
        if (keys.generation < current.generation) return SrtpError::GenerationMismatch;
        const auto info = srtpProfileInfo(keys.profile);
        if (!info) return SrtpError::UnsupportedProfile;
        if (keys.component != config.component || keys.localMasterKey.size() != info->masterKeySize || keys.remoteMasterKey.size() != info->masterKeySize ||
            keys.localMasterSalt.size() != info->masterSaltSize || keys.remoteMasterSalt.size() != info->masterSaltSize)
            return SrtpError::InvalidArgument;
        const auto overhead = rtcp ? info->rtcpOverhead() : info->rtpOverhead();
        if (config.maxPacketSize < overhead + (rtcp ? 8 : 12)) return SrtpError::InvalidArgument;
        Secret local, remote; local.assign(keys.localMasterKey, keys.localMasterSalt); remote.assign(keys.remoteMasterKey, keys.remoteMasterSalt);
        Digest localId{}, remoteId{};
        if (!local.digest(info->masterKeySize + info->masterSaltSize, localId) || !remote.digest(info->masterKeySize + info->masterSaltSize, remoteId))
            return SrtpError::CryptoFailure;
        if (currentKeys && keys.generation == current.generation)
        {
            if (current.state == SrtpState::Ready && current.profile == keys.profile && currentKeys->first == localId && currentKeys->second == remoteId) return {};
            return SrtpError::ConflictingKeys;
        }
        if (localId == remoteId) return SrtpError::InvalidArgument;
        if (keyHistory.count(localId) || keyHistory.count(remoteId)) return SrtpError::ReusedKey;
        if (keyGenerations >= config.maxKeyGenerations) return SrtpError::KeyHistoryFull;
        if (const auto ec = initializeLibrary()) return ec;
        srtp_policy_t policy{};
        if (srtp_crypto_policy_set_from_profile_for_rtp(&policy.rtp, nativeProfile(keys.profile)) != srtp_err_status_ok ||
            srtp_crypto_policy_set_from_profile_for_rtcp(&policy.rtcp, nativeProfile(keys.profile)) != srtp_err_status_ok)
            return SrtpError::UnsupportedProfile;
        policy.window_size = static_cast<unsigned long>(config.replayWindow); policy.allow_repeat_tx = 0;
        // 先构造两个上下文及新的历史记录，任意一步失败都保留旧代的密钥与重放窗口。
        auto history = keyHistory; history.insert(localId); history.insert(remoteId);
        Session sending, receiving;
        if (const auto ec = create(sending, policy, local, true)) return ec;
        if (const auto ec = create(receiving, policy, remote, false)) return ec;
        clearContexts(); outbound = std::move(sending); inbound = std::move(receiving); keyHistory.swap(history);
        currentKeys = std::make_pair(localId, remoteId); ++keyGenerations;
        current.state = SrtpState::Ready; current.generation = keys.generation; current.profile = keys.profile;
        current.overhead = overhead; current.maxPlaintextSize = config.maxPacketSize - overhead;
        UAV_LOG_INFO << "[WebRTC][" << (rtcp ? "SRTCP" : "SRTP") << " " << this << "][g=" << current.generation << "][c=" << config.component
            << "] keys installed profile=" << static_cast<int>(keys.profile) << " overhead=" << overhead << " max_plaintext=" << current.maxPlaintextSize
            << " replay_window=" << config.replayWindow << " max_ssrcs=" << config.maxStreams;
        return {};
    }
    ErrorCode process(UdpSocket::Bytes& packet, bool protect, std::optional<std::uint64_t> generation)
    {
        auto failure = [&](ErrorCode ec) {
            if (protect) ++current.stats.failedProtects; else ++current.stats.failedUnprotects;
            if (ec == SrtpError::AuthenticationFailed) ++current.stats.authenticationFailures;
            if (ec == SrtpError::ReplayDetected || ec == SrtpError::ReplayTooOld) ++current.stats.replayFailures;
            const auto n = protect ? current.stats.failedProtects : current.stats.failedUnprotects;
            UAV_LOG_IF(LogLevel::WARN, ec != SrtpError::Closed && (n & (n - 1)) == 0)
                << "[WebRTC][" << (rtcp ? "SRTCP" : "SRTP") << " " << this << "][g=" << current.generation << "] " << (protect ? "protect" : "unprotect")
                << " rejected error=" << ec.value() << ":" << ec.message() << " packet_bytes=" << packet.size() << " failures=" << n;
            return ec;
        };
        if (current.state == SrtpState::Closed) return failure(SrtpError::Closed);
        if (generation && *generation != current.generation) return failure(SrtpError::GenerationMismatch);
        if (current.state != SrtpState::Ready) return failure(SrtpError::NotReady);
        if (packet.size() > (protect ? current.maxPlaintextSize : config.maxPacketSize)) return failure(SrtpError::PacketTooLarge);
        const auto overhead = protect ? 0 : current.overhead;
        const auto minimum = rtcp ? 8u : 12u;
        if (packet.size() < minimum + overhead) return failure(SrtpError::InvalidPacket);
        const auto size = packet.size() - overhead;
        if (rtcp)
        {
            if (!rtcpHeaderValid(packet.data(), size) || (protect && !rtcpValid(packet.data(), size))) return failure(SrtpError::InvalidPacket);
            // GCM 的 index 位于 tag 后，AES-CM 位于 tag 前；两种 profile 均要求 E=1。
            if (!protect)
            {
                const auto indexOffset = *current.profile == DtlsSrtpProfile::AeadAes128Gcm || *current.profile == DtlsSrtpProfile::AeadAes256Gcm ?
                    packet.size() - 4 : packet.size() - current.overhead;
                if (!(packet[indexOffset] & 0x80)) return failure(SrtpError::InvalidPacket);
            }
        }
        else if (!rtpValid(packet.data(), size, protect, config.rtcpMux)) return failure(SrtpError::InvalidPacket);
        const auto ssrc = read32(packet.data() + (rtcp ? 4 : 8));
        auto& streams = protect ? outboundSsrcs : inboundSsrcs;
        const bool known = streams.count(ssrc) != 0;
        if (!known && streams.size() >= config.maxStreams) return failure(SrtpError::StreamLimit);
        // 分配输出和 SSRC 记录在处理前完成，避免成功后因内存不足丢失报文但消耗发送 index。
        UdpSocket::Bytes output;
        try { output.resize(protect ? packet.size() + current.overhead : size); if (!known) streams.insert(ssrc); }
        catch (const std::bad_alloc&) { return failure(SrtpError::NoMemory); }
        auto* buffer = reinterpret_cast<std::uint8_t*>(scratch.data());
        std::memcpy(buffer, packet.data(), packet.size());
        Cleanse cleanse{buffer, packet.size() + SRTP_MAX_TRAILER_LEN + 4};
        int length = static_cast<int>(packet.size());
        const auto session = protect ? outbound.get() : inbound.get();
        const auto result = rtcp ? (protect ? srtp_protect_rtcp(session, buffer, &length) : srtp_unprotect_rtcp(session, buffer, &length)) :
                                  (protect ? srtp_protect(session, buffer, &length) : srtp_unprotect(session, buffer, &length));
        if (result != srtp_err_status_ok)
        {
            if (!known)
            {
                // 不让未通过认证的 SSRC 占满上下文；其它已认证 SSRC 的重放窗口保持不变。
                std::uint32_t networkSsrc = 0;
                std::memcpy(&networkSsrc, packet.data() + (rtcp ? 4 : 8), sizeof(networkSsrc));
                srtp_remove_stream(session, networkSsrc); streams.erase(ssrc);
            }
            if (result == srtp_err_status_key_expired) { clearContexts(); current.state = SrtpState::Failed; }
            return failure(statusError(result));
        }
        current.outboundStreams = outboundSsrcs.size(); current.inboundStreams = inboundSsrcs.size();
        if (length < 0 || static_cast<std::size_t>(length) != output.size()) return failure(SrtpError::CryptoFailure);
        if (!protect && !(rtcp ? rtcpValid(buffer, output.size()) : rtpValid(buffer, output.size(), true, config.rtcpMux))) return failure(SrtpError::InvalidPacket);
        std::memcpy(output.data(), buffer, output.size()); packet.swap(output);
        if (protect) { ++current.stats.protectedPackets; current.stats.protectedBytes += packet.size(); }
        else { ++current.stats.unprotectedPackets; current.stats.unprotectedBytes += packet.size(); }
        return {};
    }
};

SrtpSession::SrtpSession(SrtpConfig config, bool rtcp) : impl_(std::make_unique<Impl>(std::move(config), rtcp)) {}
SrtpSession::~SrtpSession() = default;
SrtpSession::ErrorCode SrtpSession::setKeys(const DtlsSrtpKeyMaterial& keys)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ErrorCode ec;
    try { ec = impl_->setKeys(keys); } catch (const std::bad_alloc&) { ec = SrtpError::NoMemory; }
    if (ec) UAV_LOG_WARN << "[WebRTC][" << (impl_->rtcp ? "SRTCP" : "SRTP") << " " << impl_.get() << "][g=" << keys.generation
        << "] key installation rejected error=" << ec.value() << ":" << ec.message();
    return ec;
}
SrtpSession::ErrorCode SrtpSession::process(Bytes& packet, bool protect, std::optional<std::uint64_t> generation)
{ std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->process(packet, protect, generation); }
SrtpSession::ErrorCode SrtpSession::reset(std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->current.state == SrtpState::Closed) return SrtpError::Closed;
    if (generation <= impl_->current.generation) return SrtpError::GenerationMismatch;
    impl_->clearContexts(); impl_->currentKeys.reset(); impl_->current.profile.reset();
    impl_->current.state = SrtpState::New; impl_->current.generation = generation;
    impl_->current.overhead = impl_->current.maxPlaintextSize = 0;
    UAV_LOG_INFO << "[WebRTC][" << (impl_->rtcp ? "SRTCP" : "SRTP") << " " << impl_.get() << "][g=" << generation << "] reset media context";
    return {};
}
void SrtpSession::invalidate()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->current.state == SrtpState::Closed) return;
    UAV_LOG_WARN << "[WebRTC][" << (impl_->rtcp ? "SRTCP" : "SRTP") << " " << impl_.get() << "][g=" << impl_->current.generation << "] media context invalidated";
    impl_->clearContexts(); impl_->currentKeys.reset(); impl_->current.profile.reset();
    impl_->current.state = SrtpState::Failed;
    impl_->current.overhead = impl_->current.maxPlaintextSize = 0;
}
void SrtpSession::close()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->current.state != SrtpState::Closed)
        UAV_LOG_INFO << "[WebRTC][" << (impl_->rtcp ? "SRTCP" : "SRTP") << " " << impl_.get() << "][g=" << impl_->current.generation
            << "] closed protected=" << impl_->current.stats.protectedPackets << " unprotected=" << impl_->current.stats.unprotectedPackets
            << " authentication_failures=" << impl_->current.stats.authenticationFailures << " replay_failures=" << impl_->current.stats.replayFailures;
    impl_->clearContexts(); impl_->currentKeys.reset(); impl_->current.profile.reset(); impl_->keyHistory.clear();
    OPENSSL_cleanse(impl_->scratch.data(), impl_->scratch.size() * sizeof(impl_->scratch[0]));
    impl_->current.state = SrtpState::Closed; impl_->current.overhead = impl_->current.maxPlaintextSize = 0;
}
SrtpSnapshot SrtpSession::snapshot() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->current; }
} // namespace detail
} // namespace webrtc
