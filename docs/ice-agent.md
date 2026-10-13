# ICE agent

`src/webrtc/ice_agent.h/.cpp` 提供基于 Boost.Asio strand、steady_timer 和现有 `UdpSocket` 的 UDP ICE agent。一个实例管理一个独立 ICE transport，也就是一个 BUNDLE 组或未捆绑的媒体传输；`components={1}` 适用于 RTCP mux 和 SCTP，非 mux RTP/RTCP 使用 `{1, 2}`。

候选配对、优先级、冻结/解冻、triggered checks、角色冲突和 regular nomination 依据 [RFC 8445](https://www.rfc-editor.org/rfc/rfc8445.html)。逐步添加候选与 end-of-candidates 参考 [RFC 8838](https://www.rfc-editor.org/rfc/rfc8838.html)，选中通道上的 consent freshness 参考 [RFC 7675](https://www.rfc-editor.org/rfc/rfc7675.html)。

支持的流程：

- 生成本地 ICE 凭据及 tie-breaker，收集 IPv4/IPv6 host 和公网 STUN server-reflexive 候选。
- 按组件、地址族配对，按 pair priority 排序，限制候选和 checklist 大小，合并相同 base/远端端点的候选对。
- 短期凭据 HMAC-SHA1 和 FINGERPRINT 认证、源端点/Socket/事务关联、指数退避重传和超时。
- 487 角色冲突修复、重新计算优先级；收到 487 后更新 tie-breaker。
- 从认证请求和成功响应学习 remote/local peer-reflexive 候选，触发反向检查。
- regular nomination：先检查成功，再由 controlling 单独发送 USE-CANDIDATE；controlled 等待反向检查成功才选中。
- Trickle ICE、远端凭据到达前的认证请求、SDP/BUNDLE 导入、ICE restart。
- 选中候选对上的应用数据收发、consent 失联检测、过期与认证 403 撤销，以及异步关闭。

## 与信令、DTLS 接入

```cpp
#include "webrtc/ice_agent.h"
#include "webrtc/sdp.h"

boost::asio::io_context io;
webrtc::IceAgent::Config config;
config.role = webrtc::StunIceRole::Controlling; // 通常 offerer controlling，answerer controlled
config.bindings = {{{boost::asio::ip::make_address("192.0.2.10"), 0}, 1, 65535}};
// 192.0.2.10 是示例地址，运行时替换为本机接口地址；端口 0 自动分配。
// config.stunServers = {{resolvedStunServerAddress, 3478}};

webrtc::IceAgent::Callbacks callbacks;
callbacks.onLocalCandidate = [&](webrtc::IceCandidate candidate) {
    auto signalingCandidate = candidate.toCandidateString();
    // 信令发送 candidate，并关联 sdpMid / m-line index。
};
callbacks.onGatheringState = [&](webrtc::IceGatheringState state) {
    if (state == webrtc::IceGatheringState::Complete) {
        // 信令发送本代 ICE 的 end-of-candidates。
    }
};
callbacks.onSelectedPair = [&](webrtc::IceCandidatePair pair) {
    // 该组件已有认证且提名成功的通道，可启动 DTLS 握手。
};
callbacks.onData = [&](std::uint16_t component, UdpSocket::Datagram datagram) {
    // 根据 RFC 7983 将选中对端的数据交给 DTLS / SRTP / SRTCP 等模块。
};

webrtc::IceAgent agent(io, config, callbacks);
auto credentials = agent.snapshot().localCredentials;
// 将 credentials.ufrag / credentials.password 填入本地 SDP。
agent.gather();

// 远端 SDP 到达时：
// agent.setRemoteDescription(remoteSdp, "video", webrtc::SdpDescriptionType::Answer);
// 或分别传递：
// agent.setRemoteCredentials({remoteUfrag, remotePassword}, remoteIceLite);
// agent.addRemoteCandidate(remoteCandidate);
// agent.endOfRemoteCandidates(remoteUfrag);
agent.start(); // 可在远端信息到齐前调用，等待后续 trickle

// DTLS 要发出数据时：agent.asyncSend(std::move(dtlsPacket), 1, onSent);
// 会话结束时：agent.close(onClosed);
io.run();
```

`Connected` 表示每个组件都有 valid pair；`onSelectedPair` 表示某组件已成功提名。DTLS 从后者启动，使用 `asyncSend` 发送握手数据，使用 `onData` 接收。SDP 的证书 fingerprint/setup 仍由 `SdpSession` 保存和 DTLS 模块消费；ICE 的 STUN FINGERPRINT 是 CRC，不能代替证书验证。

`setRemoteDescription` 按 MID 查找媒体，通过 `transportMedia(section, descriptionType)` 找到 BUNDLE 传输所属媒体，再读取有效 ICE 凭据和候选。调用的 Offer/Answer 类型必须与信令一致。DTLS fingerprint 不由 ICE agent 验证。

## 候选与生命周期

不设置 `bindings` 时，Linux/Unix 自动枚举 UP 的 IPv4/IPv6 接口，默认排除回环和带 scope 的 IPv6 link-local 地址。每个具体地址/组件有独立 Socket，避免通配绑定无法确定 STUN 请求的本地候选。显式绑定回环地址可用于本机测试，通配地址会展开成对应地址族的接口地址。其他平台可传入具体 `bindings`。

`gather` 的完成回调表示收集已启动，`onGatheringState(Complete)` 表示 STUN 收集也已完成。某个 STUN 服务失败会触发 `onError`，可用 host 候选仍然保留。公网 STUN 服务端点由上层解析后配置；每个 STUN 事务从将来实际通信的同一个 Socket 发起。默认首次 RTO 至少 500ms，最多发送 7 次，最后一次等待初始 RTO 的 16 倍。

已有具体地址的 `shared_ptr<UdpSocket>` 可通过 `addLocalTransport` 注册。Agent 接管其接收回调，关闭 agent 时也关闭这些 Socket；注册期间不要替换其接收回调。`start` 在已有 transport 时直接使用它们，在没有 transport 时自动调用 `gather`。

远端候选接受 UDP host/srflx/prflx/relay；远端的 relay 端点可作为普通 UDP 目的地。系统 resolver 可解析域名；浏览器 `.local` 地址需要系统 mDNS 支持，或由调用者解析后通过 `addRemoteCandidate` 的 `resolvedEndpoint` 提交。无解析结果不会假装候选已经连通。ICE-TCP、本地 TURN Allocate/permission/ChannelData 传输和本地 ICE-lite 不在当前实现中。远端 `ice-lite` 使本 agent 使用 controlling 角色。

候选可先于远端凭据提交。信令提供的候选 `ufrag` 扩展或 `generationUfrag` 用于拒绝旧 generation；end-of-candidates 后不再接受新的信令候选，但仍可学习认证的 peer-reflexive 候选。未收到 end-of-candidates 时不会因当前 checklist 暂时为空而立即失败。整个检查流程仍受 `checkTimeout` 限制，默认 60 秒。

更换远端凭据之前先调用 `restart`，已有凭据被直接替换时返回 `already_started`。Restart 同时更换本地 ufrag/password，清除远端信息、事务、提名、consent 和旧 prflx 候选，保留本地 Socket 和 host/srflx 候选。读取新 `snapshot().localCredentials` 并交换新 SDP 后再次 `start`。旧连接立即停止应用数据；没有新通道前 `asyncSend` 返回 `not_connected`。

选中通道默认每 4~6 秒发送一个新的认证 Binding 请求，consent 请求只发送一次。默认 15 秒无有效响应转为 `Disconnected`；30 秒过期转为 `Failed` 并禁止应用发送。认证的 403 响应立即撤销 consent。迟到响应不能恢复过期 consent，必须 ICE restart。普通媒体包或对端发来的 STUN 请求不会刷新本地的 consent。

所有异步 API 与回调在内部 strand 串行执行；`snapshot` 用互斥锁返回一致副本，可跨线程读取。回调不得抛异常。`close` 幂等，取消 timer/resolver/事务并关闭所有 Socket，完成回调在已接受的 Socket 发送完成及关闭之后调用。析构会请求关闭，调用者必须保证 io_context 存活并继续运行至关闭完成。

默认最多保留 256 个本地/远端候选、100 个候选对和 1024 个待完成事务；`maxPendingTransactions` 也计入取消重传但仍在等待迟到响应的旧事务。达到事务额度时暂停创建新事务，已有事务继续重传或超时。

## 验证

```bash
cmake -S tests/webrtc -B /tmp/uav-webrtc-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/uav-webrtc-build --parallel 4
ctest --test-dir /tmp/uav-webrtc-build --output-on-failure
```

`ice_agent_test` 使用真实回环 UDP 测试 IPv4/IPv6、双组件、双 controlling/controlled 角色冲突、trickle、SDP 前请求、STUN 收集、丢包重传、regular nomination、peer-reflexive 学习、错误源端口/错误 HMAC、远端 ICE-lite、consent 撤销和过期、ICE restart、SDP 导入、mDNS 显式解析端点、resolver 取消及多线程关闭。独立 STUN 对端测试可避免仅靠两份相同实现相互验证。
