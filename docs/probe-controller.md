# ProbeController

`src/webrtc/congestion_control/probe_controller.h/.cpp` 参考现有拥塞控制模块使用的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`：[原版头文件](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/probe_controller.h)、[原版实现](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/probe_controller.cc)。许可和来源见 `third_party/webrtc/NOTICE`、`LICENSE`、`PATENTS`。

控制器同步生成 `ProbeClusterConfig`，由拥有者调度探测包；公开接口使用 C++17 的 `steady_clock::time_point`、微秒时长和整数 bit/s。拥有者负责串行调用，并在同一个本地时钟上定期调用 `Process` 驱动超时和周期探测。

## 探测行为

- **初始和指数探测**：网络可用且起始码率已知后，默认依次请求起始码率的 3 倍和 6 倍。估计结果严格高于最后一个簇实际目标的 0.7 倍时，再请求估计的 2 倍。单个候选达到或超过探测上限，就停止这一轮继续探测；被限制成相同速率的两个簇仍保留。
- **结果超时**：`Process` 中距离上次探测**严格超过 1 秒**时结束等待。在恰好 1 秒时仍等待；仅更新码率不会自动执行这个超时。
- **启动重复探测**：启用后，在初始探测建立的默认 5 秒窗口内，等待结束后可再探测估计的 3 倍；窗口内簇使用 100 ms 目标时长、20 ms 突发间隔。100 ms 调用周期下，相邻等待超时通常相隔 1.1 秒。
- **ALR 周期探测**：启用后，从 ALR 开始和上次探测两者中较晚的时刻起，默认每 5 秒请求当前估计的 2 倍。退出 ALR 要同时清除起点并记录结束时间。
- **分配码率变化**：总分配码率增加且估计不足时，默认请求总分配码率的 1 倍和 2 倍，先受当前估计的 2 倍约束。受当前估计限制时允许后续指数探测。总分配码率是所有活跃流的配置之和，与最大码率不同。
- **下降后恢复**：估计下降到原来的 0.66 倍以下时记录下降；处于 ALR、离开 ALR 不满 3 秒或启用快速恢复时，`RequestProbe` 可请求下降前估计的 0.85 倍。要求下降未满 5 秒、距上次此类请求或 Reset 超过 5 秒，且探测的 95% 仍高于当前估计。
- **网络容量估计**：可按容量上界周期探测，也可在当前估计低于上界某个比例时使用另一周期。生成速率仍以 ALR 探测倍率计算，再按配置应用容量限制。仅启用低估比例分支、未启用有限网络周期时，不使用该周期关联的速率上限和时长覆盖。

默认最大探测速率为 5 Mbit/s，`SetBitrates` 的 `max_bitrate=nullopt` 恢复此默认值。非零总分配码率另将上限限制到它的 2 倍。达到上限的目标不会触发无限后续探测。

## 带宽限制原因

`SetEstimatedBitrate` 同时记录当前估计与 `BandwidthLimitedCause`，以区别不同的探测策略：

| 原因 | 行为 |
| --- | --- |
| `kDelayBasedLimited` | 允许正常探测 |
| `kLossLimitedBweIncreasing` | 允许探测，默认限制到当前估计的 1.5 倍 |
| `kLossLimitedBwe` | 抑制新探测 |
| `kDelayBasedLimitedDelayIncreased` | 抑制新探测 |
| `kRttBasedBackOffHighRtt` | 抑制新探测 |

发送控制器可将 `LossBasedBweV2` 的 `kIncreasing` / `kIncreaseUsingPadding` 映射到第一种丢包恢复原因，将 `kDecreasing` 映射到丢包限制原因；是否因延迟增长或高 RTT 限制应结合相应检测结果决定。提供最终估计，不能直接把某个尚未接受的探测簇目标当成测量结果。

## 接口与簇参数

`ProbeControllerConfig` 显式提供原版试验配置字段，默认值对应 `WebRTC-Bwe-ProbingConfiguration`、`WebRTC-Bwe-AllocationProbing` 和 `WebRTC-Bwe-ProbingBehavior` 解析后的值；`rapid_recovery` 对应单独的快速恢复试验。可配置初始/后续倍率、可选第二次探测、ALR/网络周期、分配探测、持续时间、突发间隔、最少包数、丢包受限倍率和高估计跳过策略。有限参数非法时构造函数抛出 `invalid_argument`；可选时长的 `nullopt` 代替原版正无穷。

`skip_if_estimate_larger_than_fraction_of_max` 默认 0，关闭跳过策略。启用后，`min(当前估计, 网络上界)` 已严格大于配置最大值或分配码率折算上限的一定比例，就跳过探测并结束等待。已有网络估计对象但上界未知时，按原版默认负无穷处理这次跳过比较；没有网络估计对象时按无界处理。

`ProbeClusterConfig` 独立定义在 `probe_cluster_config.h`：

| 字段 | 含义 |
| --- | --- |
| `at_time` | 生成请求的本地时间，微秒精度 |
| `target_data_rate` | 目标 bit/s |
| `target_duration` | 目标持续时长，默认 15 ms |
| `min_probe_delta` | 探测突发之间的最小间隔，默认 2 ms |
| `target_probe_count` | 最少探测包数，默认 5 |
| `id` | 从 1 开始递增的 32 位簇 ID，Reset 不复用 |

启用有限网络探测周期且已知上界不低于簇目标时，优先使用网络模式时长/间隔，默认 15 ms / 20 ms；其次是启动重复探测窗口的 100 ms / 20 ms；最后是普通的 15 ms / 2 ms。

执行端可由目标速率与时长计算最少字节数，按原版速率乘时长取最近整数字节，再将 ID、最少包数和最少字节数写入现有 `PacedPacketInfo`。`TransportFeedbackAdapter` 会随发送历史和 TWCC 结果保存这些元数据。执行端负责探测簇调度和探测包生成；本类输出探测请求，接收反馈由下述速率估计器处理。

[ProbeBitrateEstimator](probe-bitrate-estimator.md) 已实现探测反馈的速率测量，可消费 adapter 返回的探测包记录；将测量结果交给延迟带宽估计，再用最终估计更新本控制器。

## 生命周期与源码边界

`OnNetworkAvailability(false)` 会终止当前结果等待；初始探测在网络可用后启动。其他发起策略按原版不统一检查这个标志，传输拥有者负责在不可写时停止执行探测请求，并控制 `Process` 的调用。

`Reset(now)` 清除估计、起始/最大配置、网络容量估计、ALR 结束时间及下降恢复历史，但保留网络可用性、两种启用开关、总分配码率、ALR 起点、已有启动重复探测截止时间和下一个簇 ID。需要清除 ALR 起点时显式调用 `SetAlrStartTime(nullopt)`；完整新生命周期也可以重建实例。

启动重复探测保留实际源码的截止时间语义：开关影响初始阶段是否建立窗口，关闭开关不会取消已建立的窗口。非零分配码率的普通更新路径会截止窗口；触发分配探测或因首个分配候选关闭而提前返回的路径则保留原截止时间。

带时间参数的控制事件要求非负、单调不减；ALR 起点/结束时间作为历史元数据可早于当前事件时间。速率支持完整 `uint64_t` 范围，浮点倍率按最近整数 bit/s 取整，溢出饱和，定时截止时间饱和以避免时长溢出。零起始值等待正数约束；零/取整后为零的目标不生成簇；显式零最大值拒绝。簇 ID 耗尽时抛出异常，避免回绕复用。

## 调用示例

```cpp
#include "webrtc/congestion_control/probe_controller.h"

