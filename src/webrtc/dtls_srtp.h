#pragma once

#include"common/udp_socket.h"

#include<cstdint>

namespace webrtc
{
// DTLS exporter 与媒体加密控制器共用的类型，不依赖具体 transport。
enum class DtlsSrtpProfile { AeadAes128Gcm, AeadAes256Gcm, Aes128CmSha1_80, Aes128CmSha1_32 };
const char* srtpProfileString(DtlsSrtpProfile profile);
struct DtlsSrtpKeyMaterial
{
    std::uint64_t generation{0};
    std::uint16_t component{1};
    DtlsSrtpProfile profile{DtlsSrtpProfile::Aes128CmSha1_80};
    UdpSocket::Bytes localMasterKey, localMasterSalt, remoteMasterKey, remoteMasterSalt;
};
} // namespace webrtc
