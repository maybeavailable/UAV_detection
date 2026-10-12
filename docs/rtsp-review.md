# RTSP 实现检查（2026-10-12）

最初检查仅覆盖 `src/rtsp/` 及其使用的 RTP/RTCP 公共代码的功能、协议一致性和运行稳定性，不检查网络安全。下方保留修复前的原始问题、行号和复现证据；随后已根据用户要求实施修复。验证环境为 Linux x86_64、仓库 Boost 头文件、真实本地 TCP/UDP socket、FFmpeg、AddressSanitizer/UndefinedBehaviorSanitizer；未进行板端或真实硬件编码器联调。

修复前结论：基本 RTSP 拉流可工作，但异步生命周期、时钟、RTCP、媒体源选择和慢客户端处理存在确定问题。

## 修复状态（2026-10-12）

| 编号 | 修复 | 回归覆盖 |
| --- | --- | --- |
| R1 | shared_ptr 管理后启动接收，弱引用回调与独立缓冲 | 已完成 RTCP 收包回调排队后析构，ASan |
| R2 | 共享服务器状态、弱引用 accept，start/stop 幂等 | 重复启动、成功 accept 排队后析构，ASan |
| R3 | 生产者入口与 TCP 写队列均有字节/包数上限，超限关闭会话 | 暂停 I/O 时大量投递、慢客户端与正常客户端并行 |
| R4 | 修正 RR/SR 偏移并遵循 RC，处理 padding | RR/SR、多块声明/截断、TCP/UDP 反馈回调 |
| R5 | optional PTS 区分 0 与缺省，统一初始映射 | 0/40000/80000、缺省切换、大 PTS |
| R6 | SETUP URI 绑定共享源，DESCRIBE 不改变归属 | DESCRIBE A → SETUP B、播放中查询 A、拒绝不兼容路径 |
| R7 | SDP 声明 packetization-mode=1 | SDP 检查、FU-A 还原、FFmpeg 解码 |
| R8 | 订阅在图像边界同步，返回真实首包序号；不声明虚构 rtptime/Range | 初次、重复与恢复 PLAY 的首包序号，TCP/UDP |
| R9 | 按 slice header 与参数集组织完整访问单元 | 每图像 4 切片、B 帧、AUD、SPS/分辨率变化 |
| R10 | 统一累计时间基、验证有限帧率、可中断等待 | 25/29.97/0.5fps、无效帧率、低帧率析构 |
| R11 | 载荷字节计数、高精度 NTP/RTT、同采样时刻映射、SDES CNAME | SR 字段、复合包、时间外推、亚秒 RTT 与回绕 |

运行方式和当前模块约束见 [RTSP 文档](rtsp.md)。正式主机回归、ASan/UBSan 回归与完整 aarch64 交叉编译均通过；仍未进行板端真实编码器联调。

## 修复前已确认问题

### R1 · P1：UDP 传输关闭后，已完成的 RTCP 回调仍访问已释放对象

- 位置：`src/rtsp/rtp_transport.cpp:159–170`，接收循环由构造函数第 67 行启动。
- 原因：异步回调捕获裸 `this`，接收缓冲也属于对象。关闭 socket 不能把已经完成、等待调度的成功回调变成取消回调；第 163 行注释中的保证不成立。
- 复现：连续发送两个 RTCP 数据报，执行一次接收回调，使第二次接收立即完成并排队；释放传输对象，再继续运行事件循环。
- 结果：ASan 报 `heap-use-after-free`，调用链经过 `RtpTransport::handleRtcp` 和 `RtcpPacketParser::next`。仅使用一个 I/O 线程也能复现。
- 建议：将异步接收启动移到对象由 `shared_ptr` 管理之后，明确回调与缓冲的存活期，并在关闭状态下停止重新接收。
- 证据：`build/module-review/rtsp-udp-asan.log`。

### R2 · P1：服务器析构后，成功的 accept 回调仍引用服务器

- 位置：`src/rtsp/rtspserver.cpp:152–174,198–230`。
- 原因：构造函数自动调用 `start()`，公开的 `start()` 再调用会增加 accept 操作；accept 回调捕获裸 `this`，析构只关闭 acceptor，未等待成功回调处理完毕。
- 复现：构造服务器后再调用一次公开 `start()`，连接两个客户端，运行一次事件循环，析构服务器，继续处理队列。
- 结果：ASan 在第 217 行报告 `heap-use-after-free`。该复现依赖上述启动/关闭顺序；不代表普通拉流必然崩溃。
- 建议：使启动幂等，并让停止流程等待 accept/session 回调结束，或使用具备明确生命周期的共享状态。
- 证据：`build/module-review/rtsp-server-asan.log`。

