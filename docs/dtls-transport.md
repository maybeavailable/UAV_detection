# DTLS transport

`src/webrtc/dtlstransport.h/.cpp` 实现 `webrtc::DtlsTransport`，每个 ICE 组件对应一条 DTLS 1.2 association。使用 OpenSSL 进行握手、证书认证、record 加解密和 SRTP 密钥导出；启用 `Config::enableSrtp` 时，本层持有并管理 SrtpCtl/SrtcpCtl。网络仍由 `IceTransport` 管理，PeerConnection 只转发媒体接口。

证书指纹匹配遵循 [RFC 8122 §5](https://www.rfc-editor.org/rfc/rfc8122.html#section-5)，DTLS-SRTP exporter 使用 [RFC 5764 §4.2](https://www.rfc-editor.org/rfc/rfc5764.html#section-4.2)。握手重传通过 OpenSSL 的 `DTLSv1_get_timeout` / [`DTLSv1_handle_timeout`](https://docs.openssl.org/3.4/man3/DTLSv1_handle_timeout/) 接入 Asio timer。

## 与 ICE 接入

```cpp
#include "webrtc/dtlstransport.h"

boost::asio::io_context io;
auto identity = webrtc::DtlsCertificate::generate();
auto localFingerprint = identity->fingerprint(); // sha-256；写入本地 SDP 的 a=fingerprint

std::shared_ptr<webrtc::IceTransport> ice;
std::shared_ptr<webrtc::DtlsTransport> dtls;
webrtc::IceTransport::Callbacks iceCallbacks;
iceCallbacks.onDtlsReady = [&](webrtc::IceDtlsReady ready) {
    if (dtls) dtls->start(std::move(ready));
};
iceCallbacks.onPacket = [&](webrtc::IceTransportPacket packet) {
    if (dtls) dtls->handlePacket(std::move(packet));
    // 本层分流 DTLS record 和 SRTP/SRTCP，媒体不进入 SSL_read。
};
iceCallbacks.onState = [&](webrtc::IceTransportState) {
    if (dtls) dtls->handleIceState(ice->snapshot());
};

webrtc::IceTransport::Config iceConfig;
iceConfig.ice.role = webrtc::StunIceRole::Controlling;
// 根据运行环境设置 iceConfig.ice.bindings / stunServers。
ice = std::make_shared<webrtc::IceTransport>(io, iceConfig, iceCallbacks);

webrtc::DtlsTransport::Config dtlsConfig;
dtlsConfig.certificate = identity;
dtlsConfig.component = 1;
dtlsConfig.enableSrtp = true;
dtlsConfig.srtp.maxPacketSize = 1200;
dtlsConfig.srtcp.maxPacketSize = 1200;
// 仅媒体传输可设置 requireSrtp=true；纯 SCTP/DataChannel 无须 SRTP。
webrtc::DtlsTransport::Callbacks dtlsCallbacks;
dtlsCallbacks.onConnected = [&](webrtc::DtlsTransport::Snapshot connected) {
    // TLS Finished 和远端证书指纹都已验证，可启动 SCTP 等上层协议。
};
dtlsCallbacks.onData = [&](UdpSocket::Bytes plaintext) {
    // 已认证、解密的 DTLS application record，交给 SCTP 等模块。
};
dtlsCallbacks.onRtpPacket = [&](webrtc::IceTransportPacket packet) {
    consumeAuthenticatedRtp(std::move(packet.datagram.payload));
};
dtlsCallbacks.onRtcpPacket = [&](webrtc::IceTransportPacket packet) {
    consumeAuthenticatedRtcp(std::move(packet.datagram.payload));
};
dtls = std::make_shared<webrtc::DtlsTransport>(io,
    [weak = std::weak_ptr<webrtc::IceTransport>(ice)](UdpSocket::Bytes bytes, UdpSocket::SendHandler done) {
        if (auto transport = weak.lock())
            transport->asyncSend(std::move(bytes), 1, std::move(done));
        else if (done) done(boost::asio::error::operation_aborted, 0);
    }, dtlsConfig, dtlsCallbacks);

ice->gather();
// 交换包含 localFingerprint、ICE 凭据和候选的 SDP；导入远端描述。
// ice->setRemoteDescription(remoteSdp, "data", webrtc::SdpDescriptionType::Answer);
ice->start();
// Connected 后：dtls->asyncSend(std::move(sctpPacket), onSent);
// 媒体明文：dtls->asyncSendRtp(rtpPacket, onSent) / asyncSendRtcp(rtcpPacket, onSent)。
// 关闭时先 dtls->close(...)，完成后再 ice->close(...)，让 close_notify 有机会发出。
io.run();
```

示例中的回调引用需在整个会话期间存活。一个 PeerConnection 的多组件可共用同一 `DtlsCertificate`；每个 `DtlsTransport` 配置自己的 component，并由 Sender 转发到该 ICE 组件。DTLS 不替换 ICE 的回调，不拥有或关闭 ICE Socket。

## 证书、角色与认证

`DtlsCertificate::generate` 生成 ECDSA P-256 密钥、SHA256 签名的 X.509 自签名证书及随机 serial，默认有效期 30 天。`fromPem` 可导入匹配的 EC（至少 256 位）或 RSA（至少 2048 位）证书与私钥；加密私钥需由调用方解密后提供。`certificatePem` 仅导出证书，`fingerprint` 对证书 DER 求摘要，不返回私钥。

本地 SDP 使用本地证书的 fingerprint；对端 SDP 的 fingerprint 通过 `IceDtlsReady.parameters` 进入握手验证。`localSetup=Active` 为 DTLS client，`Passive` 为 server，与 ICE controlling/controlled 角色无关。双方都要求对端发送证书，OpenSSL 验证 CertificateVerify/Finished，模块校验证书摘要；自签名证书的身份依据来自信令的 fingerprint，不使用 CN/主机名或公共 CA 信任链。

默认摘要偏好为 SHA512、SHA384、SHA256、SHA224。选择远端提供的最优受支持算法后，仅接受该算法集合中的匹配证书，不会因较强摘要不匹配而回退到较弱摘要。未知算法忽略，若没有支持的算法则拒绝启动。SHA1 默认禁用，可通过 `allowSha1Fingerprint` 显式兼容。摘要通过 constant-time 比较。

`onConnected`、`onData` 和 `onSrtpKeys` 均在指纹认证与 TLS 握手完成后才可触发。`start` 的完成回调只表示握手已启动；握手失败、超时和网络错误由状态及 `onError` 通知。指纹不匹配会发送 OpenSSL 生成的 fatal alert 并进入 `Failed`。

## 数据、MTU 与重传

使用自定义 datagram BIO 保留 UDP 报文边界；一个 `BIO_read` 只消费一个数据报，一个 `BIO_write` 对应一份完整输出负载。Sender 必须保持调用顺序、恰好完成一次回调，不得拆分或拼接报文；允许同步完成。模块将完成事件重新投递到 strand，避免同步重入 OpenSSL。

默认输出 UDP payload MTU 为 1200 字节，握手消息由 OpenSSL 分片。应用发送以 `snapshot().maxPlaintextSize` 为准，该值取配置上限和 `DTLS_get_data_mtu` 的较小值。一条应用消息编码为一个 record，不做应用层分片，超过上限返回 `message_size`，空消息返回 `invalid_argument`。SCTP 等上层协议负责自己的分片与可靠性。

未 Connected 时发送返回 `not_connected`。默认发送队列最多 256 条明文消息、2MiB；输出 BIO 和在途加密报文也受配置限额保护。握手默认最多等待 15 秒，期间按 OpenSSL 的 DTLS timer 重传 flight。DTLS application data 不保证可靠交付，篡改/重放 record 由 OpenSSL 的认证及重放窗口处理。

`handlePacket` 校验组件、generation、包类型和就绪候选对的远端端点。可缓冲早于 `start` 到达的 DTLS 包，默认最多 128 个、512KiB；开始握手后只使用与实际对端匹配的缓冲数据。媒体要求当前 generation、已 Connected 且来源匹配，不提前缓冲；未经认证的媒体不能触发换代。

## SRTP 与其他 exporter

支持 `SRTP_AEAD_AES_128_GCM`、`SRTP_AEAD_AES_256_GCM`、`SRTP_AES128_CM_SHA1_80` 和 `SRTP_AES128_CM_SHA1_32`，默认提供 AES128 GCM 与 AES128 CM SHA1 80。`requireSrtp=false` 允许纯 SCTP/DataChannel 连接不协商 SRTP；设为 true 时，没有协商结果会拒绝 Connected。

协商后使用 `EXTRACTOR-dtls_srtp` 标签、无 context 的 TLS exporter，按 client key、server key、client salt、server salt 的顺序拆分，再根据本地 DTLS 角色转换为 local/remote 方向。CM profile 的 key/salt 为 16/14 字节，AES128 GCM 为 16/12，AES256 GCM 为 32/12。密钥不放入 Snapshot；`onSrtpKeys` 仍可用于外部控制器或 exporter 集成。

`enableSrtp=true` 时，SrtpCtl/SrtcpCtl 在 transport 内创建，采用 `Config::srtp` / `Config::srtcp`。两个控制器须使用本 transport 的 component 并启用 RTCP mux。指纹和 TLS Finished 验证通过后安装两套双向上下文，全部成功才通知 Connected；安装失败会使两套上下文失效并进入 Failed。纯 DataChannel 未协商 SRTP profile 时仍可 Connected，媒体接口返回 NotReady。

`asyncSendRtp` / `asyncSendRtcp` 输入完整明文，保护后直接经 Sender 发送；完成字节数为明文长度。带 generation 的重载可拒绝旧代发送请求。输出媒体包与 DTLS application 消息共用有界发送额度，底层回调未归还的媒体包仍占额度，防止在途队列无限增长。媒体 MTU 通过控制器的 `maxPacketSize` 设置，包含 tag/index；不会经过 DTLS record 二次封装。

`onRtpPacket` / `onRtcpPacket` 只交付通过认证和重放检查的完整明文，并保留组件、来源和 generation。认证失败、重放、非法包、错误来源或旧代媒体被丢弃，通过 `Snapshot::srtp/srtcp` 和 `stats.discardedMediaPackets` 查看计数。控制器配置与边界见 [SRTP/SRTCP](srtp-control.md)。

默认 `enableSrtp=false` 保留外部媒体保护模式；该模式下调用本 transport 的媒体明文发送接口返回 `operation_not_supported`。Sender 在内置模式下应使用 ICE 的通用 `asyncSend`，让完整 SRTP/SRTCP 直接进入 UDP。

`exportKeyingMaterial` 提供其他协议的异步 RFC 5705 exporter，只有 Connected 后可用。`nullopt` 表示没有 context，空 Bytes 表示存在一个零长度 context，两者在 DTLS 1.2 中不同。长度、标签及保留标签由接口和 OpenSSL 验证。

## Restart 与关闭

将 ICE 状态快照转发给 `handleIceState`：新的 generation 清除旧 SSL、指纹认证、协商结果和待发送应用数据；ICE Failed 使 DTLS 失效，ICE Closed 关闭 DTLS。新的 `onDtlsReady` 启动新的 association。本地证书可复用，session cache/ticket/resumption 和 renegotiation 已禁用，每次重新握手生成新会话密钥。

内置 SRTP/SRTCP 在换代时 reset 并保留已用密钥历史，失败时 invalidate，关闭时 close。普通重协商不重新安装密钥，不重置 ROC、SRTCP index 或重放窗口；旧代在途媒体的完成回调只归还旧请求，不改变新会话。已提交但未完成的 Sender 回调仍须完成一次，transport 的 close 会等待它们。

`reset` 只能前进到新的 generation；旧代报文和迟到发送完成不能驱动新会话。同一代重复且参数相同的 `start` 为幂等，改变角色/身份/端点需要新 generation。Failed 后不能重用同代；Closed 对象不可重开。

`close` 取消待发送明文、尽力发送 close_notify，并等待已提交加密数据的完成回调，不等待 UDP 对端确认关闭。不会关闭底层 ICE。所有 API 可跨线程投递，SSL/BIO/timer 和用户回调由内部 strand 串行处理；回调不得抛异常，io_context 必须存活且继续运行到关闭完成。

## 验证

```bash
cmake -S tests/webrtc -B /tmp/uav-webrtc-build -DCMAKE_BUILD_TYPE=Debug -DSRTP_ROOT=/usr -DUSRSCTP_ROOT=/path/to/native/usrsctp
cmake --build /tmp/uav-webrtc-build --parallel 4
ctest --test-dir /tmp/uav-webrtc-build --output-on-failure
```

测试覆盖独立 X.509 摘要校验、PEM 导入及私钥不匹配、client/server 握手、首包丢失重传、错误指纹/最强算法不降级、多证书指纹备选、SHA1 默认拒绝、双向加密消息、record 篡改与重放、四种 SRTP profile 和 RFC exporter 字节布局、无 SRTP 的 DataChannel 场景、队列/MTU 边界、超时、generation 重置、关闭，以及真实 ICE/UDP 通道上的 SDP 指纹认证、数据收发和 ICE restart 后重新握手。

新增验证覆盖 transport 内管理的四种 SRTP profile、RTP/SRTCP 收发、明文长度回调、错误来源、认证失败、重放、旧代报文、SSRC/MTU 上限、双方密钥安装失败、关闭等待在途媒体，以及真实 PeerConnection 上 DataChannel 与媒体共存及 ICE restart。

主工程链接 `OpenSSL::SSL` / `OpenSSL::Crypto` / `SRTP::SRTP`；独立测试使用宿主库。实现使用 DTLS 1.2 和 OpenSSL 3.0+ 接口，不实现 DTLS 1.3；SCTP/DCEP 由上层 DataChannelTransport 实现。
