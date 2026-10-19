# AlrDetector

`src/webrtc/congestion_control/alr_detector.h/.cpp` 参考与本项目其他拥塞控制组件相同的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`，根据实际发送字节和发送间隔识别应用受限区域（ALR）。

源码依据：[alr_detector.h](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/alr_detector.h)、[alr_detector.cc](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/alr_detector.cc)、[IntervalBudget](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/pacing/interval_budget.cc)。BSD 许可、专利声明和来源见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 参数与原版默认值

`Config` 直接承接上游试验解析后的参数，代替项目尚未引入的 `FieldTrialsView`。该修订版的 [AlrExperimentSettings](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/rtc_base/experiments/alr_experiment.cc) 默认开启屏幕共享试验，所以实际默认值与 `alr_detector.h` 中的基础初始值不同：

| 配置 | 实际默认值（本项目采用） | 关闭试验后的基础值 |
| --- | --- | --- |
| `bandwidth_usage_ratio` | 0.80 | 0.65 |
| `start_budget_level_ratio` | 0.40 | 0.80 |
| `stop_budget_level_ratio` | -0.60 | 0.50 |

需要基础参数时可传 `AlrDetector::Config{0.65, 0.80, 0.50}`；也可设置其他明确的参数，例如原版单元测试中的 `{0.90, 0.0, -0.10}`。比例须有限，带宽使用比例在 `(0, 1]`，预算阈值满足 `-1 <= stop < start <= 1`。不解析试验字符串，也不把试验中的 pacing factor 或队列参数应用到 pacer。

## 预算与时间语义

`SetEstimatedBitrate(bitrate_bps)` 接受当前带宽控制器的估计码率，单位 bit/s。先按使用比例计算目标并四舍五入到整数 bit/s，再四舍五入到整数 kbit/s，与上游 `DataRate` 运算一致。输入必须大于 0，量化后的预算速率不能超过项目 `IntervalBudget::MaxRateBps`（1 Gbit/s）；无效更新抛出 `invalid_argument`，保留原状态。极低的正码率可以量化为 0，与上游一致。

预算允许积累未使用额度，上下限为目标速率的 500 ms 字节量。`OnBytesSent(bytes_sent, send_time)` 首次只记住发送时间，因发送历时未知而忽略该包的字节量。之后每次先扣除发送字节，再按发送间隔补预算；这个顺序影响大包和长停发间隔下的判断。

入口时间使用 `steady_clock::time_point`，截断到微秒以匹配上游 `Timestamp`。发送间隔再按 `TimeDelta::ms()` 四舍五入到毫秒，不能先对两个绝对时间取整再相减。预算每次计算的不足一字节部分直接截断。为此 `IntervalBudget` 增加 `Rounding::TruncateBytes`，ALR 使用这个模式；默认 `CarryFractionalBytes` 继续用于现有 pacer。

预算比例严格大于 start 时进入 ALR，严格小于 stop 时退出；落在两阈值间或恰好等于阈值时保持状态。更新估计码率只调整预算范围，不直接改变 ALR；下一次 `OnBytesSent` 才判断状态。没有新发送回调时状态不自动更新，也不自行启动定时器。

`GetApplicationLimitedRegionStartTime()` 返回当前 ALR 的开始时间，非 ALR 时为 `nullopt`。开始时间来自本地处理时钟，默认 `steady_clock::now()`，可以通过 `AlrDetector(Config, Clock)` 注入时钟；它不是触发包携带的发送时间。进入后保持这个时间，退出时清空，再次进入时重新取时钟。状态切换通过现有 `UAV_LOG_INFO` 记录。

类同步执行，拥有者负责串行调用。发送时间在微秒精度下倒退会抛出 `invalid_argument`，该包不改变状态；相同发送时间允许重复调用，消耗字节而不增加预算。超大字节输入安全饱和到最大欠账。ICE generation/路由更换时，拥有者应重建检测器并设置当前估计码率。

## 与确认吞吐估计器配合

下面保留上游 [GoogCcNetworkController](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/goog_cc_network_control.cc) 的调用时机：发送时更新 ALR 状态，反馈处理时检测退出并通知确认吞吐估计器。

```cpp
#include "webrtc/congestion_control/alr_detector.h"
#include "webrtc/congestion_control/acknowledged_bitrate_estimator.h"

webrtc::AlrDetector alr;
webrtc::AcknowledgedBitrateEstimator acknowledged;
bool previously_in_alr = false;

void setEstimatedBitrate(std::uint64_t target_bps)
{
    alr.SetEstimatedBitrate(target_bps);
}

void onSentPacket(const webrtc::SentPacket& packet)
{
    if (!packet.send_time) return;
    alr.OnBytesSent(packet.size, *packet.send_time);
    acknowledged.SetAlr(alr.GetApplicationLimitedRegionStartTime().has_value());
}

void onFeedback(const webrtc::TransportPacketsFeedback& feedback)
{
    const bool in_alr = alr.GetApplicationLimitedRegionStartTime().has_value();
    if (previously_in_alr && !in_alr) {
        acknowledged.SetAlrEndedTime(feedback.feedback_time);
    }
    previously_in_alr = in_alr;
    acknowledged.IncomingPacketFeedbackVector(feedback.SortedByReceiveTime());
}
```

`OnBytesSent` 的数据来自实际发送路径中的 `SentPacket`，在 pacer 排队后计数，不能等 TWCC 确认后再补算发送。`SetEstimatedBitrate` 使用控制器的带宽估计，不直接拿 pacing factor 放大后的速率代替。类提供检测状态，具体发送/反馈事件连接由下层 transport/controller 统一管理。

## 验证

`alr_detector_test` 覆盖原版发送流量、低流量进入、短峰值保持、持续发送退出、估计码率变化、自定义阈值、严格阈值边界、本地处理时钟、首包忽略、先扣再补、毫秒/速率两级取整、低速率、非法输入、极端时间/字节，以及与 `AcknowledgedBitrateEstimator` 的 ALR/快速调整状态传递。

用原版 `.cc` 的 ALR 和 IntervalBudget 函数生成固定回放期望值，只适配时间单位、环境和日志依赖。实际默认、基础、自定义三套参数各回放 1200 次发送，对全部状态和开始时间计算顺序哈希，验证 3600 次输入，包含发送间隔抖动、停发、调速和流量突发。`interval_budget_test` 同时验证新截断模式与原有小数字节累计模式；`pacer_test` 验证原有调度行为。
