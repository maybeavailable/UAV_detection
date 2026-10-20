# DelayBasedBwe / AimdRateControl

实现位于 `src/webrtc/congestion_control/`，沿用现有模块固定的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`：

- [DelayBasedBwe](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/delay_based_bwe.cc)
- [AimdRateControl](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/remote_bitrate_estimator/aimd_rate_control.cc)

代码保留源码算法与有效配置分支，使用本项目的 `TransportPacketsFeedback`、`std::chrono`、`optional<uint64_t>` 和日志库，替换 WebRTC 的单位类型、FieldTrials、事件日志和指标依赖。许可证与来源见 `third_party/webrtc/NOTICE`、`LICENSE`、`PATENTS`。

## 输入和输出

`IncomingPacketFeedbackVector(msg, acked_bitrate, probe_bitrate, network_estimate, in_alr)` 消费经过认证的 TWCC 适配结果。先选取收到且具有发送历史的包，按接收时间、发送时间、传输序号排序，送入 5 ms 的 `InterArrivalDelta` 包组，再把微秒精度的组间差值交给 `TrendlineEstimator`，最后用检测状态和确认吞吐驱动 AIMD。

所有速率均为 **bit/s**，时间使用 `steady_clock::time_point`，RTT 和可配置时长使用微秒。反馈时间必须非负、单调不减；包的发送/接收时间必须非负。反馈时间属于本地处理时钟，接收时间属于适配器重建的接收时钟，不应将两者相减当作单程延迟。Trendline 的时间戳按原版取最近整数毫秒，组间差值保留小数毫秒。

| Result 字段 | 含义 |
| --- | --- |
| `updated` | 已有有效的目标，可消费 `target_bitrate`；不代表数值一定变化 |
| `target_bitrate` | bit/s；只在 `updated == true` 时使用 |
| `probe` | 本轮接受外部探测估计；过载时忽略探测 |
| `recovered_from_overuse` | 保留原版字段名，实际表示本批次出现 **underusing → normal**，且走普通非探测分支 |
| `delay_detector_state` | 当前活动检测器的 normal / underusing / overusing |

空报告、全丢包报告、缺少发送记录的报告直接返回默认 Result，也不消费探测结果。初始 AIMD 内部占位值为 30 Mbit/s，并非有效估计或最大限制；因此未初始化时即使 Result 的 `target_bitrate` 非零，也不能忽略 `updated`。

`LatestEstimate()` 返回可选的有效当前值；兼容原版的输出参数重载保留固定占位 SSRC `{0}`，估计仍然属于整条传输。`last_estimate()` / `last_state()` 是上次反馈发布的值；`SetStartBitrate` 和 `TriggerOveruse` 直接更新 AIMD，不修改这两个反馈记录字段。

## AIMD 行为

- 设置起始/探测码率即可初始化；没有起始值时，确认吞吐首次出现后**严格超过 5 秒**才自然初始化。过载输入也可提前初始化。
- 正常状态增加码率：尚无容量历史时采用每秒 1.08 倍的乘性增长，单次增加至少 1000 bit/s、增长时间最多 1 秒；有容量历史时按 RTT、30 fps 帧大小和 1200 字节包长计算加性增长。正常增加受 `1.5 * acknowledged + 10000` 限制，但确认吞吐下降本身不会强行降低现有目标。
- 过载时回退到 `0.85 * acknowledged`，结果大于 5000 时再减去 5000 bit/s；保留容量估计兜底、不因过载提高码率、容量上下界重置和过载后 hold 状态。
- 默认最小值为 5000 bit/s。`SetMinBitrate` 修改最小值；`SetStartBitrate` 按源码直接设置、不夹紧，后续更新才应用约束。
- 有确认吞吐时，连续降速间隔为 RTT 限制到 10..200 ms；确认吞吐小于当前估计的一半时可提前降速。
- 无确认吞吐但已有有效估计且检测过载时使用减半分支。**该修订版实际代码的 `InitialTimeToReduceFurther` 会通过半速判断提前放行，连续反馈可立即再次减半**；不能把源码注释的“每 200 ms”当作实际保证。
- `GetFeedbackInterval()` 保留原版 80 字节 RTCP / 5% 带宽的 200..1000 ms 建议周期。本类不负责安排 RTCP 定时器。

`AimdRateControl::Config` 显式表达上游实验参数：`backoff_factor` 默认 0.85，非法值回退默认；`no_bitrate_increase_in_alr` 默认 false，启用后发送端在 ALR 中停止普通增加，探测设置仍可提高码率；`disable_estimate_bounded_increase` 默认 false；`use_current_estimate_as_min_upper_bound` 默认 true。`DelayBasedBwe` 始终将 `send_side` 设为 true。

`NetworkStateEstimate` 仅保留 AIMD 消费的 `link_capacity_lower/upper`，均为可选 bit/s，缺省表示没有有限边界，0 是有效边界。默认上界使用 `max(current, upper)`，因此低上界只会阻止继续增长；设置 `use_current_estimate_as_min_upper_bound=false` 可严格应用外部上界。下降时还受 `beta * lower` 保护，最后应用最小码率。

保留原版单位运算的差别：浮点速率乘法取最近整数，构造浮点 bit/s 截断，整数除法截断，帧大小按最近字节计算。超出 `uint64_t` 的结果饱和；零/不足一字节的帧采用 4000 bit/s/s 最小增长率，避免原版除零路径。负 RTT、倒退控制时间、非法包时间或大小会抛出异常；拥有者应串行调用。

## 音视频分离与超时

默认共用视频检测器。`Config::separate_audio.enabled=true` 时，为音频和视频维护独立包组和 Trendline：连续音频包数量**大于** `packet_threshold`（默认 10），且距离最后视频包的接收时间**大于** `time_threshold`（默认 1 秒），才切换到音频检测；视频到达立即切回视频。首次只有音频时仍须满足包数量条件。检测器使用同一个可选的、外部持有的 `NetworkStatePredictor`，其生命周期必须覆盖控制器。

两次非空反馈的本地处理时间间隔**大于 2 秒**时重建两组延迟检测器，保留 AIMD 和音视频切换历史，与源码一致。路由、ICE association/generation 改变时，拥有者应重建整套估计器，避免将新旧路径历史混用；类不自行订阅传输生命周期。

## 调用方式

```cpp
#include "webrtc/congestion_control/acknowledged_bitrate_estimator.h"
#include "webrtc/congestion_control/delay_based_bwe.h"

