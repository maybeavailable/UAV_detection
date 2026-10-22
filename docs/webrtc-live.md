# 板端 WebRTC 实时视频

主程序已接入可见光主通道、子通道及热成像，各自拥有独立 WebRTC VENC、H264 RTP、PeerConnection 和 Gateway 信令。**每个视频源默认一个 WebRTC 订阅，三路可同时观看**；同一视频源的第二个有效拉流请求返回 409。RTSP/MoQ 继续使用各自编码器。

| 视频源 ID | VPSS 输出 | RTSP 路径（端口 8554） |
|---|---|---|
| `rgb.main` | 2566×1520 | `/live/rgb` |
| `rgb.sub` | 1920×1080 | `/live/rgb/sub` |
| `thermal.main` | 640×512 | `/live/thermal` |

## 运行

```sh
(cd www && npm ci && npm run build)
cmake --preset ascend-aarch64
cmake --build --preset ascend-aarch64 -j2

# 在 config/gateway.json 中填写 admin_token；bind_address 默认 0.0.0.0。
# 在板卡启动时自动读取，无需 export；可使用 --gateway-config 指定其他文件。
./build/ascend-aarch64/bin/uav_detection --encode
```

将构建目录中的 `config/`、`www/` 与 `bin/` 一起部署，或设置 `UAV_GATEWAY_WEB_ROOT` 指向构建后的 `www/dist`。浏览器打开 `http://设备地址:8080/` 或 `/webrtc`，输入管理员或观看凭据，点击「连接全部」即可在一个页面同时观看三路，也可分别连接。控制台支持布局切换、截图、全屏和实时统计；开发说明见 [www/DEVELOPMENT.md](../www/DEVELOPMENT.md)。HTTPS 由已有 `UAV_GATEWAY_TLS_CERT` / `UAV_GATEWAY_TLS_KEY` 配置；页面和信令沿用 Gateway 的同源/CORS 策略。

三路 VPSS 输出始终保留上述尺寸；RGB 相机/VI 的传感器输入仍为原来的 2688×1520，主通道在 VPSS 输出 2566×1520。`--encode` 为每个输出建立 RTSP、WebRTC、MoQ 三类编码，共占用九个独立 VENC 通道，起始通道为 0/3/6，各组使用连续三个通道。无订阅的协议不接收原始帧。硬件使用 H264 Baseline，编码目标最高 30 fps。每个源的三类编码共享同一份自有 DVPP 输入缓冲，客户端增加不再创建编码器。

信令支持 Baseline/Constrained Baseline、`packetization-mode=1`。客户端须声明足够的 H264 接收级别，或按 RFC 6184 提供足够的 `max-fs` / `max-mbps`；不足时返回 406，保留源分辨率。控制台根据播放入口返回的约束声明接收尺寸，实际能否解码还取决于浏览器解码器。

未填写启动配置的 `admin_token`（且无环境变量凭据）或构建时 `UAV_GATEWAY=OFF`，RTSP 仍可使用，WebRTC 网页/信令入口不会启动。独立 `uav_gateway` / `--gateway-only` 不连接板端采集，不能单独提供实时视频。

## 信令接口

所有 JSON 接口使用 Gateway 的 `{"ok":true,"data":...}` envelope。

1. 使用登录访问令牌或部署的观看凭据，调用 `POST /api/v1/playback`：

   ```json
   {"stream_id":"rgb.main","protocol":"webrtc"}
   ```

   返回 `media_token`、播放授权 `session_id` 和相对入口 `/api/v1/webrtc/rgb.main`。

2. 浏览器创建一个 `recvonly` 视频 transceiver，生成 Offer，为 H264 声明所选流的接收能力（播放参数含 `max_fs` / `max_mbps`），执行 `setLocalDescription`，等待 `iceGatheringState=complete`。向上一步返回的入口 POST：

   ```json
   {"sdp":"完整的 SDP Offer"}
   ```

   请求头使用 `Authorization: Bearer <media_token>`。成功返回 201，data 包含 `type:"answer"`、`sdp` 和 **WebRTC 媒体会话** `session_id`。把 Answer 交给浏览器 `setRemoteDescription` 即可。播放授权 ID 和媒体会话 ID 是两个不同的 ID。

