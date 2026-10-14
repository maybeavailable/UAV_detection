#pragma once

#include"webrtc/srtp_ctl.h"

// 两个控制器共用的内部实现，不暴露 libsrtp 对象或全局初始化/关闭接口。
namespace webrtc { namespace detail {
class SrtpSession
{
public:
    using ErrorCode = UdpSocket::ErrorCode;
    using Bytes = UdpSocket::Bytes;
    SrtpSession(SrtpConfig config, bool rtcp);
    ~SrtpSession();
    ErrorCode setKeys(const DtlsSrtpKeyMaterial& keys);
    ErrorCode process(Bytes& packet, bool protect, std::optional<std::uint64_t> generation);
    ErrorCode reset(std::uint64_t generation);
    void invalidate();
    void close();
    SrtpSnapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}} // namespace webrtc::detail
