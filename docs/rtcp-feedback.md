# WebRTC RTCP 报告与反馈

`DtlsTransport` 在内置 SRTP/SRTCP 模式下拥有 `RtcpSession`，用独立 Asio timer 调度报告与反馈。`PeerConnection` 只转交 SDP 协商结果、发送请求和事件，媒体状态及密文缓存都属于下层 transport。

## PeerConnection 配置

```cpp
webrtc::PeerConnection::Config config;
config.dtls.enableSrtp = true;
// config.dtls.rtcp.enabled 默认 true；仅在有协商后的 RTP 能力时启动 timer。
config.dtls.rtcp.reportInterval = std::chrono::milliseconds(1000);
auto video = webrtc::PeerMedia::video("video", {{96, "VP8", 90000, 1, std::nullopt}});
video.feedback = {
    {std::nullopt, "nack"},
    {std::nullopt, "nack pli"},
    {std::nullopt, "ccm fir"},
    {std::nullopt, "transport-cc"}
};
video.extmaps = {{5, std::nullopt,
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", std::nullopt}};
config.media = {video};

webrtc::PeerConnection::Callbacks callbacks;
callbacks.onKeyFrameRequest = [](webrtc::RtcpKeyFrameEvent event) {
    // 通知对应 event.feedback.mediaSsrc 的编码器生成关键帧。
};
callbacks.onNack = [](webrtc::RtcpNackEvent event) {
    // 已自动重传缓存命中的包；cacheMisses 可用于判断是否需要新的关键帧。
};
callbacks.onReceiverReport = [](webrtc::RtcpReportEvent event) {
    // report 包含丢包、jitter、LSR/DLSR；rttMs 只在匹配本地 SR 时有值。
};
callbacks.onTwccFeedback = [](webrtc::TwccFeedbackEvent event) {
    // packets: transportSequence、ssrc、rtpSequence、wireSize、sentAt、remoteArrivalUs。
    // 上层可计算吞吐、延迟变化并调整码率。
};
callbacks.onTransportFeedback = [](webrtc::TransportPacketsFeedback feedback) {
    // libwebrtc 风格结果：sent_packet、receive_time、在途字节和接收排序。
    // 间隔用 TransportFeedbackDeltaCalculator 计算，或读取上述事件的 packetDeltas。
};
webrtc::PeerConnection peer(io, config, callbacks);
```

Offer/Answer 的共同能力决定是否启用 NACK、PLI、FIR、transport-cc，TWCC 扩展 ID 和方向使用 answer 的协商结果。没有协商 transport-cc 或没有相应方向的扩展 ID 时，不添加 TWCC 序号、不生成 TWCC 反馈。不具备对应反馈能力的包不会触发自动重传或关键帧事件。`snapshot().negotiatedMedia` 可查看反馈和扩展，`snapshot().dtls->rtcp` 可查看统计。

上层仍传入**完整的 RTP 明文包**，负责 RTP 时间戳、SSRC、媒体序号及 codec 分片。transport 在入队时预留 TWCC 扩展空间，在 [pacer](pacer.md) 出队时分配 TWCC 序号并保护；`asyncSendRtp` 完成回调返回调用者原始明文长度。需要关键帧时调用 `peer.requestPli(remoteMediaSsrc, done)`；远端 SSRC 必须已收到 RTP 且协商 `nack pli`。未知 SSRC、未协商和限频返回 `operation_not_supported`。

直接使用 `DtlsTransport` 时，通过 `config.rtcp.parameters` 或 `configureRtcp` 声明 PT、时钟频率、收发方向、反馈能力和 TWCC 扩展 ID。`config.rtcp.enabled = false` 可保留原有纯加解密/外部 RTCP 模式。

## 已实现行为

发送历史与 TWCC 关联由 [transport adapter](transport-adapter.md) 实现，参考固定版本的 libwebrtc 源码。包在提交 Sender 前记录时间；成功回调只更新 SR/缓存，失败移除记录。已有 `onTwccFeedback` 提供跨反馈的发送/接收/延迟变化间隔，`onTransportFeedback` 提供上游风格的逐包原始结果。

