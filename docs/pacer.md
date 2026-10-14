# WebRTC RTP pacer

`src/webrtc/congestion_control/pacer.h/.cpp` 提供同步调度器 `webrtc::Pacer`，采用 libwebrtc 的周期预算模式：独立的 `IntervalBudget` 管理媒体和 padding 预算，Pacer 管理优先级队列与放行周期。它保存有界的包元数据队列，由 `DtlsTransport` 的 strand 和独立 Asio `steady_timer` 驱动。媒体 payload、加密状态、发送回调和生命周期由下层 transport 持有；`PeerConnection` 转交配置/API，不拥有 pacer。

## 配置与发送端带宽估计接入

```cpp
webrtc::PeerConnection::Config config;
// config.media、callbacks 与 io 按已有媒体协商/收发逻辑提供。
config.dtls.enableSrtp = true;
config.dtls.pacer.enabled = true; // 内置媒体模式默认启用
config.dtls.pacer.rateBps = 3000000; // 初始 RTP pacing rate，单位 bit/s
config.dtls.pacer.paddingRateBps = 0; // padding 默认关闭；需由调用者提供合法的 padding RTP
config.dtls.pacer.processInterval = std::chrono::milliseconds(5);
config.dtls.pacer.drainLargeQueues = true;
config.dtls.pacer.queueTimeLimit = std::chrono::milliseconds(200); // 示例软目标；默认 2000 ms
config.dtls.pacer.paceAudio = true;
config.dtls.pacer.accountForAudio = true;
config.dtls.pacer.maxQueueDelay = std::chrono::milliseconds(500);
config.dtls.pacer.maxQueuePackets = 256;
config.dtls.pacer.maxQueueBytes = 2 * 1024 * 1024;
// 若按链路层预算计费，可显式加上 UDP/IP/中继开销，避免对 IPv4/IPv6 作固定假设。
config.dtls.pacer.transportOverheadBytes = 0;

webrtc::PeerConnection peer(io, config, callbacks);
// 发送端估计器给出新的 pacing rate 后：
peer.setPacingRate(4500000, [](auto ec) { /* 检查配置结果 */ });
// 若估计器同时指定媒体和 padding 目标，可原子更新两个速率：
peer.setPacingRates(4500000, 500000, [](auto ec) { /* 检查配置结果 */ });
// 暂停/恢复 RTP，RTCP 与 DTLS/SCTP 仍可发送：
peer.setPacingPaused(true);
peer.setPacingPaused(false);
// rate=0 也暂停释放 RTP；设置为正数后恢复，独立的 paused 开关仍需解除。
peer.setPacingRate(0);
```

两个速率都支持 0 或 1000..1,000,000,000 bit/s，非法更新返回 `invalid_argument`，两个原速率均保留。有效 padding 速率为 `min(rateBps, paddingRateBps)`；`setPacingRate` 保留请求的 padding 目标，后续媒体速率恢复时重新计算上限。pacing rate 是 RTP/重传的发送预算，与编码器的目标码率分别设置。当前模块接受外部估计器的速率，不自行运行 GCC/BWE，也不自动调整编码器。

原 `burstInterval` 配置改为 `processInterval`（1..30 ms）；`maxBurstBytes` 现在限制一个处理周期放行的字节数，不再表示正信用上限。

`config.dtls.pacer.enabled=false` 保留直接媒体发送路径；未启用内置 SRTP 或关闭 pacer 时，动态配置接口返回 `operation_not_supported`。失败/关闭后接口返回 `operation_aborted`。直接使用 `DtlsTransport` 时可使用相同配置及 `setPacingRate` / `setPacingRates` / `setPacingPaused`。

## 调度规则