### R3 · P1：慢 TCP 客户端导致发送队列持续增长

- 位置：`src/rtsp/rtp_transport.cpp:92–99`、`src/rtsp/rtspserver.cpp:712–758`。
- 原因：每包无条件 `post`，TCP 写队列也没有字节、帧数或时延上限。客户端接收速率低于码率时，积压没有收敛机制，控制响应也排在积压媒体后面。
- 复现：约 2.5 MB/s 的测试视频源，客户端 PLAY 后停止读取 4 秒。
- 结果：监测到队列从 0 增长到 1,689 包 / 2,375,760 字节，再增长到 3,417 包 / 4,806,744 字节。这里统计的是应用写队列，不是进程 RSS。
- 建议：在异步投递入口和会话写队列两处限制在途数据；保留完整帧边界，拥塞时丢弃完整帧并重新同步，或关闭过慢会话。
- 证据：`build/module-review/rtsp-network.log`、`rtsp-network-server.log`。

### R4 · P2：RTCP RR/SR 报告块偏移错误，反馈无法匹配媒体源

- 位置：`src/common/rtcp_packet.h:84–85,100–123`，使用方为 `src/rtsp/rtp_transport.cpp:192–213`。
- 原因：解析器只跳过 4 字节公共头，payload 中仍包含发送者 SSRC。RR 报告块应从 payload 偏移 4 开始，SR 应从偏移 24 开始；当前使用 0 和 20。另外迭代未按 RC 限定报告块数量。
- 复现：RR 发送者为 `0x11223344`，报告对象为 `0xAABBCCDD`，最高序号为 99。
- 结果：被解析出的报告对象是 `0x11223344`，最高序号是 50,331,650；针对 `0xAABBCCDD` 的正常反馈回调不触发。
- 建议：修正偏移并按 RC 解析报告块，分别验证 RR 和含报告块的 SR。依据：[RFC 3550 §6.4](https://www.rfc-editor.org/rfc/rfc3550#section-6.4)。
- 证据：`build/module-review/rtsp-rtcp.log`。

### R5 · P2：PTS 从 0 开始时，首帧与后续帧使用不同时间轴

- 位置：`src/rtsp/rtp_source.cpp:80–85,131–138`；文件源第 111–113 行会传入首帧 PTS=0。
- 原因：0 被当成“没有 PTS”，首帧走随机初值自增；下一帧 PTS=40,000µs 却直接换算成 3,600。
- 结果：输入 `0,40000,80000`，本次输出 `1908841615,3600,7200`，出现巨大时间戳跳变。仅当订阅者接收到这段起始时间轴时暴露。
- 建议：将“没有 PTS”与有效的 0 分开表示，并保持统一的 RTP 初始偏移与映射。
- 证据：`build/module-review/rtsp-timestamp.log`。

### R6 · P2：SETUP 没有按请求路径选择源，可能播放错误视频

- 位置：`src/rtsp/rtspserver.cpp:532–538,555–565`。
- 原因：SETUP 虽然算出请求的 base 路径，但只在 `m_path_` 为空时采用，之后仍按之前 DESCRIBE 的 `m_path_` 查源。
- 复现：同一连接 DESCRIBE `/live/a`，然后 SETUP `/live/b/track1` 并 PLAY `/live/b`。
- 结果：服务端返回 200，却发送 A 的 SSRC 与媒体。
- 建议：按 SETUP URI 解析并绑定媒体源；与当前会话不兼容时明确拒绝。DESCRIBE 不应悄悄改变已建立传输的媒体归属。
- 证据：`build/module-review/rtsp-network.log` 中 `Cross-path SETUP`。

### R7 · P2：文件源 SDP 缺少 FU-A 所需的 packetization-mode

- 位置：`src/rtsp/file_source.cpp:39–44`、`src/rtsp/rtp_source.cpp:148–172`。
- 原因：SDP 没有 `a=fmtp:96 packetization-mode=1`，但 NAL 大于 1,400 字节时发送 FU-A。没有该参数时默认模式为 0，模式 0 不允许 FU-A；通过 RTP 内嵌发送 SPS/PPS 与声明分片模式是两件事。
- 影响：宽容的播放器可以播放，严格遵循 SDP 的客户端可能拒绝或不能解码大 NAL。
- 建议：声明与当前单 NAL/FU-A 打包策略一致的模式 1。依据：[RFC 6184 §5.4](https://www.rfc-editor.org/rfc/rfc6184#section-5.4)、[§8.1](https://www.rfc-editor.org/rfc/rfc6184#section-8.1)。
- 验证：大 NAL 的 FU-A 还原本身通过，72 包还原 99,996 字节，内容、序号、时间戳和 marker 均一致。

### R8 · P2：PLAY 响应中的 RTP-Info 永远写 0

- 位置：`src/rtsp/rtspserver.cpp:783–792`。
- 结果：实际首包序号 49,016、时间戳 10,800，响应却写 `seq=0;rtptime=0`。
- 影响：依赖 RTP-Info 确定播放起点或恢复同步的客户端获得错误信息。
- 建议：从实际将发送的首包获取序号和时间映射，不能填写占位值。依据：[RFC 2326 §12.33](https://datatracker.ietf.org/doc/html/rfc2326#section-12.33)。
- 证据：`build/module-review/rtsp-network.log`。

### R9 · P2：文件源将每个 VCL NAL 当成一帧

- 位置：`src/rtsp/file_source.cpp:95–123`。
- 原因：每个 type 1/5 NAL 都单独增加 frameIndex、推进时间戳并等待一个帧间隔；H.264 一张图像可以包含多个 slice NAL。
- 影响：多 slice 文件会被拆成错误的访问单元；同一图像的 slice 获得不同时间戳且各自置 marker，播放速度也被降低。
- 复现：生成 2 张图像、每张 4 个 slice 的文件，FFprobe 确认图像数为 2；第一张图像的 `first_mb=0,4,8,12` 四个 slice 被分别赋予四个时间戳，且 marker 全为 1。
- 建议：以完整访问单元组织帧，可通过成熟解码库的 parser 或充分验证的边界解析器处理，不能仅以 NAL 类型判定帧结束。
- 证据：`build/module-review/rtsp-file-slices.log`。

### R10 · P2：文件源帧率计算将 double 强制转换为整数

- 位置：`src/rtsp/file_source.cpp:15,85–86,111–112`。
- 原因：播放节拍使用 double，而 PTS 计算使用 `static_cast<uint64_t>(m_fps_)`。29.97fps 的节拍与按 29fps 生成的 PTS 不一致；构造函数接受 0<fps<1，这时除数变成 0。
- 影响：非整数帧率发生时间漂移，小于 1fps 会触发整数除零。
- 复现：传入构造函数允许的 0.5fps，UBSan 报 `division by zero`，ASan 记录第 112 行的 FPE，进程终止。
- 建议：使用一致的有理数或精确累加时间基，并验证有限、有效的帧率范围。
- 证据：`build/module-review/rtsp-file-fps.log`。

### R11 · P2：RTCP 发送报告和 RTT 时间精度不正确

- 位置：`src/rtsp/rtp_transport.cpp:129–130,271–305`、`src/common/rtcp_packet.h:145–161`。
- 确定的偏差：octet count 累加了整个 RTP 包，规范要求仅统计载荷；NTP 小数始终为 0，RTP 时间则直接使用最近一个媒体包的时间，二者不是同一采样时刻；RTT 的当前时间也只有整秒精度。
- 另一个协议差异：当前只发送 28 字节 SR，没有包含默认复合 RTCP 所要求的 SDES CNAME。
- 影响：带宽统计不准、同步映射产生偏差，毫秒级 RTT 结果不可靠。依据：[RFC 3550 §6.1、§6.4.1](https://www.rfc-editor.org/rfc/rfc3550#section-6.4.1)。
- 建议：用高精度墙上时间与媒体时钟建立同一时刻的映射，正确统计载荷字节并发送完整 RTCP。

## 已通过的路径与验证范围

- 本地 TCP interleaved 和 UDP 的 OPTIONS/DESCRIBE/SETUP/PLAY 握手与媒体传输。
- PAUSE/PLAY 基本恢复，TEARDOWN 后再次 PLAY 返回 454。
- 大 NAL 的 FU-A 分片还原与逐字节比对，序号连续、同帧时间戳一致、末包 marker 正确。
- FFmpeg 经 TCP、UDP 均能解出 10 帧；中途接入时观察到缺少 PPS 的初始解码错误，等参数集/关键帧后恢复，不能据此认定所有客户端均兼容。
- 未完成板端真实 VENC、长时间运行或大量真实播放器互通测试。上述本地通过项不能抵消已复现的问题。

探针放在忽略的 `build/module-review/`，包括 `rtsp_probe.cpp`、`rtsp_network_probe.py`、`rtsp_file_probe.cpp`。ASan 失败案例用于复现缺陷，退出非零是预期结果。这些探针属于修复前的复现工具；修复后的正式测试已加入 `tests/rtsp_tests.cpp`、`tests/rtsp_test_server.cpp` 与 `tests/rtsp_integration.py`，通过 `rtsp-native` 预设运行。
