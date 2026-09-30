# UAV_detection

无人机检测系统。当前已实现：

- **log**：线程安全的异步日志库（流式 + printf 双风格、按大小轮转、历史保留、stderr 兜底）
- **rtsp**：RTSP 拉流服务器（完整握手 + H.264 RTP 打包转发，支持 UDP / TCP interleaved）

> 其余模块（camera / encode / detect / track / gb28181 / onvif / moq / webrtc 等）为待实现的目录骨架。

## 目录结构

```
src/
├── common/        公共头（common.h 常量与公共包含、single.h 单例）
├── log/           异步日志库（log、RotateFileWriter）
├── rtsp/          RTSP 服务器（rtspserver、rtp_source、rtp_transport、file_source）
├── camera/ encode/ detect/ track/ ...   待实现模块
└── main.cpp       当前为 log 演示
third_party/       Boost 1.88 / OpenSSL
```

## 构建

```bash
cmake .          # 已内置第三方 Boost/OpenSSL 查找路径；默认 Debug(-Wall -Wextra)
make
./bin/uav_detection
```

> 新增模块的 `.cpp` 必须显式加入 `CMakeLists.txt` 的 `UAV_SOURCES`（曾因遗漏导致整个 log 模块从未被编译）。

## log 模块

### 快速开始

```cpp
#include"log/log.h"

int main()
{
    // 可选配置：必须在第一次打日志之前调用
    LogConfig cfg;
    cfg.filePath     = "logs/uav_detection.log"; // 默认值，相对进程 CWD
    cfg.maxFileSize  = 10u * 1024u * 1024u;      // 单文件上限 10MB
    cfg.maxFiles     = 10;                       // 历史文件保留数，0=不清理
    cfg.maxQueueSize = 4096;                     // 异步队列上限，溢出丢最旧并留痕
    cfg.level        = LogLevel::INFO;           // 过滤阈值
    LogManager::configure(cfg);

    // 三种写法：
    UAV_LOG_INFO << "x=" << x << " y=" << y;        // cout 风格（推荐）
    UAV_LOG_INFO_F("x=%d y=%s", x, s.c_str());      // printf 风格，编译期检查格式串
    UAV_LOG_IF(LogLevel::DEBUG, cond) << "…";       // 条件日志：cond 为假时实参不求值

    return 0;
}
```

### 要点

- 级别：`TRACE < DEBUG < INFO < WARN < ERROR < FATAL`；低于阈值的记录直接丢弃，运行期可用 `setLevel()` 调整。
- 每条日志自动带上毫秒时间戳、`文件:行号`、内核 tid。
- 进程退出时后台线程会**排空队列**，退出前的日志不丢。
- 文件打不开/写不进时自动回退 stderr，日志组件绝不会把业务进程带崩。
- printf 风格格式串必须是字面量才有编译期 `-Wformat` 检查；`%s` 需要 `.c_str()`。
- cout 风格不要写 `std::endl`（行尾分号自动换行落盘）；`std::hex` 等操纵符可用且状态不会残留。
- cout 风格下 `<<` 实参在级别被过滤时仍会求值（iostream 同款代价），热路径用 `UAV_LOG_IF`。

## RTSP 模块（拉流出口）

### 数据流

```
encode 模块产出 H.264 ──pushFrame──▶ RtpSource(源，拥有 SSRC/序号/时间戳)
                                         │ H.264 → RTP 打包一次（RFC 6184 单包/FU-A）
                                         ▼
RtspServer.registerSource("/live/uav") ── 路径表            客户端(ffmpeg/VLC)
                                         │                 OPTIONS → 200 + Public
握手时按路径查表，SDP/SSRC 由源提供     │                 DESCRIBE → 源自己的 SDP（未注册→404）
                                         │                 SETUP → 协商 UDP 或 TCP interleaved
                                         │                 PLAY → 订阅到源
                                         ▼
                             源线程产帧 → 同一份 RTP 包 fan-out 转发给所有在播客户端
```

- 一个源只打包一次（同一 SSRC/序号/时间戳），多个拉流客户端共享同一份 RTP 包；
- 每个客户端在 SETUP 时拿到自己的传输管道：**UDP**（`server_port=5004-5005`，多个会话通过 `SO_REUSEADDR` + `connect` 共享绑定）或 **TCP interleaved**（复用 8554 连接上的 `$` 帧，RTP 偶数通道 / RTCP 奇数通道）；
- 每 5 秒发一次 RTCP SR（Sender Report）保活；
- **解析客户端 RR 反馈**（丢包率/累计丢包/抖动/RTT），可用于码率自适应——见下文"RR 反馈"。