- 自动生成 SR/RR + SDES CNAME；SR 统计成功发送的原始媒体包与 payload 字节，投影 RTP 时间戳，RR 包含扩展最高序号、区间/累计丢包、jitter 和 LSR/DLSR。多个 SSRC 的报告按 RTCP 明文 MTU 分包。最近两次报告周期内有 RTP 发送时使用 SR，否则使用 RR。SR 与 RR 的周期默认 1 秒，可调整。
- 收到认证后的 RTP 序号缺口，等待默认 20 ms 的重排窗口再发 NACK；默认每 100 ms 重试，最多 5 次、2 秒过期。迟到包取消对应请求。只追踪有界缺口，过大的序号跳变须连续两个新序号确认后才重建接收统计。
- 收到 NACK 的 PID/BLP，自动重传 SSRC/序号匹配的**原始 SRTP 密文**，不再次调用 `protect`。缓存默认最多 2048 包、4 MiB、2 秒，重传次数和频率有限制。重传不增加原始 SR 的媒体包计数。缓存未命中通过 `onNack.cacheMisses` 提供。
- 接收 PLI/FIR 并触发 `onKeyFrameRequest`，FIR 去重、关键帧请求默认 300 ms 限频。编码器由业务层响应。`requestPli` 提供主动请求接口。
- 为协商的 RTP 包写入同一 BUNDLE association 的 16 位 transport 序号，保留已有 RFC 8285 扩展、CSRC、payload 和 RTP padding。支持 one-byte 与 two-byte 扩展；不自动把已有 one-byte 扩展切换成 two-byte，调用者需统一包格式。
- 接收端默认每 50 ms 自动生成 TWCC。处理 run-length、one-bit、two-bit 状态块以及 250 us 单位的 unsigned small / signed large delta。发送端将反馈对应到有界发送历史，处理 16 位序号、24 位 reference time 回绕、重复反馈和缺失包迟到后的状态更新。TWCC reference clock 属于远端，`remoteArrivalUs` **不能直接减本地 `sentAt` 算单向延迟**；可比较各自的时间差。`notReceived` 表示本次反馈时尚未收到，允许后续变为收到。
- 未协商 `rtcp-rsize` 时，反馈前附加 RR + SDES；周期报告始终为 compound RTCP。RTCP 经 SRTCP 加密，反馈只在认证、端点、component、generation 校验后处理。完整 compound 先校验再处理，畸形尾包不会触发前面的 NACK/PLI。
- ICE restart 清除报告、缺口、重传缓存、TWCC 历史和计数，重新建立 DTLS 密钥后恢复定时器；失败/关闭会停止自动输出。统计中的 sent* / retransmissions 表示引擎生成或安排的包数，UDP 输出失败会用现有 log 库记录，crypto 统计可查看实际保护次数。

同一个 BUNDLE transport 的相同 PT 必须有相同的媒体种类、时钟、方向和反馈能力，同一方向的 TWCC 扩展 ID 必须一致；冲突协商返回 `operation_not_supported`。这些约束避免未实现 MID/SSRC 路由时把不同媒体的反馈能力误用。TWCC 原始反馈和实际发送记录已接入下层 [GCC](goog-cc-network-control.md)，同时保留用户回调。PSFB FMT=15 的 REMB 支持解析/构造，匹配本地发送 SSRC 后作为码率上限并通过 `onRemoteBitrateReport` 通知用户；SR/RR 的 RTT/增量丢包也进入 GCC。编码端消费目标回调执行调速；RTX 封装尚待实现。原密文重传受接收端 SRTP replay window 约束，长延迟重传可在后续增加 RTX。

## 验证

`tests/webrtc/rtcp_session_test.cpp` 覆盖独立 wire vectors、畸形 compound/padding、NACK 回绕、报告丢包/jitter/RTT、限频与过期、MTU 分包、未协商反馈隔离、TWCC 序号/reference-time 回绕及重复/迟到反馈。`dtls_transport_test` 用真实 DTLS + libsrtp 验证丢包自动找回、重传密文一致、SR/RR、TWCC 和重启隔离。`srtp_ctl_test integration` 用真实 UDP ICE PeerConnection + DataChannel 验证 SDP 反馈/扩展、事件转交和 ICE restart。

协议格式依据：[RFC 3550](https://www.rfc-editor.org/rfc/rfc3550)、[RFC 4585](https://datatracker.ietf.org/doc/html/rfc4585)、[RFC 5104](https://www.rfc-editor.org/rfc/rfc5104)、[RFC 8285](https://www.rfc-editor.org/rfc/rfc8285)、[transport-wide CC draft](https://www.ietf.org/archive/id/draft-holmer-rmcat-transport-wide-cc-extensions-01.txt)。