- 预算按完整 SRTP UDP 负载计费，包含 RTP 固定头、扩展头、padding、SRTP tag；重传也计费。可叠加 `transportOverheadBytes`。RTCP、DTLS 和 DataChannel 的 SCTP 不进入此 RTP 预算，它们仍受 transport 总队列上限约束。
- `IntervalBudget` 使用有符号整数记账，按真实 elapsed time 补充 `rate × elapsed / 8` 字节，保留不足一字节的小数余量。预算正负边界均为当前速率的 500 ms 窗口；不积累未使用的正预算，下一个周期重新发放额度，负预算则先偿还。调速保留并按新窗口截断欠账；rate=0 清零，恢复后等待处理周期。
- 默认每 5 ms 处理一次。只有正预算才放行整包，发送后可以透支；预算为零时也等待下一周期。定时器迟到最多补充 30 ms 的额度，不重放整段迟到时间。`maxBurstBytes` 默认 64 KiB，每个周期可以额外越过一次整包边界；transport 每轮最多处理 64 包再交还事件循环，同一周期的预算不会重新补充。队列为空时停止定时唤醒，空闲和暂停不保存正预算。极低目标速率下，一个完整数据报可能超过整个 500 ms 窗口，此时欠账仍按窗口截断，不能把预算解释成逐包严格速率保证。
- 媒体与 padding 具有独立预算，普通媒体、重传和已计费音频同时扣除两份预算。padding 必须等实际媒体队列清空，且两份预算都为正；其目标表示填补到该总速率，不是在媒体速率上额外叠加。模块仅调度调用者提供的 padding，不自行生成报文。
- 优先级为音频、缓存重传、视频、padding。同优先级的活动 FIFO 轮转；同一 SSRC 的普通音频/视频 RTP 保持入队顺序，即使优先级变化。缓存重传和 padding 使用独立 FIFO，padding 关闭不阻塞媒体。音频 PT 通过协商后的媒体种类分类；直接使用下层时在 `config.rtcp.parameters.media` 设置 `audio=true`。RTCP 自动处理关闭时，这些媒体参数仍可用于分类。`paceAudio=false` 允许音频绕过预算等待；`accountForAudio` 决定是否将其字节计入两份预算，默认两个选项都为 true。显式暂停和 rate=0 始终暂停所有 RTP。
- `drainLargeQueues=true` 时，周期开始按 `queuedMediaBytes × 8 / max(1 ms, queueTimeLimit - averageQueueTime)` 计算清空队列所需速率，取它与目标速率的较大值，上限 1 Gbit/s。这只调整实际媒体预算，不改外部估计器的目标，不增加 padding 速率；队列清空后恢复目标。平均等待时间排除暂停和 rate=0，padding 不参与提速字节数。关闭该选项后严格使用配置目标预算。
- 默认队列最多 256 包、2 MiB、256 个活动 FIFO；原包/重传/padding FIFO 分开计数。超过 pacer 或 transport 总额度，完成回调返回 `no_buffer_space`。总传输额度还包含在途媒体和 DTLS 应用发送。
- 超过 `maxQueueDelay` 的排队包以 `timed_out` 完成，未消耗 SRTP/TWCC 序号。暂停和 rate=0 时仍调度到期检查；等待时间包括暂停。当前按包过期，上层可按发送失败决定丢帧/重新请求关键帧。

## 加密、TWCC 与生命周期

入队时只校验 RTP 并预留 TWCC 扩展空间，出队时才分配 transport sequence、执行 SRTP 加密，再提交到 UDP Sender。TWCC 时间戳在加密后、交给 ICE Sender 前采样，**不包含 pacer 排队时间**；它是用户态提交时间，不是内核发送时间戳。实际 Sender 成功完成后才记录媒体发送统计/重传历史。失败、超时和取消回调返回 0 字节，成功返回调用者原始 RTP 明文长度。

缓存 NACK 重传经过相同 pacer，沿用原始 SRTP 密文，不再次保护或更改其 TWCC 序号。后续带宽估计器应识别这些重传，避免把原始包的发送时间与重传到达时间作为普通延迟样本；独立 RTX 封装仍待实现。

ICE restart 清理旧代排队包和预算，重置 pacer 统计/代次，保留当前配置速率与暂停状态。已经提交给 Sender 的旧包继续等待一次完成回调；迟到完成不写入新代媒体统计。失败、关闭停止定时器并取消排队包，close 等待真正提交的输出完成。普通 SDP 重协商保留 pacer 队列与预算。

`snapshot().dtls->pacer` 提供目标速率 `rateBps`、当前提速后的 `adjustedRateBps`、有效 `paddingRateBps`、两份有符号预算、暂停状态、排队包数/字节、最老墙钟等待和排除暂停的平均等待、预计序列化耗时、释放/拒绝/过期/取消计数。释放计数表示调度放行，实际加密/发送结果应结合 SRTP 和 RTCP 统计查看。`expectedQueueTime` 是按当前速率计算的队列字节耗时，未计入欠账和后续提速；暂停、rate=0 或有 padding 排队但 padding 速率为零时为空，活动状态下空队列为 0。

速率/暂停变更及积压提速使用现有 log 库的 DEBUG 日志；重复过期用采样 WARN 日志，不打印媒体负载或密钥。

## 范围与验证

支持调度已提供的 padding 包，当前不自动生成 padding、探测速率集群，也不实现拥塞窗口控制或带宽估计。接收方向不使用发送 pacer。

`interval_budget_test` 验证欠账、未用额度、窗口截断、小数余量和整数极值；`pacer_test` 用确定性时钟验证周期补预算、动态调速、padding 双预算、音频选项、积压提速、空闲和迟到定时器的突发限制、优先级/FIFO/轮转、额外开销、队列界限、暂停、到期和代次。`dtls_transport_test` 用真实 DTLS/libsrtp 验证媒体/padding 间隔、RTCP 绕过暂停、音频优先级、出队 TWCC 序号/时间、排队超时不耗序号、NACK 密文重传、关闭和旧代在途完成；`srtp_ctl_test integration` 验证 PeerConnection 配置转交与 ICE restart。

参考 [libwebrtc IntervalBudget](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/pacing/interval_budget.cc) 和历史 [PacingController 周期模式](https://webrtc.googlesource.com/src/+/ae10029bff6e5b53a7791a60e4163cc4201d61d2/modules/pacing/pacing_controller.cc)。新版动态欠账模式与此模式不同，当前实现不复制完整 libwebrtc 的探测器、拥塞窗口或 padding 生成链路。
