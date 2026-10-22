# Gateway 后端与网页托管

Gateway 提供网页/API 接入、HTTP/HTTPS、WebSocket/WSS、登录会话与权限、设备/码流配置持久化、异步控制命令和播放授权。网页静态资源使用同一端口托管。前端控制台位于 `www`，使用 Vue 3、TypeScript 和 Vite。开发与部署见 [前端说明](../www/DEVELOPMENT.md)。

板端 `uav_detection --encode` 同时启用 Gateway 时，已接入 WebRTC 实时视频，控制台入口为 `/` 或 `/webrtc`。采集/编码、信令接口和播放方式见 [WebRTC 实时视频](webrtc-live.md)。独立 Gateway 仍须应用注入真实媒体适配器。

GB28181 和 ONVIF 是并列的原生协议入口，不经过 Gateway，也不使用 Gateway Token。国标自行 SIP 注册、查询与点播；ONVIF 自行发现并处理 SOAP 服务请求。当前国标模块尚未在主程序启动，ONVIF 尚待实现，详见 [整体架构](architecture.md)。

## 构建、运行与安装

Gateway 使用 C++17、仓库的 Boost 头文件、OpenSSL 和系统线程库。本机独立构建不依赖 Ascend、SIP 或 libxml2；交叉编译使用仓库的目标架构 OpenSSL。

```bash
(cd www && npm ci && npm run build)
cmake --preset gateway-native
cmake --build --preset gateway-native -j2
ctest --preset gateway-native

# 先在 config/gateway.json 中填写 admin_token，程序启动时自动读取。
./build/gateway-native/bin/uav_gateway \
  --config config/gateway.example.json \
  --state-file data/gateway-state.json \
  --web-root www/dist
```

未指定网页目录时，自动查找工作目录中的 `www/dist`、可执行文件旁或上一级的 `www`、安装目录 `share/uav-gateway/www`。CMake 构建会同步已有的 `www/dist` 到构建目录 `www/`；先构建前端再构建 C++。找不到页面时根路径返回服务说明，`/webrtc` 返回 503。显式设置 `UAV_GATEWAY_WEB_ROOT=` 可禁用托管。监听地址与端口由 `config/gateway.json` 配置，也可通过 `--bind` / `--port` 临时覆盖。未设置管理员令牌时拒绝启动，凭据不通过命令行参数或日志输出。

启动配置文件 `config/gateway.json`：

```json
{
  "admin_token": "<接入凭据>",
  "viewer_token": "",
  "bind_address": "0.0.0.0",
  "port": 8080
}
```

仓库中的 `admin_token` 初始为空，填入实际凭据后即可启用 Gateway，无需再 export 接入凭据或监听地址；可选的 `viewer_token` 必须与管理员不同。主程序直接运行 `./build/ascend-aarch64/bin/uav_detection --encode`。需要指定其他配置文件时，使用 `--gateway-config /path/to/gateway.json`，主程序、独立 `uav_gateway` 和 `--gateway-only` 均支持。

自动查找顺序为：工作目录的 `config/gateway.json`、可执行文件旁的 `config/gateway.json`、可执行文件上一级的 `config/gateway.json`、安装目录的 `etc/uav-gateway/gateway.json`。读取第一个存在的文件；显式指定的文件缺失、JSON 无效或字段类型不符会报错，不会忽略错误后改用默认凭据。没有文件时保留原环境变量方式。启动配置与 `--config` 的设备/码流目录是两个不同文件，启动配置不会写入持久化设备状态，也不能放在网页根目录中。

优先级为：命令行监听选项 > 对应环境变量 > 启动配置文件 > 内置默认值。内置默认监听 `127.0.0.1:8080`，仓库提供的配置文件设为 `0.0.0.0:8080`。配置文件中的端口允许 0，表示自动分配端口。

HTTPS/WSS 使用同一监听端口，配置证书链与未加密的 PEM 私钥：

```bash
./build/gateway-native/bin/uav_gateway \
  --bind 0.0.0.0 --port 8443 \
  --tls-cert /etc/uav/certificate-chain.pem \
  --tls-key /etc/uav/private-key.pem \
  --state-file /var/lib/uav/gateway-state.json \
  --web-root /opt/uav/web
```

TLS 最低版本为 1.2。证书与私钥必须同时配置并匹配；加载错误拒绝启动。默认 HTTP 便于本机开发，也支持由同源反向代理终止 TLS。部署时让私钥只对服务账户可读。

```bash
cmake --install build/gateway-native --prefix /opt/uav-gateway
```