### 端口

| 用途 | 端口 | 常量（`src/common/common.h`） |
|---|---|---|
| RTSP 控制连接（TCP） | 8554 | `RTSP_PORT` |
| RTP（UDP） | 5004 | `RTSP_RTP_PORT` |
| RTCP（UDP） | 5005 | `RTSP_RTCP_PORT` |

### 快速开始

encode 模块就绪前，可用 `FileRtpSource` 循环播放 H.264 文件验证整条链路：

```bash
# 1. 生成测试文件（640x360@25fps，3 秒）
ffmpeg -f lavfi -i testsrc=duration=3:size=640x360:rate=25 \
       -c:v libx264 -pix_fmt yuv420p -f h264 test.h264
```

```cpp
#include"rtsp/rtspserver.h"
#include"rtsp/file_source.h"
#include"log/log.h"

int main()
{
    boost::asio::io_context io;

    RtspServer server(io);
    auto source = std::make_shared<FileRtpSource>("/live/uav", "test.h264", 25.0);
    server.registerSource(source);

    UAV_LOG_INFO << "RTSP server on " << RTSP_PORT;
    io.run();
    return 0;
}
```

拉流验证：

```bash
ffprobe -rtsp_transport udp rtsp://127.0.0.1:8554/live/uav            # 探测流信息
ffmpeg  -rtsp_transport udp -i rtsp://127.0.0.1:8554/live/uav -f null -   # UDP 拉流
ffmpeg  -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live/uav -f null -   # TCP 拉流
```

### 接入真实编码源

让 encode 模块的产出源继承 `RtpSource`（或直接用 `pushFrame`），注册到服务器即可：

```cpp
class EncodedSource : public RtpSource
{
public:
    std::string path() const override { return "/live/uav"; }
    std::string makeSdp(const std::string& host, int port) const override
    {
        // 按真实编码参数生成 SDP（含 a=control:track1）
    }
};

server.registerSource(std::make_shared<EncodedSource>());

// encode 模块每帧：
source->pushFrame(frameData, frameSize, ptsUs);   // Annex-B，ptsUs 微秒
```

### RR 反馈（丢包/抖动/RTT）

客户端周期性的 RTCP RR 会被解析（UDP 走 5005 收包、TCP 走 interleaved RTCP 通道），
首条以 INFO、后续以 DEBUG 落日志：

```
RTCP RR from 127.0.0.1: loss=3.90625% cumLost=5 jitter=10ms rtt=0ms
```

解析器在 `src/common/rtcp_packet.h`（`RtcpPacketParser` / `RtcpReportBlock` / `rtcpRttMs`）。
encode 模块可通过回调拿到反馈做码率自适应（回调在 io 线程触发）：

```cpp
server.setFeedbackCallback([](const RtcpFeedback& fb) {
    if (fb.lossPercent() > 3.0)
        lowerBitrate();
    else if (fb.rttMs > 200)
        adjustKeyframeInterval();
});
// 对已有/之后建立的会话与传输全部生效
```

### 已验证

- 握手回归：分片发送、管道化、双会话共享端口绑定、错误码（400/404/405/454/461/505）、TCP `$` 帧共存
- ffmpeg 实拉解码：UDP 152 帧/8 秒、TCP 121 帧/6 秒；双客户端并发 fan-out 各 134 帧/6 秒
- 抓包 FU-A 重组校验：IDR 4737 字节与源文件一致
- RR 反馈 16/16：UDP 收包解析、TCP interleaved 通道解析、SR 抓取、丢包率/累计丢包/抖动/RTT 数值精确校验、畸形 RTCP 不崩

### 已知事项 / 下一步

- `FileRtpSource` 是测试占位源；客户端"接入到出首帧"的等待取决于 IDR 间隔（测试文件 3 秒一个 IDR，真实编码建议 1~2 秒）。
- 暂不支持组播 SETUP、多 track（每路源目前通告 `track1` 一个）、RTSP 会话超时踢人。
- 注意：ffmpeg 的 RTSP 客户端默认不主动发 RR（实测拉流 12 秒无 RR）；VLC 等客户端会发。
- `src/common/rtp_packet.h` / `rtcp_packet.h` 均已实现同款解析器：
  - `RtpPacketParser`：RTP 固定头 + CSRC + 扩展头 + padding 剥离（发送路径已在用，SR 时间戳取自它）；
  - `RtcpPacketParser`：RR/SR 报告块与复合包迭代（RR 反馈在用）。
- UDP 发送满时按实时流惯例直接丢包，不阻塞编码线程。
