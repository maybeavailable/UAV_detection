# InterArrivalDelta

`src/webrtc/congestion_control/inter_arrival_delta.h/.cpp` 移植 libwebrtc `InterArrivalDelta`，与本项目 transport adapter、确认吞吐估计器采用相同修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`。

源码依据：[inter_arrival_delta.h](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/inter_arrival_delta.h)、[inter_arrival_delta.cc](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/inter_arrival_delta.cc)。调用方式参考上游 [DelayBasedBwe](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/delay_based_bwe.cc)。BSD 许可、专利声明及来源见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 分组与输出

构造时指定发送时间分组长度，上游带宽控制器使用 5 ms。包的发送时间与当前组第一个发送时间之差**严格大于**分组长度时，通常开始下一组。组内保存最大发送时间、最后接收时间、最后本地系统时间和累计字节数。发送时间早于当前组首包的包直接忽略；组内的小幅发送乱序允许存在，最大发送时间不会倒退。

接收端突发到达的包可以合并到同一组，即使发送时间已跨过分组窗口。规则保留上游边界：

- 发送时间与组内最大发送时间相同，始终归入同一组。
- 否则，需要接收间隔减发送间隔小于 0、接收间隔不大于 5 ms，并且距组首包的接收时间小于 100 ms。

只有新包开始下一组时，才可能产生前面两个完整组的差值。因此，至少需要开始第三个组，才能得到首个结果；触发计算的新包属于下一组，不计入本次差值。

`ComputeDeltas(send_time, arrival_time, system_time, packet_size, &send_delta, &arrival_delta, &size_delta)` 返回是否生成有效结果。输出分别是相邻两组的最大发送时间差、最后接收时间差和累计字节数差；时间差类型为 `chrono::microseconds`，大小差为有符号 `int`。只有返回 `true` 时才能使用输出；原版在异常检查前就会写入部分输出，本项目保留这个行为。

## 异常处理

完成组的接收时间差减去本地系统时间差，达到或超过 3 秒时，认为时钟偏移发生跳变并重置。该判断保留方向，不对差值取绝对值；接收时间和本地系统时间同步向前增长不会仅因间隔很长而重置。

组间接收时间差小于 0 时丢弃触发包，保留原来的两个组；连续 3 次检测后重置。有效的非负接收差值会清零连续计数。异常重置会丢弃触发重置的当前包，下一次调用才建立新的首组，这与上游一致。异常日志使用项目的 `UAV_LOG_WARN`，不逐包输出日志。

## 在 TWCC 链中使用

```cpp
#include "webrtc/congestion_control/inter_arrival_delta.h"
#include "webrtc/transport_adapter.h"
using namespace std::chrono_literals;

webrtc::InterArrivalDelta inter_arrival(5ms);

void onFeedback(const webrtc::TransportPacketsFeedback& feedback)
{
    for (const auto& packet : feedback.SortedByReceiveTime()) {
        webrtc::InterArrivalDelta::Delta send_delta{0}, arrival_delta{0};
        int size_delta = 0;
        if (inter_arrival.ComputeDeltas(*packet.sent_packet.send_time,
                *packet.receive_time, feedback.feedback_time, packet.sent_packet.size,
                &send_delta, &arrival_delta, &size_delta)) {
            const auto delay_delta = arrival_delta - send_delta;
            // 将组间差值交给后续 TrendlineEstimator / 延迟拥塞检测。
        }
    }
}
```

三个时间参数分别来自本地发送时间、重建的远端接收时间和本地反馈处理时间。`system_time` 应传反馈处理时间，同一批反馈中的包可以相同。无需对齐发送端与接收端时钟，只计算各自时钟内的差值。遵循上游，这里传 `sent_packet.size`，不加上确认吞吐估计器使用的 `prior_unacked_data`。

类同步执行，拥有者负责串行调用。发送/接收/系统时间使用 `steady_clock::time_point`，入口截断到微秒以匹配上游 `Timestamp` 精度；内部使用微秒计数，避免跨极端 `time_point` 做纳秒减法溢出。分组长度不能为负，允许 0；输出指针必须非空，单包字节数必须能用 `int` 表示，否则抛出 `invalid_argument`；组累计大小超出 `int` 范围时抛出 `overflow_error`，不消费该包。

本类不保存 `PacketResult`、序号或 generation，不自动筛选重传歧义、不自动订阅 transport，也不实现流超时策略。拥有者在 ICE generation、路由或流生命周期变化时重建它，必要时按自身策略过滤歧义反馈。已有 `TransportFeedbackDeltaCalculator` 仍提供逐包间隔，两者用途不同；后续延迟带宽估计使用这里的包组间隔。

[TrendlineEstimator](trendline-estimator.md) 已实现包组差值的平滑、线性拟合、自适应阈值和正常/过载/欠载检测，文档给出组合调用示例。

## 验证

`inter_arrival_delta_test` 覆盖分组边界、带符号大小差、零字节组、5 ms 突发间隔、严格 100 ms 突发上限、相同发送时间、组内乱序、旧包丢弃、连续乱序重置及恢复、零接收差、3 秒时钟偏移阈值、本地时钟回退、微秒精度、输入与数值边界，以及跨两批 TWCC 的实际 adapter 数据路径。

另用该修订版原始 `.h/.cc` 函数生成固定回放期望值，只替换时间单位和日志依赖。三种分组窗口（0、5、20 ms）各回放 600 个包，对每次有效标记及有效差值计算顺序哈希，验证 1800 次输入的行为与上游一致，包含突发、反馈批次、包乱序与时钟跳变。
