# WebRTC 日志

WebRTC 的运行日志统一使用现有 `log/log.h` 中的 `UAV_LOG_*`，复用 LogManager 的异步队列、级别过滤和文件轮转。WebRTC 模块不自行配置或停止日志库，遵循应用的全局配置；独立测试已链接真实日志实现。

## 覆盖范围

- PeerConnection：实例创建、Offer/Answer 生成、信令和连接状态变化、SDP 提交、ICE restart、操作失败及关闭完成。失败记录包含操作名，例如 `setRemoteDescription`、`createOffer`、`createDataChannel`。
- IceAgent / IceTransport：候选收集状态、候选对提名、本地/远端端点、角色冲突后的切换、DTLS 就绪、STUN 事务失败、consent 失效、重启和关闭统计。
- DtlsTransport：握手开始、client/server 角色、指纹验证成功、摘要算法、cipher、SRTP profile、握手耗时、重传、验证失败、换代和关闭统计。
- SRTP / SRTCP：上下文和密钥代次安装、重放窗口及 SSRC/报文限制、认证或重放失败、上下文失效、重置及累计统计。
- DataChannel / SCTP：association 建立、stream 数量、DCEP OPEN、SID 和通道状态、可靠性参数、暂停/恢复、协议错误及关闭完成。
- TransportAdapter / TransportDelta：发送历史限制、路由和 generation 变化、历史过期、发送失败、TWCC 状态匹配、历史缺失、旧路由过滤、首次丢失与恢复、重传时间歧义，以及发送/接收/延迟间隔。
- Pacer / IntervalBudget：速率配置、暂停和恢复、排队拒绝原因、过期、清空、排队等待、队列排空速率调整、每轮预算补充及逐包消耗。
- AcknowledgedBitrateEstimator / BitrateEstimator / AlrDetector：反馈批次、采样窗口、ACK 码率、时钟回退和采样中断、ALR 进入/退出及结束后的快速估计。
- InterArrivalDelta / TrendlineEstimator / LinkCapacityEstimator：乱序忽略、分组重置、收发间隔、延迟趋势、阈值、拥塞状态，以及链路容量样本和上下界。
- AimdRateControl / DelayBasedBwe / LossBasedBweV2：目标码率增减、RTT、ALR 限制、探测结果采用/拒绝、音视频检测器切换、丢包观测、候选评分、上下界和 HOLD 决策。
- ProbeController / ProbeBitrateEstimator：探测配置、网络可用性、ALR/周期/恢复探测触发、受限原因、探测簇参数、80% 门限进度、收发速率、估计结果提取及簇过期。纯数据结构和接口头文件通过使用它们的实现记录日志。
- GoogCC / SendSideBwe / CwndPushback：总控初始化、路由切换、目标码率与回压目标、丢帧比例、RTT/RR、反馈批次、拥塞窗口及填充比；见 [GoogCC 总控](goog-cc-network-control.md)。
- GCC Transport / BitrateProber：下层自动接入启动、探测簇排队、完成和迟到丢弃；探测及 padding 的实际发送计数可结合 transport 快照查看。

## 级别与上下文

INFO 记录正常生命周期和关键协商结果；WARN 记录可恢复或重复的错误；ERROR 记录 ICE、DTLS、SCTP 等连接失败。DEBUG 记录候选详情、STUN 检查失败、握手重传及信令操作排队情况。

日志包含模块名、实例地址、对应模块的 generation（`g=`）、ICE component（`c=`）或 DataChannel SID，日志库还会补充时间、线程 ID 和源码位置。可按实例地址查看单个对象的状态序列，再通过端点、component 和 generation 对照相关传输。ICE 在关闭时递增自身 generation 以拒绝迟到事件，其他模块可能仍记录最后活跃会话的代次。

默认 INFO 不逐包记录成功日志。拥塞控制的 DEBUG 记录反馈批次、码率变化及决策；TRACE 记录逐包发送时间、TWCC 状态、收发间隔、Pacer/ALR 字节预算和趋势样本。高频 DEBUG/TRACE 在构造消息、格式化和读取诊断值之前检查 `shouldLog()`，关闭时跳过消息表达式。重复的认证、重放或模块错误采用累计次数 1、2、4、8、16……采样，记录中带总失败次数；被省略的日志不影响原有错误回调和统计。DEBUG/TRACE 应按排障需要短时开启，TRACE 数据量大，可能触发全局日志队列丢弃和文件轮转。

时间字段以 `_us` / `_ms` 标明单位，速率使用 `_bps`，长度使用 `_bytes`。`send_us` 来自本地 steady clock；TWCC 的 `remote_arrival_us` 属于接收端时钟域，不能与本地发送时间相减解释为单向延迟。`TransportDelta` 输出两个时钟域各自的相邻包间隔及其差值。

ICE 密码和 ufrag、DTLS 私钥、SRTP master key/salt、密钥摘要、完整 SDP、STUN 原始报文以及应用/媒体消息内容不写入日志。DataChannel label/protocol 仅记录字节长度。指纹只记录验证结果和摘要算法，媒体错误只记录错误码、长度和累计计数。

## 配置

在应用创建任何日志对象或 WebRTC 对象之前配置日志库：

```cpp
#include "log/log.h"

LogConfig logging;
logging.filePath = "logs/uav_detection.log";
logging.level = LogLevel::INFO;
logging.maxFileSize = 10 * 1024 * 1024;
logging.maxFiles = 10;
LogManager::configure(logging);
```

临时排障可切换级别，影响应用中所有使用该日志库的模块：

```cpp
LogManager::getInstance().setLevel(LogLevel::DEBUG);
// 需要逐包时间、预算、趋势时短时开启：
LogManager::getInstance().setLevel(LogLevel::TRACE);
// 完成排障后恢复：
LogManager::getInstance().setLevel(LogLevel::INFO);
```

先完成 PeerConnection / transport 的关闭，再由应用统一停止日志库。`LogManager::stop()` 不属于某个 PeerConnection 的生命周期。

已有 WebRTC 回归覆盖正常连接、失败、重启、并发、背压和关闭路径，可运行 [PeerConnection 文档](peerconnection.md) 中的构建及测试命令。应用自定义 standalone 构建也需链接 `src/log/log.cpp`、`src/log/RotateFileWriter.cpp` 和线程库。