webrtc::AcknowledgedBitrateEstimator acknowledged;
webrtc::DelayBasedBwe delay_bwe;

void initialize()
{
    delay_bwe.SetStartBitrate(600000);
    delay_bwe.SetMinBitrate(30000);
    delay_bwe.OnRttUpdate(std::chrono::milliseconds(100));
}

webrtc::DelayBasedBwe::Result onFeedback(
    const webrtc::TransportPacketsFeedback& feedback,
    bool in_alr, std::optional<std::uint64_t> probe_bps)
{
    acknowledged.SetAlr(in_alr);
    acknowledged.IncomingPacketFeedbackVector(feedback.SortedByReceiveTime());
    return delay_bwe.IncomingPacketFeedbackVector(
        feedback, acknowledged.bitrate(), probe_bps, std::nullopt, in_alr);
    // 调用者仅在 result.updated 时消费 result.target_bitrate。
}
```

ALR 结束时仍应按确认吞吐组件的约定调用 `SetAlrEndedTime`。`probe_bps` 是外部探测估计结果，本类不根据 probe cluster 自行计算它。包大小使用 `sent_packet.size`，不把 `prior_unacked_data` 加入延迟包组；后者仅属于确认吞吐计量。

这是 GCC 的延迟带宽估计组件。当前提供完整反馈输入和目标码率输出，不自动接管 `DtlsTransport` 的 pacer 或编码器；后续发送控制器可以在整合探测、其他限制与估计后统一消费目标值。

[LossBasedBweV2](loss-based-bwe-v2.md) 已实现丢包模型，可以消费本类的有效估计与完整的 `packet_feedbacks`，结合确认吞吐生成丢包受限带宽及恢复状态；不要将仅包含收到包的排序向量用于丢包统计。

## 验证

`aimd_rate_control_test` 覆盖初始化、增长与回退、RTT/半速降速判断、ALR、容量上下界、取整、零值和宽整数边界；与原版源码及原版整数单位实现对照六种配置、共 6000 次更新。

`delay_based_bwe_test` 对照原版五个类和原版单位类型，回放四种场景、共 2400 批报告，比较目标速率、Result 全部字段、历史状态与有效估计的顺序哈希。额外覆盖过载分支、探测优先级、音视频切换边界、2 秒超时边界、2000 包小数毫秒精度、丢包/未知发送记录过滤，以及 RTCP TWCC 编解码 → TransportFeedbackAdapter → AcknowledgedBitrateEstimator → DelayBasedBwe 的实际数据链。

对照期望值可以独立复现：`python3 tests/webrtc/reference/generate_delay_bwe_reference.py`。此脚本显式联网下载固定修订版的源文件，使用原版单位运算和算法函数生成单独的参考程序；仅替换平台、日志、试验解析和反馈容器依赖。普通 CMake/CTest 不联网、不编译此参考程序。