安装产物包括 `bin/uav_gateway`、`share/uav-gateway/openapi.json`，已构建的 `share/uav-gateway/www` 静态资源，以及 `etc/uav-gateway/gateway.json` 启动配置。构建目录也会提供 `config/gateway.json`；重新配置和安装均保留已经存在的配置文件。手工部署时一起复制 `bin/`、`www/`、`config/`。接口文档也可直接从公开路径 `/openapi.json` 获取。

| 环境变量 | 用途 |
| --- | --- |
| UAV_GATEWAY_TOKEN | 覆盖文件中的 admin_token；启用 Gateway 时必须非空 |
| UAV_GATEWAY_VIEW_TOKEN | 覆盖文件中的 viewer_token，可选 |
| UAV_GATEWAY_BIND / UAV_GATEWAY_PORT | 覆盖文件中的 bind_address / port；端口 0 自动分配 |
| UAV_GATEWAY_CONFIG | 初次启动的设备/码流目录文件 |
| UAV_GATEWAY_STATE | 持久化文件，默认 data/gateway-state.json；显式设为空禁用持久化 |
| UAV_GATEWAY_WEB_ROOT | 构建后的网页目录；未设置时自动查找，显式设为空禁用托管 |
| UAV_GATEWAY_TLS_CERT / UAV_GATEWAY_TLS_KEY | TLS 证书链与私钥路径 |
| UAV_GATEWAY_ORIGIN | 允许的浏览器来源，例如 https://console.example.com；默认只允许同源 |
| UAV_GATEWAY_MEDIA_TOKEN | 独立媒体进程调用播放授权校验接口的服务凭据，须与用户令牌不同 |
| UAV_GATEWAY_MAX_CONNECTIONS / UAV_GATEWAY_MAX_COMMANDS | 连接上限 1–1024，命令上限 1–256；默认 64/32 |
| UAV_GATEWAY_ACCESS_TTL / UAV_GATEWAY_REFRESH_TTL | 访问/刷新有效期，秒；默认 900/28800，上限 86400/604800，刷新期限不能短于访问期限 |
| UAV_GATEWAY_PLAYBACK_TTL / UAV_GATEWAY_TICKET_TTL | 播放授权/WS 票据有效期，秒；默认 120/30，上限 3600/300 |

CLI 的 bind、port、config、state-file、web-root、tls-cert、tls-key 覆盖对应环境变量。单独使用 C++ Gateway 时，Config.stateFile 默认为空，应用可明确选择存储路径。

板端使用 `cmake --preset ascend-aarch64` 与 `cmake --build --preset ascend-aarch64` 构建。在启动配置文件中填写管理员令牌后，正常运行 uav_detection 会同时启动 Gateway、登记真实 RGB 控制适配器，并上报 RGB 和热成像的采集统计。热成像在该入口只提供状态。双相机出图测试可通过 `--duration 15` 定时结束，详见 [测试说明](camera-capture-test.md)。Gateway 配置错误或绑定失败时记录错误并继续采集。独立 uav_gateway 启动失败仍返回非零退出码。

```bash
# 板端：相机采集与 Gateway 一起运行
./build/ascend-aarch64/bin/uav_detection

# 仅运行 Gateway，跳过硬件采集
./build/ascend-aarch64/bin/uav_detection --gateway-only --port 8080
```

## 登录、权限与退出

接入令牌由部署者配置，权限为 admin 或 viewer。本版本采用令牌登录，不提供用户名/密码账户数据库。

```bash
curl -H 'Content-Type: application/json' \
  -d '{"token":"<你的接入令牌>"}' \
  http://127.0.0.1:8080/api/v1/auth/login
```

成功返回 201，data 包含 session_id、role、access_token、refresh_token 和有效期。访问凭据默认 15 分钟，会话最长 8 小时；最多同时保留 64 个登录会话。浏览器使用短期 access_token 调用 API，无需保存长期接入令牌到前端资源中。

```bash
curl -H "Authorization: Bearer $ACCESS_TOKEN" \
  http://127.0.0.1:8080/api/v1/auth/me

curl -H 'Content-Type: application/json' \
  -d '{"refresh_token":"<上次返回的刷新凭据>"}' \
  http://127.0.0.1:8080/api/v1/auth/refresh

curl -X POST -H "Authorization: Bearer $ACCESS_TOKEN" \
  http://127.0.0.1:8080/api/v1/auth/logout
```

