# PeerConnection

`webrtc::PeerConnection` 将现有 SDP、IceTransport、DtlsTransport 接到一个 C++17 异步对象中。实现位于 `src/webrtc/peerconnection.h/.cpp`，主工程和独立 WebRTC 测试工程均已加入源文件。

关键协商、传输状态和异常路径使用现有日志库，级别、内容及配置见 [WebRTC 日志](webrtc-logging.md)。

## 能力与边界

- Offer/Answer、Stable/HaveLocalOffer/HaveRemoteOffer 状态及 pending/current 描述；普通重协商保留 MID、m-line 顺序及现有 DTLS association。
- 显式配置 audio/video/application 能力；answer 采用 offer 的 payload type，协商方向、RTX apt、RTCP feedback 与 RTP header extensions。不支持的媒体以 port=0 拒绝。
- 单个 UDP ICE transport，max-bundle、RTCP mux、IPv4/IPv6；多个独立 transport 或 SCTP association 返回 `operation_not_supported`。
- 自动生成 ICE 凭据和 ECDSA 证书，也可复用传入的 `DtlsCertificate`。生成 SDP 包含真实证书 fingerprint，DTLS 验证对端 SDP fingerprint 后才能进入 Connected。
- Trickle ICE，支持候选早于 SDP、结束标记、MID/index 检查、外部 mDNS 解析端点，以及按 ufrag 隔离 ICE restart 前后的候选。外部解析结果优先于 SDP 中的同一 hostname 候选；结束标记等待已收到候选解析完成，解析失败的原候选仍可携带解析端点重试。
- 双方均可发起 ICE restart；在最终 answer 应用时更新传输。pending offer 可 `rollback()`，当前连接在协商成功前继续可用。
- 线程安全快照、内部 strand、操作/候选队列上限、关闭取消和发送完成回调。回调不得抛异常；调用者持续运行并保持 `io_context` 存活，直到 `close` 完成。

PeerConnection 持有并管理 [DataChannel](datachannel.md)，通过 `createDataChannel`、`onDataChannel` 提供消息收发；SRTP/SRTCP 控制器由下层 [DtlsTransport](dtls-transport.md) 持有，负责密钥安装、加解密、重放保护和生命周期。PeerConnection 转发媒体接口，不直接拥有加密控制器。RTP 封装、媒体编解码及信令服务器由上层实现。

启用内置 SRTP 后，下层还管理 [自动 RTCP 报告与反馈](rtcp-feedback.md)：SR/RR、NACK 密文重传、PLI/FIR 关键帧请求和 TWCC。PeerConnection 按 SDP 配置 PT、时钟、反馈能力和扩展 ID，并转交相关事件与 `requestPli` 请求。

内置 RTP 默认经下层 [pacer](pacer.md) 节奏发送；通过 `config.dtls.pacer` 设置初始预算/队列，并用 `setPacingRate` / `setPacingRates` / `setPacingPaused` 转交发送端估计器的媒体/padding 速率和暂停状态。

配置分层：`config.enableDataChannels` / `config.dataChannel` 配置 DataChannel；`config.dtls.enableSrtp` / `config.dtls.srtp` / `config.dtls.srtcp` 配置下层媒体保护。两个启用开关默认 false，保留原有外部 SCTP 和外部 SRTP 接口；推荐常规应用启用内置模块。内置媒体模式使用 `asyncSendRtp` / `asyncSendRtcp` 输入完整明文，由 `onRtpPacket` / `onRtcpPacket` 接收已认证的明文。配置、状态与统计均沿 transport 分层，媒体统计位于 `snapshot().dtls->srtp/srtcp`。

编码能力必须与实际编解码器一致。H264 保守要求 `packetization-mode` 与 `profile-level-id` 相同，VP9 要求 `profile-id` 相同；未实现 codec 参数专用协商的编码要求 fmtp 相同。不提供完整 H264 profile/level 兼容性推导或 simulcast 协商。拒绝后的 m-line 保留原 MID；可用 `addMedia` 添加新的 RTP m-line。当前不支持 pranswer、自动处理 offer glare、多个 BUNDLE 组或已拒绝槽位的重新启用。

Offer 的 setup 使用 actpass；同一 ICE generation 内保留已建立的 DTLS 角色与远端指纹，改变身份/角色需要重新建立传输。`onSrtpKeys` 在该 generation 首次协商 RTP 后交付一次；纯 SCTP 先建立连接、随后增加 RTP 时也会交付已导出的密钥。密钥不进入 Snapshot，调用者应避免记录密钥、ICE 密码或完整敏感 SDP。若现有 DTLS 没有协商出 SRTP profile，新增 RTP 需要 ICE restart 来重新握手。

## 接入示例

