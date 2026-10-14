# WebRTC 日志

WebRTC 的运行日志统一使用现有 `log/log.h` 中的 `UAV_LOG_*`，复用 LogManager 的异步队列、级别过滤和文件轮转。WebRTC 模块不自行配置或停止日志库，遵循应用的全局配置；独立测试已链接真实日志实现。

## 覆盖范围

- PeerConnection：实例创建、Offer/Answer 生成、信令和连接状态变化、SDP 提交、ICE restart、操作失败及关闭完成。失败记录包含操作名，例如 `setRemoteDescription`、`createOffer`、`createDataChannel`。
- IceAgent / IceTransport：候选收集状态、候选对提名、本地/远端端点、角色冲突后的切换、DTLS 就绪、STUN 事务失败、consent 失效、重启和关闭统计。
- DtlsTransport：握手开始、client/server 角色、指纹验证成功、摘要算法、cipher、SRTP profile、握手耗时、重传、验证失败、换代和关闭统计。
- SRTP / SRTCP：上下文和密钥代次安装、重放窗口及 SSRC/报文限制、认证或重放失败、上下文失效、重置及累计统计。
- DataChannel / SCTP：association 建立、stream 数量、DCEP OPEN、SID 和通道状态、可靠性参数、暂停/恢复、协议错误及关闭完成。

## 级别与上下文

INFO 记录正常生命周期和关键协商结果；WARN 记录可恢复或重复的错误；ERROR 记录 ICE、DTLS、SCTP 等连接失败。DEBUG 记录候选详情、STUN 检查失败、握手重传及信令操作排队情况。

日志包含模块名、实例地址、对应模块的 generation（`g=`）、ICE component（`c=`）或 DataChannel SID，日志库还会补充时间、线程 ID 和源码位置。可按实例地址查看单个对象的状态序列，再通过端点、component 和 generation 对照相关传输。ICE 在关闭时递增自身 generation 以拒绝迟到事件，其他模块可能仍记录最后活跃会话的代次。

媒体报文不逐包记录成功日志。重复的认证、重放或模块错误采用累计次数 1、2、4、8、16……采样，记录中带总失败次数；被省略的日志不影响原有错误回调和统计。开启 DEBUG 的候选、STUN 等细节日志仍应按实际排障需要使用。

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
// 完成排障后恢复：
LogManager::getInstance().setLevel(LogLevel::INFO);
```

先完成 PeerConnection / transport 的关闭，再由应用统一停止日志库。`LogManager::stop()` 不属于某个 PeerConnection 的生命周期。

已有 WebRTC 回归覆盖正常连接、失败、重启、并发、背压和关闭路径，可运行 [PeerConnection 文档](peerconnection.md) 中的构建及测试命令。应用自定义 standalone 构建也需链接 `src/log/log.cpp`、`src/log/RotateFileWriter.cpp` 和线程库。