刷新会同时轮换 access_token 与 refresh_token，旧凭据立即失效，最长会话时间不延长。退出撤销会话及关联播放凭据；尚未使用的 WebSocket 票据失效，已连接 WebSocket 最迟在下一次状态推送时关闭。会话与播放凭据不落盘，进程重启后全部失效。

脚本仍可直接使用管理员/只读接入令牌作为 Bearer Token，身份分别为 root.admin/root.viewer。它们是部署配置，不属于登录会话，调用 logout 返回 login_session_required；轮换这些令牌需要修改配置并重启。登录与刷新按直连来源 IP 限制每分钟总共 20 次，最多记录 256 个来源；不会信任客户端自行填写的 X-Forwarded-For。

viewer 可读取设备状态与能力、提交播放请求、管理自己创建的播放授权、订阅事件；不会收到设备原始 config。admin 额外可修改配置、提交/取消设备命令、读取审计并管理所有播放授权。不同登录会话的播放授权相互隔离。

## HTTP API

除 `/openapi.json` 和静态资源外，HTTP 返回统一 JSON：

```json
{"ok":true,"data":{}}
```

```json
{"ok":false,"error":{"code":"unauthorized","message":"valid Bearer token required"}}
```

响应包含 X-Request-ID、X-Content-Type-Options 和 Referrer-Policy。Origin 不匹配时返回 403。允许来源的 OPTIONS 预检返回 204，允许 Authorization/Content-Type/X-Request-ID 请求头与 GET/POST/PUT/DELETE 方法；CORS 精确回显来源，不使用通配符或 Cookie 鉴权。

| 方法与路径 | 权限 | 行为 |
| --- | --- | --- |
| GET /healthz、/readyz | 公开 | Gateway 存活/就绪；不代表相机、算法或媒体已就绪 |
| GET /openapi.json | 公开 | 原始 OpenAPI 3.0.3 文档 |
| POST /api/v1/auth/login | 接入令牌 | 创建可撤销登录会话 |
| POST /api/v1/auth/refresh | 刷新凭据 | 轮换访问与刷新凭据 |
| GET /api/v1/auth/me | 只读及以上 | 当前身份与权限 |
| POST /api/v1/auth/logout | 登录会话 | 撤销当前登录会话 |
| GET /api/v1/status | 只读及以上 | 设备快照、队列、请求/错误/事件丢弃计数、存储与 TLS 配置状态 |
| GET /api/v1/devices、/devices/{id} | 只读及以上 | 设备目录、状态与支持的动作 |
| PUT /api/v1/devices/{id} | 管理员 | 创建/替换未附着适配器的设备配置，原子保存 |
| DELETE /api/v1/devices/{id} | 管理员 | 删除未附着适配器且无流引用的设备 |
| GET /api/v1/streams、/streams/{id} | 只读及以上 | 码流目录与适配器接入状态 |
| PUT /api/v1/streams/{id} | 管理员 | 创建/替换未附着媒体适配器的流配置 |
| DELETE /api/v1/streams/{id} | 管理员 | 删除未附着适配器的流配置 |
| POST /api/v1/devices/{id}/commands | 管理员 | 返回 202 和命令 ID，异步执行 |
| GET /api/v1/commands | 只读及以上 | 最近 32 条命令记录，最新提交在前 |
| GET /api/v1/commands/{id} | 只读及以上 | 查询命令状态与执行结果 |
| DELETE /api/v1/commands/{id} | 管理员 | 取消尚未开始的命令；已开始返回 409 |
| POST /api/v1/playback | 只读及以上 | 先解析候选入口再鉴权，签发限定码流/协议的播放凭据 |
| GET /api/v1/playback/{id} | 所有者/管理员 | 查看授权元数据，不重复返回媒体凭据 |
| POST /api/v1/playback/{id}/renew | 所有者/管理员 | 延长授权并轮换媒体凭据，旧凭据失效 |
| DELETE /api/v1/playback/{id} | 所有者/管理员 | 撤销播放授权 |
| GET /webrtc | 公开页面 | 静态控制台 index.html 的别名，播放时仍需凭据；缺少页面构建时 503 |
| POST /api/v1/webrtc/{stream_id} | 对应 WebRTC 媒体凭据 | 完整 SDP Offer 换 Answer，201 返回媒体会话 ID；占用默认单路视频名额 |
| DELETE /api/v1/webrtc/{stream_id}/{session_id} | 创建会话的播放授权 | 关闭媒体会话并释放名额，使用续期后的当前媒体令牌 |
| POST /api/v1/media/authorize | 媒体服务凭据 | 校验媒体凭据、码流与协议，供独立媒体进程使用 |
| POST /api/v1/ws-ticket | 只读及以上 | 创建一次性 WebSocket 票据 |
| GET /api/v1/events?ticket=... | 票据 | WebSocket Upgrade |
| GET /api/v1/audit | 管理员 | 最近 256 条内存审计记录，不含正文与凭据 |
| GET /api/v1/openapi.json | 只读及以上 | JSON envelope 中的 OpenAPI 文档 |

