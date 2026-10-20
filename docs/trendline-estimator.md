# TrendlineEstimator

`src/webrtc/congestion_control/trendline_estimator.h/.cpp` 参考与已有 transport adapter、InterArrivalDelta 等组件相同的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`。

源码依据：[trendline_estimator.h](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/trendline_estimator.h)、[trendline_estimator.cc](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/trendline_estimator.cc)。BSD 许可、专利声明和来源记录见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 输入和算法

`Update(recv_delta_ms, send_delta_ms, send_time_ms, arrival_time_ms, packet_size, calculated_deltas)` 保留原版接口和单位。前两个参数是 `InterArrivalDelta` 输出的包组间隔，以 double 毫秒表示，保留亚毫秒精度；后两个时间是当前触发包的发送/接收时间，按上游 `Timestamp::ms()` 四舍五入到整数毫秒。发送时钟和接收时钟无需对齐，各自在自己的时间轴内取差值。

有效差值按以下过程更新：

1. 累加 `recv_delta_ms - send_delta_ms`，得到原始累计延迟变化。
2. 用 `smoothed = 0.9 * previous + 0.1 * accumulated` 平滑。
3. 保存最近窗口的接收时间、平滑值和原始累计值。拟合横轴是相对于首个有效样本的接收时间，纵轴是平滑延迟。
4. 窗口填满后用最小二乘法求斜率；接收时间完全相同、无法拟合时保留上一斜率。
5. 使用 `min(有效差值数, 60) * slope * 4` 与阈值比较。有效差值计数最多保留到 1000。

正向超过阈值时先计时：首次算半个发送间隔，后续累加完整发送间隔。只有持续时间严格超过 10 ms、计数大于 1、当前斜率不小于前一斜率，才判为过载。低于负阈值时判为欠载；位于阈值范围内时恢复正常。

阈值初始为 12.5，下降/上升调整系数分别为 0.039、0.0087，最终限制在 `[6, 600]`。每次正向自适应时间跨度最多 100 ms；趋势绝对值高于当前阈值加 15 时跳过阈值调整，避免追随大的延迟尖峰。保留上游对接收时间倒退的有符号时间差处理，排序只作用于拟合窗口；时间轴更换由拥有者重建检测器。

`packet_size` 保留接口兼容性，不用于筛选小包或给拟合加权。`calculated_deltas=false` 时不更新拟合和阈值，但仍会调用已注入的预测器。

## 配置、状态和预测器

`TrendlineEstimator::Config` 是 `TrendlineEstimatorSettings` 的别名，直接提供上游 `WebRTC-Bwe-TrendlineEstimatorSettings` 的参数，替代试验字符串解析：

| 参数 | 默认值 | 规则 |
| --- | --- | --- |
| `window_size` | 20 | 10..200；超出范围按原版退回 20 |
| `enable_sort` | false | 对窗口中新插入的接收时间样本做稳定插入排序 |
| `enable_cap` | false | 为非负拟合趋势尝试施加斜率上限 |
| `beginning_packets` | 7 | 开头选取最小原始延迟的样本数 |
| `end_packets` | 7 | 末尾选取最小原始延迟的样本数 |
| `cap_uncertainty` | 0 | 上限的附加值，范围 0..0.025 |

斜率上限根据首尾区域各自最小的原始累计延迟和对应接收时间计算；两端接收时间差小于 1 ms 时不施加上限。首尾样本数必须各至少 1，且总数不能超过窗口；非法配置按原版关闭 cap 并记录 WARN。启用 cap 时，非法或非有限的 uncertainty 回退到 0。

`State()` 返回 `BandwidthUsage::kBwNormal`、`kBwUnderusing` 或 `kBwOverusing`。`Snapshot()` 提供窗口大小、有效差值计数、累计/平滑延迟、斜率、修改后的趋势、阈值、过载计时/计数、原始判定和最终返回状态，方便诊断。至少两个有效差值前，修改后的趋势为 `nullopt`。

同时提供与上游对应的 `DelayIncreaseDetectorInterface`、`BandwidthUsage` 和 `NetworkStatePredictor` 头文件。可通过 `TrendlineEstimator(config, predictor)` 注入外部预测器；对象由调用者持有，须比检测器存活更久。每次 `Update` 都将当前包时间和原始判定交给预测器，`State()` 返回预测结果。`UpdateTrendline` 仅更新原始检测器，不更新预测器。没有注入预测器时，`State()` 返回原始判定。

## 消费 TWCC 和包组差值

```cpp
#include "webrtc/congestion_control/inter_arrival_delta.h"
#include "webrtc/congestion_control/trendline_estimator.h"
#include "webrtc/transport_adapter.h"
using namespace std::chrono_literals;