```cpp
#include "webrtc/peerconnection.h"

boost::asio::io_context io;
webrtc::PeerConnection::Config config;
config.enableDataChannels = true;
config.dataChannel.maxMessageSize = 65536;
config.dataChannel.maxChannels = 128;
config.dtls.enableSrtp = true;
config.dtls.srtp.maxPacketSize = 1200;  // 最终 SRTP UDP 负载上限
config.dtls.srtcp.maxPacketSize = 1200; // 包含 SRTCP index/tag
config.media = {
    webrtc::PeerMedia::audio("audio", {{111, "opus", 48000, 2, "minptime=10;useinbandfec=1"}}),
    webrtc::PeerMedia::video("video", {{96, "VP8", 90000, 1, std::nullopt}}),
    webrtc::PeerMedia::data("data")
};
// config.ice.ice.stunServers = {已解析的 STUN UDP endpoint};
// config.dtls.certificate = 可复用的本地证书;

webrtc::PeerConnection::Callbacks callbacks;
callbacks.onIceCandidate = [&](webrtc::PeerIceCandidate candidate) {
    sendCandidateThroughSignaling(candidate); // 包括空 candidate 结束标记及 usernameFragment
};
callbacks.onConnectionState = [&](webrtc::PeerConnectionState state) {
    updateConnectionStatus(state);
};
callbacks.onDataChannel = [&](std::shared_ptr<webrtc::DataChannel> channel) {
    webrtc::DataChannel::Callbacks handlers;
    handlers.onMessage = [](webrtc::DataChannelMessage message) {
        consumeDataMessage(std::move(message));
    };
    channel->setCallbacks(std::move(handlers));
};
callbacks.onRtpPacket = [&](webrtc::IceTransportPacket packet) {
    consumeRtp(std::move(packet.datagram.payload));
};
callbacks.onRtcpPacket = [&](webrtc::IceTransportPacket packet) {
    consumeRtcp(std::move(packet.datagram.payload));
};
callbacks.onError = [&](auto error, std::string message) {
    reportError(error, message);
};
webrtc::PeerConnection peer(io, config, callbacks);

peer.createOffer([&](auto error, auto description) {
    if (error) { reportError(error, "createOffer"); return; }
    const auto offer = *description;
    peer.setLocalDescription(offer, [&, offer](auto result) {
        if (result) { reportError(result, "setLocalDescription"); return; }
        sendSdpThroughSignaling(offer.type, offer.toSdp());
    });
});
io.run();
```

示例中的信令、消息消费和媒体处理函数由业务层提供。收到 answer 后调用 `setRemoteDescription(SdpDescriptionType::Answer, sdp, done)`；收到候选调用 `addIceCandidate(candidate, done)`。收到 offer 的一方先 `setRemoteDescription(Offer, ...)`，完成后 `createAnswer` → `setLocalDescription(answer)` → 通过信令发送 answer。完成回调表示该信令操作已应用；连接可用以 `onConnectionState(Connected)` 为准。

Connected 后调用 `peer.asyncSendRtp(完整RTP明文, done)` / `peer.asyncSendRtcp(完整RTCP明文, done)`，完成回调的字节数是原始明文长度。控制器在 DTLS Connected 通知前完成双向密钥安装；普通重协商保留上下文和重放窗口，ICE restart 清除旧代上下文并等待新握手。

外部 SCTP 模式保持 `enableDataChannels=false`，通过 `onDtlsData`/`asyncSendDtls` 接入；外部 SRTP 模式保持 `config.dtls.enableSrtp=false`，通过 `onSrtpKeys` 配置自己的控制器，使用 `onSecurePacket`/`asyncSendSecurePacket` 处理密文。内置模式不再调用对应的 raw 接收回调；`asyncSendSecurePacket` 仍可用于发送缓存的媒体密文。请勿把已加密媒体传入 `asyncSendRtp`/`asyncSendRtcp`。

主动调用 `createDataChannel` 也会启用内置 SCTP。DataChannel 的消息回调在独立的 SCTP strand 上运行，PeerConnection 的媒体回调经转发后在 PeerConnection strand 上运行；详细用法及背压语义见 [DataChannel](datachannel.md)。

首次应用本地或远端 offer 后自动收集候选。`createOffer` 自身不打开 UDP Socket，也不改变信令状态。使用非 trickle 信令时，应用本地描述后等待 `onGatheringState(Complete)`，再读取 `snapshot().pendingLocalDescription`（或已提交的 `currentLocalDescription`）发送完整 SDP。

重启通过 `createOffer({true}, done)` 发起，后续流程与普通 offer 相同。新的凭据保存在生成/pending 描述中；`Snapshot::localCredentials` 表示已提交的本地凭据，传输 generation 在最终 answer 应用后递增。候选回调自带所属描述的凭据，信令应完整转发。

关闭调用 `peer.close(done)`，内部依次关闭 DataChannel、DTLS 和 ICE，所有已接受的发送回调先于 close 完成回调。关闭后的信令操作返回 `operation_aborted`，重复 close 成功。

## 验证

```sh
cmake -S tests/webrtc -B /tmp/uav-ice-candidate-build -DCMAKE_BUILD_TYPE=Debug -DSRTP_ROOT=/usr -DUSRSCTP_ROOT=/path/to/native/usrsctp
cmake --build /tmp/uav-ice-candidate-build -j4
ctest --test-dir /tmp/uav-ice-candidate-build --output-on-failure
```

`peerconnection_test` 使用真实本地 UDP 双端验证 IPv4/IPv6、trickle/完整 SDP 候选、指纹验证、DTLS 明文收发、SRTP 密钥匹配及密文转发、能力协商、普通重协商、双向 ICE restart、回滚、无效状态/描述/候选、队列边界与跨线程关闭。尚未执行浏览器互通测试。

设计参考 [JSEP（RFC 8829）](https://www.rfc-editor.org/rfc/rfc8829.html)、[Offer/Answer（RFC 3264）](https://www.rfc-editor.org/rfc/rfc3264.html)、[BUNDLE（RFC 8843）](https://www.rfc-editor.org/rfc/rfc8843.html) 与 [RTP Header Extensions（RFC 8285）](https://www.rfc-editor.org/rfc/rfc8285.html)。实现范围以上述接口与边界为准。