设备与动作 ID 为 1–64 字符，仅字母、数字、下划线、点与短横线。设备类型限 camera、radar、spectrum、ptz、laser。流目录可描述 rtsp、webrtc、moq、gb28181；`POST /playback` 不受理 gb28181，返回 native_protocol，由独立 SIP 服务建立国标会话。

## 配置保存与设备能力

```bash
curl -X PUT -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"type":"radar","name":"Radar A","enabled":true,"config":{"host":"192.0.2.10","port":9000,"frame_id":"radar_a"}}' \
  http://127.0.0.1:8080/api/v1/devices/radar.a

curl -X PUT -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"radar.a","codec":"h264","protocols":["webrtc"]}' \
  http://127.0.0.1:8080/api/v1/streams/radar.a.main
```

PUT 是完整替换，不是 PATCH。配置先写入同目录临时文件、fsync，再原子 rename；提交成功后才更新内存并推送 registry.changed。保存失败返回 503 persistence_failed，内存保持原状态。磁盘操作使用独立有界文件工作线程，不阻塞网络线程。状态文件权限为 0600，独占进程锁防止两个服务同时写入同一文件。

首次运行读取 --config 种子目录。生成持久化快照后，重启以快照恢复配置与删除结果，种子文件中已被删除的条目不会重新出现。应用注册的真实设备处理器和媒体 resolver 始终以本次应用装配为准，持久化文件不能覆盖这些运行时适配器；硬件状态和在线连接不会从磁盘恢复。启动目录/状态文件上限 4 MiB。

注册真实处理器或媒体 resolver 的条目由应用管理，PUT/DELETE 返回 409，避免网页改动与硬件生命周期冲突。最多 128 个设备、128 个流；单份设备 config 最多 16 KiB，状态最多 2 KiB。

保存连接参数不等于设备已经连接。雷达/频谱厂商驱动、具体参数校验与坐标融合由对应适配器实现；未附着适配器时状态为 not_implemented，命令返回 503。保存流目录也不会启动编码或伪造在线媒体入口。

## 命令执行

```bash
curl -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"action":"brightness","parameters":{"value":60},"ttl_ms":2000}' \
  http://127.0.0.1:8080/api/v1/devices/rgb/commands
```

202 只表示受理。queued/running 表示排队/执行中，succeeded 表示适配器确认完成，dispatched 表示仅确认已下发，failed/expired/cancelled 表示失败/排队过期/取消。可通过查询接口或 command.result 事件读取结果。

独立命令线程串行执行，默认最多 32 个在途/排队命令，满时返回 429。ttl_ms 为 100–10000，默认 2000，约束开始执行的期限；过期命令不访问设备。处理器必须有界返回，Gateway 不强行终止正在执行的 C++ 回调。最多保留 256 条记录，按提交顺序淘汰已结束记录。后端异常转为 backend_exception，不暴露内部异常内容；超大结果转为 backend_result_too_large。

板端 RGB 支持 autofocus、zoom_to、focus_to、brightness、contrast、saturation。除 autofocus 外通过 parameters.value 传值；zoom_to 为 0–16384，focus_to 为 0–65535，图像参数为 0–100。参数错误记录为 failed，控制串口未建立返回 camera_disconnected。现有相机驱动只确认串口写入，成功记录为 dispatched/command_sent，data.device_confirmed 为 false。设备 ACK、物理到位、连续云台速度控制与控制权仲裁仍由设备层后续接入。

## 播放授权与媒体适配

```bash
curl -H "Authorization: Bearer $ACCESS_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"stream_id":"rgb.main","protocol":"webrtc"}' \
  http://127.0.0.1:8080/api/v1/playback
```

顺序为：调用只读 resolver 获取候选地址 → Gateway 鉴权 → 检查流与设备状态 → 签发播放授权 → 返回 url、parameters、session_id、media_token、expires_in_seconds。未授权请求不会返回候选地址或流是否存在的信息；未接入/未就绪媒体返回 503。候选 URL 仅允许相应协议的 http(s)、ws(s)、rtsp(s) 或同源相对路径，拒绝脚本协议、控制字符、跨域双斜线路径与内嵌用户名/密码。