webrtc::InterArrivalDelta groups(5ms);
webrtc::TrendlineEstimator trendline;

std::int64_t milliseconds(webrtc::TransportTime at)
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count();
    return us / 1000 + (us % 1000 >= 500); // 本例使用非负时间轴
}

void onFeedback(const webrtc::TransportPacketsFeedback& feedback)
{
    for (const auto& packet : feedback.SortedByReceiveTime()) {
        webrtc::InterArrivalDelta::Delta send_delta{0}, recv_delta{0};
        int size_delta = 0;
        const bool calculated = groups.ComputeDeltas(*packet.sent_packet.send_time,
            *packet.receive_time, feedback.feedback_time, packet.sent_packet.size,
            &send_delta, &recv_delta, &size_delta);
        trendline.Update(std::chrono::duration<double, std::milli>(recv_delta).count(),
            std::chrono::duration<double, std::milli>(send_delta).count(),
            milliseconds(*packet.sent_packet.send_time), milliseconds(*packet.receive_time),
            packet.sent_packet.size, calculated);
    }
    const auto usage = trendline.State(); // 交给后续发送端带宽控制器
}
```

调用时机对齐上游 [DelayBasedBwe](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/delay_based_bwe.cc)。传入当前包大小，不是两个组的大小差，也不加 `prior_unacked_data`。两个组尚未完成时仍调用 `Update(..., false)`，由它跳过趋势处理并保留预测器调用。

类同步执行，拥有者负责串行调用、路由/ICE generation 与流超时后的重建，以及需要时的重传歧义过滤。整数时间戳必须非负；有效采样的间隔、累计值和滤波值必须有限，无效输入在更新状态前抛出 `invalid_argument`。初始化、原始状态变化、预测状态变化与配置回退使用现有日志库。

本类输出延迟使用状态，供后续带宽控制器结合确认吞吐等信息使用；transport 事件连接和目标码率控制由拥有者管理。

[DelayBasedBwe](delay-based-bwe.md) 已封装上面的包组/趋势调用顺序、2 秒流超时和可选音视频独立检测，并结合 AIMD 生成目标码率。

## 验证

`trendline_estimator_test` 覆盖原版正常/过载/欠载和小包场景、平滑与窗口预热、退化拟合、计数上限、过载计时严格边界、阈值边界及延迟尖峰、斜率上限、参数回退、预测器调用时机、虚接口和无效输入。

以原版 `.h/.cc` 函数生成固定期望值，只替换试验解析和日志依赖。六套有效配置各回放 600 次 `Update`，比较 3600 次状态的顺序哈希及状态计数，另外逐点核对 18 个采样点的斜率、阈值和状态，并比较最终累计和平滑延迟。包含分数毫秒、包到达重排、长间隔、排序、斜率上限和无效差值标记。

数据路径测试实际走 `AddPacket -> ProcessSentPacket -> TWCC -> SortedByReceiveTime -> InterArrivalDelta -> TrendlineEstimator`，分别验证正常、过载、欠载三种流量，包含跨反馈批次的状态连续性。
