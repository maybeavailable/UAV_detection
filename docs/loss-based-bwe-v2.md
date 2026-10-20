# LossBasedBweV2

`src/webrtc/congestion_control/loss_based_bwe_v2.h/.cpp` 参考与现有估计器相同的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`：[原版头文件](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/loss_based_bwe_v2.h)、[原版实现](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/loss_based_bwe_v2.cc)。许可与来源见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 模型与观测

模型同时估计固有随机丢包概率和丢包受限带宽。当发送速率 `S` 超过候选容量 `C` 时，丢包概率为 `p + (1-p)*(S-C)/S`；否则为固有丢包概率 `p`，最终限制在 `1e-6..1-1e-6`。对多个候选带宽分别用 Newton 法拟合固有丢包，再比较带宽偏好项与历史加权对数似然；目标相同时选择更高带宽。保留瞬时丢包上界、确认吞吐下界、发送速率平滑、恢复期增长限制、HOLD 和 padding 状态。

`UpdateBandwidthEstimate(packet_results, delay_based_bps, in_alr)` 必须接收**完整的 `feedback.packet_feedbacks`**，同时包含收到和丢失的包。不要使用 `SortedByReceiveTime()`：它过滤丢包，只适合确认吞吐和延迟估计。每条记录必须有发送时间、非负的展开传输序号和合法包大小；`receive_time == nullopt` 表示丢失，存在时表示收到。算法只使用发送时间、包大小、序号和收到标记，不用接收时间计算丢包模型。

按发送时间累计观测，跨度达到默认 250 ms 才完成一个观测；首个观测从本批最早发送时间开始，后续从上一观测的最晚发送时间开始。时间不前进或跨度不足时保留部分观测，不重新拟合。乱序、重复丢包和恢复反馈按该源码版本处理：仍在当前部分观测中的同序号丢包只保留一份丢失大小，后续收到会移除此项；**每条反馈记录仍累计总包数和总字节数**。已完成的历史观测不会被后来恢复反馈追溯修改。因此不要把整批重复反馈主动投递两次，也不要自行移除 adapter 正常输出的恢复记录。

默认使用**字节丢失率**：对观测的字节数与丢失字节数按时间权重取整，再计算丢失比例。观测充分时剔除丢失率最大、最小的观测，以过滤尖峰；最大丢失观测伴随明显发送突发时保留它。可配置为按包数统计。两种模式同时影响模型的目标函数和 Newton 导数。包大小使用 `sent_packet.size`，不加入 `prior_unacked_data`，后者属于确认吞吐计量。

## 接口与状态

公开速率单位为 bit/s，时长使用 `std::chrono::microseconds`。`optional<uint64_t>` 的 `nullopt` 表示未知或没有有限上限，零是已知的合法速率。内部使用局部整数单位保持原版的浮点乘法取最近整数、整数除法截断、整数 kbit/s 取最近整数等规则；类型实现封装在 `.cpp`，接口与工程其余 C++17 组件一致。

| 接口 | 行为 |
| --- | --- |
| `IsEnabled()` | 配置启用且有效；默认配置启用 |
| `IsReady()` | 模型已初始化，并已完成最少数量的观测，默认 3 个 |
| `UseInStartPhase()` / `ReadyToUseInStartPhase()` | 配置允许 / 已可用于启动阶段；禁用时均返回 false |
| `SetBandwidthEstimate(bps)` | 设置模型初值；保留观测和计时历史，不等于重建控制器 |
| `SetAcknowledgedBitrate(bps)` | 提供确认吞吐，并更新瞬时下界；缺少新确认估计时可不调用，已有值会保留 |
| `SetMinMaxBitrate(min, max)` | 设置码率边界，最小值默认 1000 bit/s，最大值默认无界；`max=nullopt` 移除最大限制 |
| `GetLossBasedResult()` | 返回可选带宽和 `LossBasedState` |
| `Snapshot()` | 返回观测数量、部分观测计数、丢失率、固有丢包、内部模型带宽、发送速率、缓存上下界和下一次 HOLD 时长 |

未就绪时，`GetLossBasedResult()` 返回最近提供的有限延迟估计及 `kDelayBasedEstimate`；没有延迟估计时带宽为 `nullopt`。这与 `IsReady()` 独立，不能用“结果含有码率”判断丢包模型已就绪。空报告和禁用状态也会更新延迟回退值。未显式设置初值时，收到完整观测且有有限延迟估计后可自动初始化。

| LossBasedState | 含义 |
| --- | --- |
| `kDelayBasedEstimate` | 当前由延迟估计或最大边界约束 |
| `kDecreasing` | 丢包受限下降或 HOLD 阶段 |
| `kIncreasing` | 丢包受限恢复增长，可供后续控制器触发探测 |
| `kIncreaseUsingPadding` | 丢包受限恢复增长，提示后续控制器使用 padding |

HOLD 初始时长为 300 ms，默认每次进入时将下次时长乘 2，最多 60 秒。默认 padding 状态持续 2 秒；此状态表示控制意图，不在本类内生成 padding 包。保留源码实际的 **1 bit/s** 恢复增量，避免被原版“1 kbps”的注释误导。

## 配置

`Config` 显式表达原版 `CreateConfig()` 的有效默认值，替代 FieldTrials 字符串；可直接查看头文件中的全部字段。候选因子默认 `{1.02, 1.0, 0.95}`，可加入确认吞吐候选和更高的延迟估计候选；默认在 ALR 中跳过确认吞吐候选，但 padding 活跃时允许例外。可配置 ALR 中追加瞬时上界候选。

默认配置还包括：15 个观测窗口、Newton 迭代 1 次/步长 0.75、固有丢包初值 0.01/下界 0.001、两套时间权重因子 0.9、瞬时丢包上界 `100000/(average_loss-0.05)`、确认吞吐下界因子 1.0、恢复增长系数 1.3/窗口 300 ms、字节丢失率启用、将内部最优候选收敛到实际边界启用。

`bound_best_candidate` 截断内部候选时会把固有丢包重置为 0，所以下一轮拟合前快照中的值可以低于拟合下界；这保留了原版行为。

候选、瞬时界限、延迟估计和恢复窗口按源码顺序约束。**确认吞吐/最小值下界的优先级高于延迟估计和最大值上界**，因此下界较高时，输出可能超过所提供的延迟估计或最大限制；这保留了上游策略，调用方应通过发送控制器整合最终限制。

非法配置使实例禁用，并记录原因。另检查显式 C++ 浮点参数的 NaN/Infinity，以及负 padding/HOLD/倍率等无法安全参与单位运算的值。非法包记录、超范围速率或字节累计会在消费整批前抛出异常。与原版有符号整数单位对应，公开速率和单个观测字节累计的有限范围为 `0..INT64_MAX-1`，更大输入拒绝；内部超范围计算饱和到有限边界。空字节观测的丢失比例定义为 0，避免原版 `0/0`。

`max=nullopt` 在此接口明确表示移除最大限制，与原版忽略无穷大 setter 参数的行为不同。`UseInStartPhase()` 和禁用状态的确认吞吐 setter 已防护空配置访问。

## 与现有组件配合

```cpp
#include "webrtc/congestion_control/acknowledged_bitrate_estimator.h"
#include "webrtc/congestion_control/delay_based_bwe.h"
#include "webrtc/congestion_control/loss_based_bwe_v2.h"

