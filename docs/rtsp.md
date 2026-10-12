# RTSP 拉流模块

实现位于 `src/rtsp/`，每个媒体源提供一路 H.264 视频，客户端可直接通过 RTSP 连接服务器。默认端口为 8554；支持 TCP interleaved 与 UDP unicast，以及 OPTIONS、DESCRIBE、SETUP、PLAY、PAUSE、TEARDOWN 和 GET_PARAMETER。此文档描述模块接口，不表示主程序已经接入硬件编码器。

## 接入

```cpp
boost::asio::io_context io;
RtspServer server(io);                  // 构造时启动监听，start() 重复调用无副作用
server.registerSource(encodedSource);   // shared_ptr<RtpSource>，例如 /live/uav
io.run();
```

`RtspServer(io, 0)` 可由系统分配监听端口，`port()` 返回实际端口。`stop()` 关闭监听和所有会话，可重复调用；停止后不重新开启监听，需要创建新的服务器。

服务器及会话的生命周期、源注册、回调配置与 RTCP 处理，应在同一个 I/O 线程执行，或在事件循环启动之前/停止之后执行。若应用运行多个 I/O 工作线程，应为此模块提供独立的单线程 `io_context`。`pushFrame()` 由单一编码生产者线程调用；订阅、退订会与访问单元边界同步。关闭服务器后，已完成但尚未调度的 accept/RTCP 回调也不会访问已销毁的服务器或传输对象。

真实编码源继承 `RtpSource`，实现 `path()` 与 `makeSdp()`。每次 `pushFrame()` 输入**一个完整图像访问单元**，而非任意字节块或单个切片：

```cpp
source->pushFrame(annexB, size, 0);       // 第一张图像，0 是有效 PTS
source->pushFrame(annexB, size, 40000);   // 下一张图像，单位为微秒
source->pushFrame(annexB, size);          // 未提供 PTS 时按 25fps 合成
```

PTS 保持随机 RTP 初始偏移，按媒体时钟换算，支持 32 位 RTP 时间戳自然回绕。省略 PTS 与显式传入 0 的含义不同；切换到合成时间再恢复显式 PTS 时，会建立新的连续映射。默认 H.264 时钟为 90kHz，SDP 应包含：

```sdp
m=video 0 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1
a=control:track1
```

单 NAL 与 FU-A 分片共用同一图像时间戳，仅访问单元末包置 marker。SPS/PPS 可通过媒体内嵌发送，无需在 SDP 写死参数集。

## 会话与拥塞行为

- SETUP 按 `/媒体源路径/track1` 绑定实际源。DESCRIBE 只获取描述，不改变已有会话。已有传输时切换其他源返回 455；先 TEARDOWN 再 SETUP 可切换源。播放中 re-SETUP 返回 455，暂停后可重新协商当前源。
- PLAY 的 `RTP-Info` 返回在访问单元边界订阅后的真实首包序号。直播不声明固定的 NPT 起点，因此不发送占位 `rtptime=0` 或 `Range: npt=0.000-`。响应先进入 TCP 写队列，媒体随后进入。
- PAUSE、TEARDOWN、断开和队列溢出均退订媒体源；TEARDOWN 后再次 PLAY 返回 454，需重新 SETUP。
- 每个传输的生产者入口默认最多缓存 2MiB / 4096 包；每个 TCP 会话的写队列也最多缓存 2MiB / 4096 项。入口每批最多处理 64 包，只保持一个后续发送任务。超过任一上限即关闭该慢客户端，不影响其他会话。上限不包括操作系统 socket 缓冲及共享媒体源的内存。
- UDP socket 使用非阻塞发送；发送失败的包不计入发送统计。

单独创建 `RtpTransport` 时，必须先放入 `shared_ptr`，再调用 `start()` 启动 RTCP 接收。`setPlaying(true)` 也会调用 `start()`。SETUP 已自动完成这些步骤。

## RTCP

RR/SR 按各自发送者字段长度与 RC 解析报告块，过滤到本媒体源 SSRC 后触发反馈。`lastFeedback()` 可跨线程读取最近反馈。RTT 使用 NTP 中间 32 位的亚秒精度和回绕运算。

发送端默认每 5 秒发送 SR + SDES CNAME 复合包；SR 的 octet count 仅统计 RTP 载荷。NTP 保留小数，RTP 时间从最近成功交付的媒体包按媒体时钟外推到报告采样时刻。此映射以本模块的发送时刻为基准；需要多设备采集时钟同步时，还需在编码/采集入口统一 PTS 与时钟源。

## 文件测试源

`FileRtpSource(path, file, fps)` 循环播放 Annex-B AVC 文件。内部解析 SPS/PPS、slice header、frame_num、场/POC 与 IDR 字段，将同一图像的多个切片归并为完整访问单元；IDR 携带当前已知的参数集，并保留访问单元原有的参数集/SEI 顺序，不预读后续参数集覆盖前面的图像。支持 AUD；未支持的扩展 NAL 或无效头会记日志并停止该文件源。

帧率必须有限且位于 `[1e-6, 1e6]`，支持 29.97、0.5 等非整数值。PTS 与发送节拍使用同一累计时间基，等待可被析构中断。文件源按访问单元顺序和传入帧率合成时间，不恢复容器的 DTS/PTS，也不提供 seek；建议生成无 B 帧测试文件：

```sh
ffmpeg -f lavfi -i testsrc2=duration=3:size=640x360:rate=25 \
  -c:v libx264 -bf 0 -pix_fmt yuv420p -f h264 test.h264
```

## 回归测试

主机测试仅依赖 C++17、仓库 Boost 头文件、线程库、Python3，以及提供 libx264 编码器的 FFmpeg，不需要 CANN 或 SIP 库：

```sh
cmake --preset rtsp-native
cmake --build --preset rtsp-native -j4
ctest --preset rtsp-native
```

`tests/rtsp_tests.cpp` 验证时间戳、FU-A、RTCP、队列上限、关闭生命周期、多切片与非整数帧率；`tests/rtsp_integration.py` 使用真实 TCP/UDP socket 验证会话流程、源绑定、首包序号、RTCP 反馈、慢客户端隔离，以及 FFmpeg 双传输解码。额外验证 B 帧、AUD、SPS/分辨率变化的访问单元分组与重组解码。

内存与未定义行为检查：

```sh
cmake -S . -B build/rtsp-sanitize -DUAV_GATEWAY=OFF \
  -DUAV_RTSP_ONLY=ON -DUAV_RTSP_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build build/rtsp-sanitize -j3
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/rtsp-sanitize --output-on-failure
```

协议依据：[RFC 2326 §12.33](https://www.rfc-editor.org/rfc/rfc2326#section-12.33)、[RFC 3550](https://www.rfc-editor.org/rfc/rfc3550)、[RFC 6184](https://www.rfc-editor.org/rfc/rfc6184)。主机回归不覆盖板端 VENC、长期运行和所有播放器的互通。
