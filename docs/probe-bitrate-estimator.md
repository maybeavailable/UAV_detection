# ProbeBitrateEstimator

`src/webrtc/congestion_control/probe_bitrate_estimator.h/.cpp` 参考现有模块相同的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`：[原版头文件](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/probe_bitrate_estimator.h)、[原版实现](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/probe_bitrate_estimator.cc)。许可与来源见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 输入与计算

`HandleProbeAndEstimateBitrate(const PacketResult&)` 逐包消费已收到、有发送历史的探测反馈，返回可选的整数 bit/s。每个簇由非负的 `sent_packet.pacing_info.probe_cluster_id` 标识，元数据中的 `probe_cluster_min_probes` 和 `probe_cluster_min_bytes` 必须为正数。`PacedPacketInfo::kNotAProbe` 为 -1，负 ID 的普通包、丢失包和没有发送时间的记录直接忽略，不更新簇或清理历史。

簇分别记录最早/最晚发送、最早/最晚接收、最后发送包大小、最先接收包大小、总字节和包数，支持乱序反馈和交错簇。极值时间相等时保留先处理包的大小，与原版严格 `<` / `>` 判断相同。大小使用 `sent_packet.size`，包括 adapter 配置的网络开销；`prior_unacked_data`、`wire_size` 和目标发送码率不参与本计算。

同时满足收到包数与收到字节的门限后才估计：

- 最少包数为 `floor(probe_cluster_min_probes * 0.80)`，例如要求 5 包时收到 4 包即可满足。
- 最少字节数为 `round(probe_cluster_min_bytes * 0.80)`，例如要求 6 字节时必须收到至少 5 字节。
- 门限取自**当前反馈包**的元数据，不在首包时冻结。
- 发送和接收跨度都必须大于 0、且不超过 1 秒；恰好 1 秒有效。

发送速率使用 `(总字节 - 最后发送包大小) / 发送跨度`；接收速率使用 `(总字节 - 最先接收包大小) / 接收跨度`。因为时间跨度不包含发送最后一包、接收第一包的完整传输时间，需要扣除对应大小。速率按原版整数微比特/微秒除法向下取整；不把两种时钟的时间戳相减当作单程延迟。

接收/发送速率比大于 2 时拒绝，等于 2 仍有效。通常取两个速率较小者；若接收速率严格小于 `round(0.9f * 发送速率)`，认为链路已饱和，返回 `round(0.95f * 接收速率)`。保留原版 **float 常量再提升到 double** 的计算方式，例如 80 Mbit/s 的接收速率在饱和分支得到 75,999,999 bit/s，而非把系数换成 double 后的 76,000,000。

## 历史与结果缓存

一个簇达到门限后，后续探测包仍可更新它并产生新结果，不会立即删除或标记完成。类不按传输序号去重，拥有者应使用 adapter 的正常反馈输出；adapter 会移除已经确认的发送历史，避免重复收到包再次投递。

每次有效探测输入前，删除 `簇的最后接收时间 + 1 秒 < 当前包接收时间` 的簇。边界相等时保留，清理由接收时钟驱动，允许输入乱序；普通流量和读取接口不触发清理。

`FetchAndResetLastEstimatedBitrate()` 返回**最后一次处理成功的结果**，并清除这个缓存。它不取各簇最大值，也不清除簇统计。门限不足、区间/比例无效和簇历史过期均不清除已有成功结果；连续读取两次时第二次返回 `nullopt`。通常在处理完一批反馈后调用一次，将本批最后的成功结果交给延迟估计器。

`Snapshot()` 提供当前簇数量和未读取的成功结果，供诊断使用。拥有者应串行访问，并在路由或 ICE generation 改变时重建实例，避免混合不同路径的簇与接收时钟。

接口使用 C++17、`std::chrono` 和 `optional<uint64_t>` bit/s，替代上游单位类型；结果成功/失败采用项目现有 DEBUG 日志。非法探测门限或负时间戳在更新前抛出 `invalid_argument`，计数溢出抛出 `overflow_error`。字节到速率的乘除避免中间溢出，超出 `uint64_t` 的有限速率饱和到上限。扣除边界包后没有有效字节时返回未知，避免原版断言及零速率路径。

## 与现有组件配合

`ProbeController` 生成探测簇请求，执行端调度实际探测，并把簇 ID、最少包数和最少字节数写入 `PacedPacketInfo`。`TransportFeedbackAdapter` 把这些元数据及实际发送时间带入 TWCC 的 `PacketResult`；本类据此测量探测速率。

```cpp
#include "webrtc/congestion_control/probe_bitrate_estimator.h"
#include "webrtc/congestion_control/delay_based_bwe.h"

webrtc::ProbeBitrateEstimator probe_estimator;
webrtc::DelayBasedBwe delay_bwe;

webrtc::DelayBasedBwe::Result onFeedback(
    const webrtc::TransportPacketsFeedback& feedback,
    std::optional<std::uint64_t> acknowledged_bps, bool in_alr)
{
    for (const auto& packet : feedback.SortedByReceiveTime()) {
        (void)probe_estimator.HandleProbeAndEstimateBitrate(packet);
    }
    const auto probe_bps = probe_estimator.FetchAndResetLastEstimatedBitrate();
    return delay_bwe.IncomingPacketFeedbackVector(
        feedback, acknowledged_bps, probe_bps, std::nullopt, in_alr);
}
```

`DelayBasedBwe` 可在非过载状态接受测量结果。发送控制器再结合其他限制形成最终带宽，用 `ProbeController::SetEstimatedBitrate` 更新估计和限制原因；达到门限时会生成下一轮探测请求。这里测量成功不代表请求中的目标码率已被原样实现，执行与反馈都需要实际发送路径提供。

## 验证

`probe_bitrate_estimator_test` 覆盖原版正常、快速/慢速接收、突发、大小不同、多个簇和读取缓存场景，以及包数/字节门限不同取整、float 系数高码率差异、比例与 1 秒边界、乱序和相同时间极值、缓存保留、输入过滤、原子拒绝与数值溢出。

六类输入各回放 4000 个包，共 24000 个，与原版算法和原版整数单位类型对照，比较逐包返回值、缓存读取、历史簇数量、成功与读取次数。`python3 tests/webrtc/reference/generate_probe_bitrate_estimator_reference.py` 可联网下载固定版本、使用 C++17 重现期望值；参考与产品只共享输入，普通 CMake/CTest 不联网。

集成测试使用真实 `ProbeController` 请求生成元数据，经发送历史、RTCP TWCC 编解码和 adapter 得到反馈，再测量探测速率、交给 `DelayBasedBwe`，最后验证触发后续指数探测及重复反馈被去除。
