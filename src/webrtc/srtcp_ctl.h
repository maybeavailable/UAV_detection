#pragma once

#include"webrtc/srtp_ctl.h"

namespace webrtc
{
// 同步、线程安全的 RTCP -> SRTCP / SRTCP -> RTCP 控制器。
// 支持 compound/reduced-size RTCP；保护整个 UDP 负载，独立管理每个 SSRC 的 SRTCP index/重放状态。
// 使用与 SrtpCtl 相同的 DTLS master key/salt；libsrtp 通过不同 KDF label 分离 RTP/RTCP 密钥。
class SrtcpCtl
{
public:
    using Bytes = UdpSocket::Bytes;
    using ErrorCode = UdpSocket::ErrorCode;
    using Config = SrtpConfig;
    using Snapshot = SrtpSnapshot;
    SrtcpCtl();
    explicit SrtcpCtl(Config config);
    ~SrtcpCtl();
    SrtcpCtl(const SrtcpCtl&) = delete;
    SrtcpCtl& operator=(const SrtcpCtl&) = delete;
    ErrorCode setKeys(const DtlsSrtpKeyMaterial& keys);
    // 成功替换 packet，失败保留原报文；拒绝未加密（E=0）的 SRTCP。
    ErrorCode protect(Bytes& packet, std::optional<std::uint64_t> generation = std::nullopt);
    ErrorCode unprotect(Bytes& packet, std::optional<std::uint64_t> generation = std::nullopt);
    ErrorCode reset(std::uint64_t generation);
    void invalidate();
    void close();
    Snapshot snapshot() const;
private:
    std::unique_ptr<detail::SrtpSession> session_;
};
} // namespace webrtc
