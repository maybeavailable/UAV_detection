# 确认吞吐估计

`src/webrtc/congestion_control/bitrate_estimator.h/.cpp` 和 `acknowledged_bitrate_estimator.h/.cpp` 参考与 transport adapter 相同的 libwebrtc 修订版 `602d5fc23caaca7728ad5f3f0e68b92274d1a38a`。

源码依据：[BitrateEstimator](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/bitrate_estimator.cc)、[AcknowledgedBitrateEstimator](https://webrtc.googlesource.com/src/+/602d5fc23caaca7728ad5f3f0e68b92274d1a38a/modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator.cc)。BSD 许可、专利声明和来源记录见 `third_party/webrtc/LICENSE`、`PATENTS`、`NOTICE`。

## 算法和接口

`BitrateEstimator::Update(at_time, amount, in_alr)` 接受接收时间、确认字节数和应用受限状态。首次采样窗口默认 500 ms，之后默认 150 ms；到达边界的当前包计入下一个窗口。两次更新的间隔大于当前窗口时清空累计字节，保留时间余数；时间倒退时重建窗口。两者都保留已经形成的滤波估计，不主动产生零码率样本。

滤波保留上游的 float 状态、初始方差 50、每次更新的过程噪声 5。样本与当前估计偏差越大，不确定性越高，样本权重越小。`ExpectFastRateChange()` 将方差增加 200，让后续样本更快改变估计。

`bitrate()` 返回滤波后的 `optional<uint64_t>`，单位 bit/s，形成首个完整窗口前为 `nullopt`。`PeekRate()` 返回当前窗口累计字节除以累计时间的瞬时值，没有累计时间时为 `nullopt`；它包含当前包，不能作为稳定估计。时间使用 `steady_clock::time_point`，按上游 `Timestamp::ms()` 四舍五入到毫秒；码率输出沿用上游向整数 bit/s 截断的方式。

`Config` 替代项目尚未引入的 `FieldTrialsView`，对应上游 `WebRTC-BweThroughputWindowConfig`：

| 本项目配置 | 上游参数 | 默认值 |
| --- | --- | --- |
| `initial_window` | `initial_window_ms` | 500 ms |
| `window` | `window_ms` | 150 ms |
| `uncertainty_scale` | `scale` | 10 |
| `uncertainty_scale_in_alr` | `scale_alr` | 10 |
| `small_sample_uncertainty_scale` | `scale_small` | 10 |
| `small_sample_threshold` | `small_thresh` | 0 字节 |
| `uncertainty_symmetry_cap_bps` | `symmetry_cap` | 0 bit/s |
| `estimate_floor_bps` | `floor` | 0 bit/s |

两个窗口均须在 150..1000 ms，不确定性系数必须有限、非负且可用 float 表示；构造时拒绝无效配置。小样本条件是严格小于阈值，且只有下降样本使用小样本/ALR 专用系数；小样本分支优先。默认 ALR 系数与普通系数相同，因此 `SetAlr(true)` 不会冻结估计。最低码率只约束后续滤波结果，不修改首次初始化样本，与上游一致。

适配额外防止字节累计、bit/s 转换溢出，并处理零字节初始化后的零分母。超大数值使用较宽中间值，输出饱和到 `uint64_t` 上限；正常输入保留上游 float 运算。没有移植该修订版工厂默认选择的 `RobustThroughputEstimator`，这里明确实现用户指定的 `AcknowledgedBitrateEstimator` 算法。

## 消费 transport feedback

```cpp
#include "webrtc/congestion_control/acknowledged_bitrate_estimator.h"

webrtc::AcknowledgedBitrateEstimator acknowledged;

// 在 transport/controller 的串行上下文中，每个反馈只消费一次。
void onFeedback(const webrtc::TransportPacketsFeedback& feedback)
{
    acknowledged.IncomingPacketFeedbackVector(feedback.SortedByReceiveTime());
    if (const auto bps = acknowledged.bitrate()) {
        // 将 *bps 交给后续发送端带宽控制器，单位 bit/s。
    }
}
```

`IncomingPacketFeedbackVector` 要求已收到且有发送信息的包，依次按接收时间、发送时间、transport sequence 排序。`SortedByReceiveTime()` 提供这个输入，并过滤未收到/未发送的记录。输入无效或单包的两个大小字段相加溢出时，整批抛出 `invalid_argument`，不会先消费前面的包。估计器不负责去重；transport adapter 会移除已经确认的历史，调用者不要重复投递同一批结果。

每包确认量是 `sent_packet.size + prior_unacked_data`，包含 adapter 配置的网络开销和只参与带宽分配的先行流量；不使用 `wire_size` 代替。时间使用重建的 `receive_time`，不使用反馈包抵达发送端的时间。带有重传接收时间歧义的确认包仍按上游计入吞吐。

进入 ALR 调用 `SetAlr(true)`；退出时分别调用 `SetAlr(false)` 和 `SetAlrEndedTime(local_time)`。后者记录本地发送时钟的结束时间，收到首个 `send_time > local_time` 的确认时，先执行一次 `ExpectFastRateChange()`，再消费该包；这个状态可跨反馈批次保留。仅调用 `SetAlr(false)` 不会自动设置结束时间。

[AlrDetector](alr-detector.md) 根据发送字节和带宽估计提供 ALR 状态，文档中给出与本类配合的发送/反馈调用时机。

[LinkCapacityEstimator](link-capacity-estimator.md) 在控制器检测到过载时消费确认吞吐，结合探测结果维护容量均值和上下界。

[DelayBasedBwe](delay-based-bwe.md) 已组合包组差值、Trendline 和 AIMD，可将本类的 `bitrate()` 作为确认吞吐输入，得到延迟带宽目标。

类本身同步执行，不自行加锁、不注册 transport 回调。ICE generation/路由更换时，拥有者应重建估计器，防止不同路径的时间和吞吐样本混合。这是后续发送端带宽控制器的输入，尚未据此自动修改 pacer 或编码器码率。

## 验证

`bitrate_estimator_test` 覆盖窗口边界、毫秒舍入、停发与时间倒退、ALR/小样本优先级、最低码率、快速收敛、零值和数值边界。固定序列通过原版 `.cc` 的采样和滤波函数生成期望值，三种场景各回放 400 次更新，检查 36 个采样点的估计及瞬时码率，场景包含带宽升降、ALR、定制配置、长间隔和时间回退。

`acknowledged_bitrate_estimator_test` 验证接收时间及字节转交、跨批次 ALR 退出、严格时间比较、无效输入原子性、实际滤波结果，以及 `AddPacket -> ProcessSentPacket -> TWCC -> SortedByReceiveTime -> estimator` 的完整数据路径，包含网络开销、先行未反馈字节和重复确认。
