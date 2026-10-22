# UAV VISION 网页控制台

前端使用 Vue 3、TypeScript、Vite 和 Lucide 图标，构建后由现有 C++ Gateway 托管。板端仅需要静态构建资源，不需要 Node.js、npm 或外部 CDN。

## 构建与部署

开发主机使用 Node.js 22.12+（本次验证为 22.23.2）：

```sh
cd www
npm ci
npm run build
```

输出目录为 `www/dist`，包含独立的 JS/CSS 资源。然后构建 Gateway 或主程序：CMake 自动将资源复制到构建目录下的 `www/`。部署时一起复制 `bin/` 和同级的 `www/`、`config/`，或使用 `cmake --install`，后者将页面安装至 `share/uav-gateway/www`。

启动程序时，Gateway 会查找工作目录中的 `www/dist`、可执行文件旁的 `www`、可执行文件上一级的 `www` 和安装目录 `share/uav-gateway/www`。也可显式设置 `UAV_GATEWAY_WEB_ROOT=/绝对路径/www/dist`。将此变量显式设为空可禁用页面托管；未构建页面时 API 仍可运行，`/webrtc` 返回 503。

打开 `http://设备地址:8080/` 或 `/webrtc`，登录设备后选择单路或「连接全部」。在 `config/gateway.json` 填写 `admin_token` 和 `bind_address`，板端启动时自动读取；以 `--encode` 启动采集/编码，具体见 [WebRTC 接入](../docs/webrtc-live.md)。

## 本机开发

先启动 Gateway，并允许开发服务器的 Origin：

```sh
export UAV_GATEWAY_ORIGIN=http://127.0.0.1:5173
# 在此环境中启动实际 Gateway/主程序
```

另一个终端：

```sh
cd www
UAV_DEV_GATEWAY=http://127.0.0.1:8080 npm run dev -- --host 127.0.0.1 --port 5173 --strictPort
```

浏览器访问 `http://127.0.0.1:5173`。Vite 将 `/api` 与 `/healthz` 代理至 Gateway；生产部署直接使用同源 Gateway，无需开发代理。开发时使用与上述 Origin 完全一致的地址与端口。

## 页面与连接管理

- 可见光主通道 2566×1520、子通道 1920×1080、热成像 640×512 同屏监看，支持主次/等分布局和移动端。
- 每个视频源最多一路 WebRTC 观看；布局切换、标签页切换不会重建连接。
- 单路连接/停止/重试、全部连接/停止、全屏、原始分辨率 PNG 截图。
- 实际接收码率、帧率、RTT、丢包和码率走势来自 `RTCPeerConnection.getStats()`；尚无数据时显示「—」。网关响应为 HTTP 健康探测耗时。
- 设备状态来自 Gateway，连接记录仅记录当前页面的事件。未提供的视频源会禁用连接入口，不显示模拟画面。
- 登录凭据和访问/刷新/媒体令牌仅保存在内存，不写入浏览器存储。自动刷新登录、续期并轮换播放令牌，停止/退出时释放媒体连接和播放授权。

`src/components` 存放组件，`src/services/api.ts` 负责 Gateway 鉴权与请求，`src/services/video.ts` 负责完整 SDP 交换和媒体生命周期，`src/styles.css` 负责响应式样式。

## 验证

```sh
npm run typecheck
npm run build
```

真实浏览器集成测试位于 `tests/webrtc_live/browser_test.py`，需要 Python Playwright/Chromium、已构建的测试服务和三个实际尺寸的 H264 单帧文件。测试覆盖三路解码、授权续期与登录刷新、布局切换、截图/全屏、单路名额重用、退出与移动端布局，运行方式见 [WebRTC 验证](../docs/webrtc-live.md#验证)。