播放凭据默认 120 秒有效，最多 128 份，同时限定 stream_id、protocol 和登录会话。续期轮换凭据，删除或退出登录立即使校验失败。这个 session_id 标识 Gateway 授权，不代表 WebRTC 协商或底层媒体传输已经建立。

同进程媒体适配器在实际接受播放连接时调用 Gateway::validatePlaybackToken(token, streamId, protocol)。独立媒体进程使用独立服务凭据校验：

```bash
curl -H "Authorization: Bearer $UAV_GATEWAY_MEDIA_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"token":"<media_token>","stream_id":"rgb.main","protocol":"webrtc"}' \
  http://127.0.0.1:8080/api/v1/media/authorize
```

合法返回 200 authorized=true，无效/过期/撤销返回 403。未配置媒体服务凭据时此 HTTP 校验接口返回 503。同进程校验不需要媒体服务凭据。

媒体服务必须在建立会话时校验，并在授权到期或撤销后结束会话，可周期校验；Gateway 无法单方面关闭其他模块持有的 socket。media_token 通过媒体适配器约定的短期授权字段传递，不将 Gateway 长期接入令牌写入 URL。直接进入 GB28181、ONVIF、RTSP 的原生客户端按各自协议鉴权，不依赖这些网页授权。

当前主程序登记 rgb.main，但编码和 WebRTC 服务尚未接通，所以真实板端播放返回 503。Gateway 已完成访问与授权职责，视频传输由媒体模块实现；测试中的就绪地址只存在于测试驱动。

## WebSocket 与网页托管

```javascript
const reply = await fetch('/api/v1/ws-ticket', {
  method: 'POST',
  headers: { Authorization: `Bearer ${accessToken}` },
});
const result = await reply.json();
if (!reply.ok) throw new Error(result.error.message);
const scheme = location.protocol === 'https:' ? 'wss:' : 'ws:';
const socket = new WebSocket(`${scheme}//${location.host}${result.data.url}`);
socket.onmessage = ({ data }) => console.log(JSON.parse(data));
```

票据默认 30 秒有效、使用一次即作废，最多 128 份；不会把长期 Bearer Token 放进 WebSocket URL。连接时立即发送 snapshot，之后每秒更新。推送 command.accepted、command.result、registry.changed 和后端 publish 的领域事件。客户端文本消息 {"type":"ping"} 返回 pong，其他消息返回 error。控制命令通过 HTTP 提交。

每连接写队列最多 32 条/1 MiB，慢消费者独立断开。后端公开事件最多 64 KiB，异步投递前限制最多 128 次待处理投递，过载丢弃计入 dropped_events。最多 64 个 HTTP/WS/TLS 连接；HTTP 正文 64 KiB、头部 8 KiB、URI 2048 字节、读写及 TLS 握手超时 15 秒；WS 输入最多 8 KiB。

网页根目录提供 index.html 和 JS/CSS/图片/字体等静态文件，支持 GET/HEAD、MIME 类型与缓存再验证。SPA 无扩展名页面路由仅在 Accept 包含 text/html 时回退到 index.html；缺失脚本和 API 路径不回退。单个资源最多 4 MiB，不提供目录列表。路径先解码再校验，拒绝上级目录、隐藏文件、空字节以及指向资源目录外的符号链接。状态文件和 TLS 私钥不得位于网页根目录中。

## 应用接入与验证

接口见 [gateway.h](../src/gateway/gateway.h)。start 前 registerDevice/registerStream；运行时 updateDeviceStatus/publish 可从业务线程调用。CommandHandler 在独立串行命令线程执行，只有发送结果时使用 CommandCompletion::dispatched。StreamResolver 在网络线程执行，必须快速、只读，不连接设备、启动编码或等待网络。

start/stop 由所有者线程调用，不在处理器或网络回调里调用 stop。退出时拒绝新请求、取消排队命令、等待在途处理器和已受理文件工作，随后关闭连接、撤销凭据、排空取消回调。应用在 stop 返回后销毁硬件对象。

主机集成测试使用真实 HTTP/HTTPS/WS/WSS，覆盖登录、轮换、退出、角色及所有权隔离、播放授权范围/过期/撤销、候选入口解析顺序、配置重启恢复/删除/写入失败/独占锁、命令取消/过载/过期/异常、静态路径防越界、SPA/CORS、证书连接、慢客户端隔离和在途操作退出。板端硬件需要单独实测。