3. 停止时 `DELETE /api/v1/webrtc/rgb.main/<媒体会话 ID>`，使用当前有效的媒体令牌。该会话只接受创建它的播放授权。订阅名额立即释放，底层异步完成关闭。

4. 使用登录访问令牌调用 `POST /api/v1/playback/<播放授权 ID>/renew`，应在授权到期前续期（控制台在有效期约 60% 时执行），并保存返回的新 `media_token`。续期轮换令牌不会打断已有媒体会话；后续 HTTP 请求必须使用新令牌。撤销播放授权、登录退出、禁用设备或授权过期会关闭相关 WebRTC 会话。

本接口是一次完整 SDP 交换，不是完整 WHEP 实现，不提供 PATCH trickle 或原会话的 ICE restart；重连时关闭旧媒体会话并重新 POST Offer。服务端等待本地候选收集完成再返回 Answer；默认协商超时 8 秒、Answer 后未连接超时 20 秒、断开宽限 15 秒。HTTP 请求不阻塞 Gateway 网络线程。

## 媒体与反馈

```text
任一 VPSS 输出 → 该源帧池 → 该源 WebRTC EncoderSession → MediaSubscription
  → VideoSender → H264 RTP → PeerConnection → Pacer/DTLS-SRTP → ICE/UDP → 浏览器
浏览器 RTCP/TWCC → transport GCC → 该路编码预算
浏览器 PLI/FIR → 该路编码器请求 IDR
```

Answer 使用 Offer 中协商的 PT、TWCC 扩展 ID，并声明发送 SSRC/MSID。当前只发送视频，不开启 DataChannel 或音频。GCC 限制与编码的 min/start/max 码率一致，默认 30 kbit/s / 300 kbit/s / 5 Mbit/s；慢客户端和发送失败沿用订阅队列的 IDR 恢复。

默认使用本机 ICE host 地址，适用于可达的局域网；本次没有增加 TURN、端口映射或公网穿透服务。原 ICE 层的 STUN 端点可通过 C++ `VideoService::Config::peer` 配置。浏览器候选收集和接收使用标准 [WebRTC API](https://webrtc.org/getting-started/peer-connections)，H264 参数约束见 [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184.html)。

## 验证

```sh
cmake -S tests/webrtc_live -B /tmp/uav-webrtc-live \
  -DSRTP_ROOT=/path/to/native/deps -DUSRSCTP_ROOT=/path/to/native/deps
cmake --build /tmp/uav-webrtc-live -j2
ctest --test-dir /tmp/uav-webrtc-live --output-on-failure
```

集成测试运行真实 Gateway HTTP、完整 SDP、UDP ICE、指纹验证、DTLS/SRTP、H264 RTP 和 PLI；仅用可控编码后端替代硬件。覆盖三路并发、逐源单路名额、错误鉴权、分辨率接收能力不足、授权续期/轮换/撤销、连接超时和停止/重启。

可选浏览器验证使用 FFmpeg 生成三个实际分辨率的合法访问单元，以真实 Chromium 检查同屏解码、授权/登录续期、布局、截图、全屏、单路占用与重连以及移动端布局：

```sh
(cd www && npm ci && npm run build)
ffmpeg -f lavfi -i testsrc=size=2566x1520:rate=30 -frames:v 1 -pix_fmt yuv420p \
  -c:v libx264 -profile:v baseline -level:v 5.0 -x264-params keyint=1 -f h264 /tmp/main.h264
ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30 -frames:v 1 -pix_fmt yuv420p \
  -c:v libx264 -profile:v baseline -level:v 4.0 -x264-params keyint=1 -f h264 /tmp/sub.h264
ffmpeg -f lavfi -i testsrc=size=640x512:rate=30 -frames:v 1 -pix_fmt yuv420p \
  -c:v libx264 -profile:v baseline -level:v 3.1 -x264-params keyint=1 -f h264 /tmp/thermal.h264
python3 tests/webrtc_live/browser_test.py /tmp/uav-webrtc-live/webrtc_live_test \
  /tmp/main.h264 /tmp/sub.h264 /tmp/thermal.h264
```

测试使用短授权有效期来验证令牌轮换；不改变生产默认值。截图输出到 `/tmp/uav-console-*.png`。

浏览器测试需要 Playwright 与其 Chromium。主机验证不代替板卡相机、VENC 资源及实际网络环境的验证。
