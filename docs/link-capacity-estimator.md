# LinkCapacityEstimator

`src/webrtc/congestion_control/link_capacity_estimator.h/.cpp` 参考 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`，与本项目已有拥塞控制组件使用同一源码版本。

源码依据：[link_capacity_estimator.h](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/link_capacity_estimator.h)、[link_capacity_estimator.cc](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/link_capacity_estimator.cc)。调用策略可参考上游 [AimdRateControl](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/remote_bitrate_estimator/aimd_rate_control.cc)。许可和来源记录见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 更新规则

`OnOveruseDetected(acknowledged_rate_bps)` 在控制器检测到过载时消费确认吞吐，均值更新权重为 0.05；`OnProbeRate(probe_rate_bps)` 消费带宽探测得到的速率，权重为 0.5。普通确认吞吐更新本身不自动更新链路容量。

所有公开输入/输出速率均使用 `uint64_t` bit/s。按原版 `DataRate::kbps()` 的默认整数返回类型，输入先四舍五入到整数 kbit/s，再转 double：例如 499 bit/s 得到 0，500 得到 1 kbit/s，1,234,567 得到 1,235 kbit/s。均值和归一化方差在内部保留 double，输出时再将 kbit/s 乘 1000 并向整数 bit/s 截断。

首个样本直接初始化均值，后续均值使用 `mean = (1 - alpha) * mean + alpha * sample`。方差更新使用**已更新的均值**与当前样本之差，并除以 `max(mean_kbps, 1.0)`；归一化方差初始为 0.4，之后限制在原版 `0.4f..2.5f` 范围。标准差为 `sqrt(normalized_variance * mean_kbps)`，上下界是内部均值加减三倍标准差，下界至少为 0。

`Reset()` 只清除均值，不清除归一化方差，与上游相同。因此重新采样后的范围可以比全新实例更宽：

| 操作 | 均值 bit/s | 下界 bit/s | 上界 bit/s |
| --- | --- | --- | --- |
| 全新实例收到过载样本 500,000 | 500,000 | 457,573 | 542,426 |
| 随后收到探测样本 1,000,000 | 750,000 | 620,096 | 879,903 |
| Reset 后收到过载样本 500,000 | 500,000 | 396,619 | 603,380 |

如果需要全部初始状态，例如拥有者重建一条新路由的控制器，应创建新实例。类不会自行根据上下界判断是否重置，相关策略由带宽控制器决定。

## 接口和未知状态

| 接口 | 有估计时 | 无估计时 |
| --- | --- | --- |
| `has_estimate()` | true | false |
| `estimate()` | 均值，`optional<uint64_t>` | `nullopt` |
| `UpperBound()` | 三倍标准差上界，`optional<uint64_t>` | `nullopt`，对应原版 `DataRate::Infinity()` |
| `LowerBound()` | 三倍标准差下界，`uint64_t` | 0 |

零速率是合法样本，可以产生已知的零容量；不要把它与未知状态混淆。上界的 `nullopt` 表示尚无有限上界，不应解释成 0。超出 `uint64_t` 表示范围的计算结果饱和到上限，并不使用该上限表示无穷大。公开速率支持整个 `uint64_t` 范围，转换和取整避免溢出。

## 与确认吞吐、探测结果配合

```cpp
#include "webrtc/congestion_control/link_capacity_estimator.h"

webrtc::LinkCapacityEstimator capacity;

void onOveruse(std::optional<std::uint64_t> acknowledged_bps)
{
    if (!acknowledged_bps) return;
    // 上游 AIMD 在过载样本明显低于下界时清除旧均值，允许立即重新初始化。
    if (*acknowledged_bps < capacity.LowerBound()) capacity.Reset();
    capacity.OnOveruseDetected(*acknowledged_bps);
}

void onProbeResult(std::uint64_t probe_bps)
{
    capacity.OnProbeRate(probe_bps);
}
```

`acknowledged_bps` 可以来自已有 `AcknowledgedBitrateEstimator::bitrate()`。容量上界也可供控制器在吞吐明显超过旧上界时重新探索：先判断 `UpperBound()` 是否有值，再与解引用后的 bit/s 比较。容量估计提供控制器的历史和范围，本类不生成探测流量，也不直接设置 pacer/编码器目标。

[DelayBasedBwe / AimdRateControl](delay-based-bwe.md) 已实现这些容量更新和重置策略。按该修订版源码，AIMD 的外部探测分支调用 `SetEstimate`，不额外调用容量估计的 `OnProbeRate`。

类同步执行，拥有者负责串行调用和生命周期。初始化、探测速率更新、已有均值的重置使用项目现有 DEBUG 日志，不逐次记录普通过载更新。

## 验证

`link_capacity_estimator_test` 验证未知状态、两种更新权重、上下界、方差限幅与收敛、Reset 保留方差、kbit/s 取整边界、零值、完整 `uint64_t` 范围的安全转换，以及确认吞吐估计器到容量估计器的调用。

原版 `.h/.cc` 函数只适配 `DataRate` 与 `SafeClamp` 依赖，生成固定回放期望值。三种场景各输入 1000 个样本，逐次记录均值、上下界及估计是否存在，并比较顺序哈希，覆盖 3000 次样本更新，包含稳态、容量升降、噪声、零值、取整边界、过载/探测混合和反复重置。