webrtc::AcknowledgedBitrateEstimator acknowledged;
webrtc::DelayBasedBwe delay_bwe;
webrtc::LossBasedBweV2 loss_bwe;

webrtc::LossBasedBweV2::Result onFeedback(
    const webrtc::TransportPacketsFeedback& feedback, bool in_alr)
{
    acknowledged.SetAlr(in_alr);
    acknowledged.IncomingPacketFeedbackVector(feedback.SortedByReceiveTime());
    if (const auto ack = acknowledged.bitrate()) loss_bwe.SetAcknowledgedBitrate(*ack);

    const auto delayed = delay_bwe.IncomingPacketFeedbackVector(
        feedback, acknowledged.bitrate(), std::nullopt, std::nullopt, in_alr);
    const auto delay_rate = delayed.updated
        ? std::optional<std::uint64_t>(delayed.target_bitrate)
        : delay_bwe.LatestEstimate();
    loss_bwe.UpdateBandwidthEstimate(feedback.packet_feedbacks, delay_rate, in_alr);
    return loss_bwe.GetLossBasedResult();
}
```

调用前可分别设置延迟/丢包估计初始码率和最小/最大限制；ALR 退出时仍按确认吞吐估计器约定设置结束时间。拥有者负责串行调用、生命周期、路由/ICE generation 改变时重建整套估计器，并消费结果状态来控制探测、pacer 和编码器。类不自动订阅 transport 事件。

## 验证

`loss_based_bwe_v2_test` 覆盖启用/禁用、启动就绪和延迟回退、250 ms 边界、部分观测、丢包恢复、按包/字节统计、尖峰与突发发送、HOLD/padding、码率边界、无效输入原子性与零字节观测，并走真实 RTCP TWCC 编解码 → adapter → 确认吞吐/延迟估计 → 丢包估计的数据链。

八组配置各回放 600 批反馈，共 4800 批，与固定修订版的**原始算法和原始整数单位类型**对照。比较输出带宽、四种状态、就绪标记、观测与待定丢包计数、内部模型带宽、发送速率、上下界、HOLD 时长，以及量化至 `1e-9` 的平均/固有丢包比例。

可用 `python3 tests/webrtc/reference/generate_loss_bwe_v2_reference.py` 复现期望值。脚本联网下载固定版本、使用 C++20 编译原版 `std::span` 实现，只替换平台、日志、试验解析和反馈容器依赖；共享回放输入，不共享估计算法。普通 CMake/CTest 和产品代码使用 C++17，测试时不联网。
