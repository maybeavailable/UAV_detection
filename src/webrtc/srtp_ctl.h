#pragma once

#include"webrtc/dtls_srtp.h"

#include<cstddef>
#include<cstdint>
#include<memory>
#include<optional>
#include<type_traits>

namespace webrtc
{
enum class SrtpError
{
    NotReady = 1, Closed, InvalidArgument, InvalidPacket, AuthenticationFailed,
    ReplayDetected, ReplayTooOld, KeyExpired, StreamLimit, PacketTooLarge,
    GenerationMismatch, ConflictingKeys, ReusedKey, KeyHistoryFull,
    UnsupportedProfile, CryptoFailure, NoMemory
};
const boost::system::error_category& srtpErrorCategory();
boost::system::error_code make_error_code(SrtpError error);
struct SrtpProfileInfo
{
    std::size_t masterKeySize{0}, masterSaltSize{0}, rtpAuthTagSize{0}, rtcpAuthTagSize{0};
    std::size_t rtpOverhead() const { return rtpAuthTagSize; } // 无 MKI
    std::size_t rtcpOverhead() const { return 4 + rtcpAuthTagSize; } // SRTCP index + tag
};
std::optional<SrtpProfileInfo> srtpProfileInfo(DtlsSrtpProfile profile);
enum class SrtpState { New, Ready, Failed, Closed };
struct SrtpConfig
{
    std::uint16_t component{1};
    std::size_t maxPacketSize{65507}; // 最终 UDP 负载上限，包含认证 tag 和 SRTCP index
    std::size_t maxStreams{256}; // 每方向、每代的 SSRC 上限；失败认证不占用额度
    std::size_t replayWindow{128}; // 64..32767；SRTCP 使用 libsrtp 的独立重放窗口
    std::size_t maxKeyGenerations{1024}; // 记录已使用密钥的摘要，拒绝跨代再次使用
    bool rtcpMux{true}; // SRTP 禁止 PT 64..95，避免 RTP/RTCP 分流冲突；不影响 SRTCP
};
struct SrtpStats
{
    std::uint64_t protectedPackets{0}, protectedBytes{0}, unprotectedPackets{0}, unprotectedBytes{0};
    std::uint64_t failedProtects{0}, failedUnprotects{0}, authenticationFailures{0}, replayFailures{0};
};
struct SrtpSnapshot
{
    SrtpState state{SrtpState::New};
    std::uint64_t generation{0};
    std::uint16_t component{1};
    std::optional<DtlsSrtpProfile> profile;
    std::size_t overhead{0}, maxPlaintextSize{0}, outboundStreams{0}, inboundStreams{0};
    SrtpStats stats; // 生命周期累计；不包含密钥
};
namespace detail { class SrtpSession; }

// 同步、线程安全的 RTP -> SRTP / SRTP -> RTP 控制器。内部使用 libsrtp2，两个方向独立。
// 仅消费完整 RTP UDP 负载，不封装 RTP、不发送网络数据；不支持 MKI/SDES/加密扩展头。
class SrtpCtl
{
public:
    using Bytes = UdpSocket::Bytes;
    using ErrorCode = UdpSocket::ErrorCode;
    using Config = SrtpConfig;
    using Snapshot = SrtpSnapshot;
    SrtpCtl();
    explicit SrtpCtl(Config config); // 无效配置抛 invalid_argument
    ~SrtpCtl();
    SrtpCtl(const SrtpCtl&) = delete;
    SrtpCtl& operator=(const SrtpCtl&) = delete;

    // localMasterKey/Salt 用于发送，remoteMasterKey/Salt 用于接收。原子安装，错误不替换已有上下文。
    // 同代相同密钥幂等并保留 ROC/重放状态；换代需要全新双向密钥，不能复用历史密钥。
    ErrorCode setKeys(const DtlsSrtpKeyMaterial& keys);
    // 成功替换 packet；失败保持 packet 不变。可指定来源 generation，拒绝延迟的旧代数据。
    ErrorCode protect(Bytes& packet, std::optional<std::uint64_t> generation = std::nullopt);
    ErrorCode unprotect(Bytes& packet, std::optional<std::uint64_t> generation = std::nullopt);
    // 发送的同 SSRC/序号不可重新加密；需要重传时缓存并重发已生成的密文，或使用新的 RTX 序号。
    ErrorCode reset(std::uint64_t generation); // 必须前进；立即清除当前上下文，保留已用密钥摘要
    void invalidate(); // 当前传输失效；清除上下文，保留 generation 和已用密钥历史
    void close(); // 幂等，关闭后不可再次安装密钥
    Snapshot snapshot() const;
private:
    std::unique_ptr<detail::SrtpSession> session_;
};
} // namespace webrtc

namespace boost { namespace system {
template<> struct is_error_code_enum<webrtc::SrtpError> : std::true_type {};
}} // namespace boost::system