webrtc::ProbeController probes;

std::vector<webrtc::ProbeClusterConfig> onTransportReady(
    std::chrono::steady_clock::time_point now)
{
    probes.EnablePeriodicAlrProbing(true);
    auto pending = probes.SetBitrates(30000, 300000, 5000000, now);
    const auto available = probes.OnNetworkAvailability({now, true});
    pending.insert(pending.end(), available.begin(), available.end());
    return pending;
}

std::vector<webrtc::ProbeClusterConfig> onEstimate(
    std::uint64_t final_bitrate_bps,
    webrtc::BandwidthLimitedCause cause,
    std::chrono::steady_clock::time_point now)
{
    return probes.SetEstimatedBitrate(final_bitrate_bps, cause, now);
}
```

拥有者应消费每个返回向量，包括 `SetBitrates`、网络可用事件、分配变化、估计变化、`RequestProbe` 和定期 `Process` 的返回值。将 `AlrDetector` 的起点交给 `SetAlrStartTime`；退出时清除起点，并调用 `SetAlrEndedTime`。恢复判据成立后由发送控制器决定调用 `RequestProbe`。

## 验证

`probe_controller_test` 覆盖初始和指数增长、严格门限、超时、分配与最大码率限制、重复启动、ALR、网络估计、五种限制原因、快速恢复、Reset 保留状态、ID 连续性、零值和数值边界。另用实际 `AlrDetector` 生成起点，通过控制器生成簇，再验证 `PacedPacketInfo` 经 adapter 的真实 RTCP TWCC 编解码后完整保留。

八组配置各回放 1500 个事件，共 12000 个，与固定修订版的原始控制器、配置默认值和原始整数单位类型对照，比较每个簇的全部字段、所有状态计数、估计/约束、等待门限、启动截止时间和簇 ID 的顺序哈希。

`python3 tests/webrtc/reference/generate_probe_controller_reference.py` 可联网获取固定版本并复现期望值。参考程序使用 C++20 编译原版 `std::span` 实现，只替换日志、试验解析、平台和网络数据容器；产品与普通测试保持 C++17，测试时不联网。
