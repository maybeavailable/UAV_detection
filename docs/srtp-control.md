# SRTP / SRTCP 控制器

`webrtc::SrtpCtl`（`srtp_ctl.h/.cpp`）负责完整 RTP UDP 负载与 SRTP 之间的转换，`webrtc::SrtcpCtl`（`srtcp_ctl.h/.cpp`）负责完整 RTCP UDP 负载与 SRTCP 之间的转换。两者共用内部 `srtp_session.h/.cpp`，通过 libsrtp2 完成密钥派生、加密、认证及重放保护。

两个对象独立拥有双向上下文，接口同步、内部互斥，可跨线程调用；网络发送由 `IceTransport` / `PeerConnection` 完成。RTP 封装、RTCP 报告/反馈的生成与业务处理由上层完成。

常规接入由下层 DtlsTransport 管理两个控制器：配置 `peerConfig.dtls.enableSrtp=true`，通过 `peerConfig.dtls.srtp` / `srtcp` 调整参数；使用 `peer.asyncSendRtp` / `asyncSendRtcp` 输入完整明文，接收 `onRtpPacket` / `onRtcpPacket` 已认证明文。指纹认证、exporter 密钥安装、ICE 换代及关闭由 transport 完成；PeerConnection 不直接持有 SRTP/SRTCP。配置示例见 [PeerConnection](peerconnection.md)。

## 接口

以下是独立控制器的外部接入方式，使用时保持 `peerConfig.dtls.enableSrtp=false`。

```cpp
webrtc::SrtpCtl rtp;
webrtc::SrtcpCtl rtcp;

auto ec = rtp.setKeys(dtlsKeys);
if (!ec) ec = rtcp.setKeys(dtlsKeys);
// 两个安装操作均成功后开放媒体；每个对象内部的双向密钥安装是原子的。

UdpSocket::Bytes packet = buildCompleteRtpPacket();
ec = rtp.protect(packet, dtlsKeys.generation);
if (!ec) peer.asyncSendSecurePacket(std::move(packet));

// onSecurePacket 收到的仍是密文：
ec = encrypted.type == webrtc::IcePacketType::Rtp
    ? rtp.unprotect(encrypted.datagram.payload, encrypted.generation)
    : rtcp.unprotect(encrypted.datagram.payload, encrypted.generation);
if (!ec) consumeAuthenticatedPacket(std::move(encrypted.datagram.payload));
```

`setKeys` 直接接收 `DtlsSrtpKeyMaterial`：发送使用 `localMasterKey/localMasterSalt`，接收使用 `remoteMasterKey/remoteMasterSalt`。不需要再根据 DTLS client/server 角色交换密钥，DTLS exporter 已完成方向转换。

`protect` / `unprotect` 成功时替换 vector，失败时保留调用者原始报文。接收端只交付经过认证的完整明文；认证失败、重复报文、重放窗口之外的报文、无效结构、超长报文及超出 SSRC 上限分别返回 `SrtpError`。错误码是项目使用的 `boost::system::error_code`，可直接与枚举比较：

```cpp
if (ec == webrtc::SrtpError::AuthenticationFailed) { /* 丢弃，不能消费报文 */ }
```

`Snapshot` 提供状态、generation、component、profile、密文额外长度、可接受的明文最大长度、双向 SSRC 数量和累计统计，不包含密钥。`protectedBytes` 统计输出密文字节，`unprotectedBytes` 统计交付的明文字节。

## Profile 与报文

| DTLS-SRTP profile | Master key / salt | SRTP tag | SRTCP tag / index | SRTP / SRTCP 额外长度 |
| --- | --- | --- | --- | --- |
| AES128_CM_SHA1_80 | 16 / 14 字节 | 10 字节 | 10 / 4 字节 | 10 / 14 字节 |
| AES128_CM_SHA1_32 | 16 / 14 字节 | 4 字节 | 10 / 4 字节 | 4 / 14 字节 |
| AEAD_AES_128_GCM | 16 / 12 字节 | 16 字节 | 16 / 4 字节 | 16 / 20 字节 |
| AEAD_AES_256_GCM | 32 / 12 字节 | 16 字节 | 16 / 4 字节 | 16 / 20 字节 |

