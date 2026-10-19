# Transport feedback adapter

`src/webrtc/transport_adapter.h/.cpp` 移植 libwebrtc `TransportFeedbackAdapter` 的 TWCC 路径，固定参考版本为 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`。类名保留 `TransportFeedbackAdapter`，同时提供 `TransportAdapter` 别名。

源码依据：[adapter.cc](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/rtp/transport_feedback_adapter.cc)、[adapter.h](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/rtp/transport_feedback_adapter.h)、[network_types.h](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/api/transport/network_types.h) / [network_types.cc](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/api/transport/network_types.cc)。许可证及专利声明保留在 `third_party/webrtc/LICENSE`、`PATENTS`。

## 数据和调用顺序

保留上游的三个入口：

1. `AddPacket(info, pacingInfo, overheadBytes, creationTime)` 登记 transport sequence、SSRC、RTP sequence、大小、音频/重传和 probe 元数据，尚未记录发送时间。
2. `ProcessSentPacket(info)` 记录提交发送的时间，首次发送计入该 route 的在途字节，返回 `SentPacket`。重复发送更新发送时间，不重复计费，并标记接收时间歧义。不参与反馈但参与带宽分配的包通过 `packet_id=-1` / `included_in_allocation` 累加，挂到下一个跟踪包的 `prior_unacked_data`。
3. `ProcessTransportFeedback(decodedTwcc, feedbackReceiveTime)` 将 TWCC 关联成 `TransportPacketsFeedback`。`ProcessRtcp` 可直接输入完整 RTCP 明文，整个 compound 验证成功后才处理 TWCC；格式失败时输出参数保持不变。仅允许输入已经过认证的 RTCP。

`SentPacket` 保留 `send_time`、`size`、`sequence_number`、`prior_unacked_data`、`data_in_flight`、`audio`、`pacing_info`。`PacketResult` 包含 `sent_packet`、`receive_time`、RTP 标识与接收时间歧义标记。`TransportPacketsFeedback` 提供 `ReceivedWithSendInfo()`、`LostWithSendInfo()`、`PacketsWithFeedback()`、`SortedByReceiveTime()`；接收排序依次比较接收时间、发送时间、transport sequence。

本项目使用 C++17 / Asio 类型：时间为 `steady_clock::time_point`，字节为 `size_t`，上游无穷时间哨兵用 `optional` 表示。`send_time=nullopt` 表示未发送，`receive_time=nullopt` 表示此次反馈时尚未收到，允许以后恢复。

16 位序号展开成 64 位序号；24 位 reference time 按最近周期展开。接收时间沿用上游的显示基准：首次反馈到达时建立本地 offset，随后累加远端 reference/delta，`receive_time` 按上游取到毫秒。额外的 `remote_arrival_us` 保留远端 250 µs 精度。这个 offset 不是时钟同步，**不能用 `receive_time - send_time` 或远端原始时间减发送时间当作单向延迟**。

在途字节按 route 分别跟踪。反馈前沿推进时，覆盖到的包退出在途计数，包括尚未收到的包；后者继续保留发送历史，允许迟到确认。已收到的包立即移除历史，所以重复确认不会再次输出。原始结果允许重复未收到状态，通过 `reported_lost_for_the_first_time` / `reported_recovered_for_the_first_time` 区分状态变化。

## transport 集成

`DtlsTransport -> RtcpSession -> TransportFeedbackAdapter` 管理跟踪状态，PeerConnection 只转交事件。协商了发送方向 `transport-cc` 和 TWCC 扩展的 RTP 均参与跟踪，包含音频、视频、padding；按完整 SRTP UDP payload 加配置的 `config.dtls.pacer.transportOverheadBytes` 计费。

pacer 出队、分配 TWCC、加密后，提交 Sender **之前**登记和记录发送时间。该时间是用户态提交时间，排除了 pacer 等待和加密耗时，尚未使用内核发包时间戳。Sender 成功回调只更新 SR 统计和缓存，不重新登记历史。因此反馈先于发送完成回调到达时也能关联时间。发送失败移除历史和在途计数；未提交的排队超时/取消包不建立发送记录。

本地发送请求及缓存重传持有展开后的 64 位 transport id。`RemovePacket` / `MarkRetransmitted` 使用这个完整 id，不会将很晚的失败或重传操作误关联到下一轮相同的 16 位 wire sequence。直接调用 `RtcpSession` 时，应将 `sendingRtp()` 返回的 id 传给后续的成功、失败或重传接口。

缓存 NACK 重传沿用原 transport sequence：更新发送时间、标记 `ambiguous_receive_time`，不重复增加在途字节。对端收到哪次传输无法区分，间隔计算会跳过这些包。独立 RTX 可通过 `RtpPacketSendInfo::original_ssrc` 声明独立 SSRC，避免误标；本模块不生成 RTX。

直接使用 adapter 时，历史窗口默认 60 秒，与上游一致；项目另加默认 8192 条、最多 32768 条的容量限制。`RtcpSession` 沿用 `historyAge` / `maxTwccHistory`，默认 2 秒和 8192 条。到期/容量淘汰同步释放在途字节。`Clear()` 清空历史，保留序号、远端时钟连续性及计数；`Reset(generation)` 用于 ICE restart，清空序号、时钟和计数。`SetNetworkRoute` 只切换 route，独立调用者需同时重置自己的间隔/估计器状态。

## 获取包间隔

```cpp
webrtc::TransportFeedbackDeltaCalculator intervals;
webrtc::PeerConnection::Callbacks callbacks;
callbacks.onTransportFeedback = [&](webrtc::TransportPacketsFeedback feedback) {
    for (const auto& packet : feedback.SortedByReceiveTime()) {
        // packet.sent_packet.send_time：发送端提交时间
        // packet.receive_time：上游方式重建的接收时间
        // packet.remote_arrival_us：远端原始时间，保留 250 us 精度
        // packet.sent_packet.size：计费字节，含配置的额外开销
    }
    for (const auto& delta : intervals.Calculate(feedback)) {
        // send_delta = S(i) - S(i-1)
        // receive_delta = A(i) - A(i-1)
        // delay_delta = receive_delta - send_delta
        // 类型均为 chrono::microseconds，可交给后续延迟趋势/码率估计。
    }
};
```

`TransportFeedbackDeltaCalculator` 是上游 adapter 以外的独立便利层，提供逐包间隔。它保存跨反馈的前一个有效包，按接收顺序遍历，跳过未收到、旧序号、发送/接收时间倒退和歧义重传。优先使用远端原始时间差，保留 250 µs 精度；generation 变化自动重置。没有前包时不伪造零间隔。[InterArrivalDelta](inter-arrival-delta.md) 单独实现上游 GCC 的发送包分组及组间差值，可直接消费排序后的反馈。

兼容已有 `onTwccFeedback`，额外提供 `packetDeltas`、`feedbackAt` 和逐包 `ambiguousReceiveTime`，不必自行持有 calculator。兼容事件仍只报告状态变化，字节仍是 SRTP UDP payload；原始 `onTransportFeedback` 遵循上游重复未收到结果语义，二者不要作为两份独立反馈重复送入同一个估计器。

`snapshot().dtls->rtcp->transport` 提供登记/发送/重传数、历史大小、当前 route 在途字节、未知查找、route 过滤、到期、本地失败计数。反馈批次使用项目现有日志库，不输出媒体或密钥。

## 范围与验证

adapter 移植 TWCC 路径，未移植上游 RFC 8888 / ECN，也不实现 GCC、probe 生成或自动调速。[确认吞吐估计器](bitrate-estimator.md) 已在 `congestion_control` 中实现，可消费 `SortedByReceiveTime()`；后续带宽控制器再结合这些输入，通过已有 pacer 速率接口控制发送。

`transport_adapter_test` 覆盖登记/发送分离、在途计费、跨批间隔、丢失/恢复、未知包 delta 累加、序号/reference 回绕、负 delta、重排、重复发送、重传歧义、route、过期/容量、失败移除及畸形 compound 原子性。`rtcp_session_test` 验证反馈早于成功回调和兼容事件；`dtls_transport_test` 验证真实加密反馈先于 Sender 完成、本地失败、pacer/TWCC 时序和重启；`srtp_ctl_test integration` 验证 PeerConnection 转交和代次。