AES-CM 的 `_32` profile 只缩短 RTP 的 tag，SRTCP 仍使用 80-bit 认证；GCM 的 SRTCP index 位于 tag 后。映射来自 [DTLS-SRTP（RFC 5764）](https://www.rfc-editor.org/rfc/rfc5764.html) 和 [AES-GCM SRTP（RFC 7714）](https://www.rfc-editor.org/rfc/rfc7714.html)，实际操作采用 libsrtp 的 profile API。

RTP 支持 CSRC、普通扩展头及 padding；这些头字段参与认证，payload 加密。RTCP 支持 compound 和 reduced-size 报文，整个 UDP 负载仅增加一个 SRTCP index/tag，除首部 8 字节外的内容加密。接收端要求 E=1。核心结构和 SR/RR 固定字段会校验，SDES/APP/反馈等负载的具体语义由 RTCP 解析器处理。当前未提供 MKI、SDES 密钥协商、RFC 6904 加密扩展头或 Cryptex 配置。

`Config::maxPacketSize` 是包含 tag/index 的最终 UDP 负载上限，默认 65507，可按路径 MTU 设置为 1200 等值。内部缓冲保证 libsrtp 所需的对齐及 trailer 空间。默认启用 RTCP mux，RTP PT 64..95 被拒绝；独立 RTP 传输可设置 `rtcpMux=false`。

## 生命周期

从 `PeerConnection::onSrtpKeys` 安装同一份密钥到两个对象。媒体收发前确认 PeerConnection 为 Connected，两个控制器为 Ready，且 generation 与 ICE 一致。

同代重复安装相同密钥幂等，不重置 ROC、SRTCP index 或重放窗口；同代不同密钥返回 `ConflictingKeys`。安装更高 generation 的新密钥会替换当前上下文并清空 SSRC 状态。双向密钥材料必须不同，并且不能重复使用本对象历史中任一方向已使用的 key+salt；密钥摘要历史受 `maxKeyGenerations` 限制。

下层 transport 的 [RtcpSession](rtcp-feedback.md) 使用这些控制器发送自动 SR/RR 和反馈，并在收到 NACK 时重用缓存的 SRTP 密文。

ICE generation 前进时，可先调用两个对象的 `reset(newGeneration)`，立即使旧上下文失效，再等待新 DTLS exporter 密钥。`reset` 只允许向前移动；`protect` / `unprotect` 的可选 generation 参数可拒绝延迟的旧代队列数据。关闭 PeerConnection 时调用两个对象的 `close()`，关闭幂等且不可重新开启。

`invalidate()` 用于当前传输失败：清除加密上下文、使状态进入 Failed，并保留 generation 与已用密钥历史。更高 generation 可 reset 并安装新密钥；旧密钥不能通过重新安装恢复。内置模式由 DtlsTransport 自动调用这些生命周期接口。

RTP 的同 SSRC/序号不允许再次加密。网络重传使用已生成的密文，或通过 RTX 分配新的 SSRC/序号。SRTCP index 由库独立产生。SSRC 状态在同一代保留，避免删除后重置发送计数器或接收重放窗口；新代密钥会清理。默认每方向最多 256 个 SSRC，未通过认证的 SSRC 不占额度。

libsrtp 在进程内初始化一次，控制器不调用全局 `srtp_shutdown()`；应用自己的 libsrtp 使用者也应保持全局运行时在控制器存活期间有效。

## 构建与验证

主工程已经通过 `SRTP::SRTP` 链接仓库中的 aarch64 libsrtp2，新增三个实现文件已加入主工程源文件列表。独立主机测试需要宿主架构的 libsrtp2，不能链接仓库中的 aarch64 库。例如安装系统 libsrtp2 后：

```sh
cmake -S tests/webrtc -B /tmp/uav-webrtc-tests -DCMAKE_BUILD_TYPE=Debug -DSRTP_ROOT=/usr -DUSRSCTP_ROOT=/path/to/native/usrsctp
cmake --build /tmp/uav-webrtc-tests -j4
ctest --test-dir /tmp/uav-webrtc-tests --output-on-failure
```

也可把 `SRTP_ROOT` 指向自行原生构建的 libsrtp2 前缀；AES-GCM 需要启用支持 GCM 的后端。本次主机验证使用仓库下载的 libsrtp 2.8.1 源码在临时目录构建，并保留板端库。

PeerConnection 现已链接 usrsctp，独立测试配置也需要其宿主库；构建方式见 [DataChannel](datachannel.md)。

新增测试包含 libSRTP 已知 SRTP 密文向量、四种 profile 的原生 API 双向互通、SRTP 序号回绕、SRTCP 独立索引、乱序与重放、篡改、失败时保留原报文、SSRC/报文边界、密钥幂等与历史复用拒绝、跨线程使用，以及真实 PeerConnection → DTLS exporter → SRTP/SRTCP → UDP 收发和 ICE restart。
