# ICE Candidate

`src/webrtc/ice_candidate.h/.cpp` 提供 `webrtc::IceCandidate` 候选模型，包括 SDP 解析/序列化、参数校验、优先级计算和 Asio UDP 端点转换。候选格式参考 [RFC 8839 §5.1](https://www.rfc-editor.org/rfc/rfc8839.html#section-5.1)，候选及候选对优先级参考 [RFC 8445 §5.1.2](https://www.rfc-editor.org/rfc/rfc8445.html#section-5.1.2) 和 [§6.1.2.3](https://www.rfc-editor.org/rfc/rfc8445.html#section-6.1.2.3)，TCP 的 `tcptype` 参考 [RFC 6544 §4.5](https://www.rfc-editor.org/rfc/rfc6544.html#section-4.5)。

```cpp
#include "webrtc/ice_candidate.h"
#include "common/udp_socket.h"

boost::asio::io_context io;
UdpSocket::Config config;
config.localEndpoint = {boost::asio::ip::make_address("192.0.2.1"), 0}; // 替换为本机具体接口地址
UdpSocket socket(io, config);
auto local = webrtc::IceCandidate::host(socket.localEndpoint(), "1");
auto signalingValue = local.toCandidateString(); // candidate:...
auto sdpLine = local.toSdp();                    // a=candidate:...\r\n

webrtc::IceCandidate remote;
std::string reason;
if (webrtc::IceCandidate::parse(signalingValue, remote, &reason)) {
    if (auto endpoint = remote.udpEndpoint()) {
        socket.asyncSendTo({0x01}, *endpoint);
    }
}
socket.close();
io.run();
```

候选类型支持 `host/srflx/prflx/relay`；传输类型支持 UDP/TCP。TCP 必须携带 `tcptype active/passive/so`，active 端口必须为 9；TCP 候选只表达信息，不会转换成 UDP 端点。UDP 候选不能携带 `tcptype`。

`parse` 接受信令中的 `candidate:...` 和 SDP 的 `a=candidate:...`，支持末尾 CRLF/LF、IPv4、裸 IPv6、域名及浏览器 `.local` 主机名。域名保留原样；`udpEndpoint()` 对域名返回 `nullopt`，由 ICE agent 完成 DNS/mDNS 解析后再使用。地址不接受 URL 方括号和 IPv6 作用域标记。空候选和 `end-of-candidates` 由信令/ICE agent 单独处理，不作为 `IceCandidate`。

组件范围 1~256，优先级范围 1~2147483647，候选端口范围 1~65535。host 不允许 `raddr/rport`；其他类型必须同时提供二者。可使用 `0.0.0.0/::` 与端口 9 作为隐藏 related 地址的值；接收时也保留 `rport=0`。未知扩展以非空、可见 ASCII 的名称/值对保存，不参与传输决策；保留名称和原始顺序，支持 `generation`、`ufrag`、`network-id`、`network-cost` 等。拒绝重复的保留字段、超范围整数、非法名称/值和多行输入。完整 SDP 行上限 4096 字节，避免序列化后无法再次解析。解析失败不修改输出候选，原因可通过 `reason` 获取。

`candidatePriority` 提供默认类型偏好 host=126、prflx=110、srflx=100、relay=0，也可显式指定类型偏好。`candidatePairPriority(local, remote, localControlling)` 使用 64 位计算，角色反转会正确改变平局项。非法参数抛 `std::invalid_argument`。

候选的 UDP/TCP 传输枚举为 `IceCandidateTransport`；`IceTransport` 名称用于 WebRTC 的实际传输类。

`host` 与 `fromUdpEndpoint` 用于从已知 UDP 地址创建候选；reflexive/relay 类型必须提供 related 端点。relay 的 related 是 TURN Allocate 的 mapped 地址，其 base 是 relay 自身。不能将绑定到 `0.0.0.0/::` 的 Socket 地址直接发布为本地候选；需由收集器选取具体接口地址。foundation 的分配、同类型候选的唯一 local preference、STUN/TURN 收集、连通性检查、ICE 角色和候选对状态由后续 ICE agent 维护。信令中的 `sdpMid`、`sdpMLineIndex` 等媒体关联也属于信令层。

测试包含 RFC 示例、四种候选类型、TCP、IPv6、mDNS、扩展保留、非法输入、优先级边界，以及使用候选 SDP 往返后通过 `UdpSocket` 收发真实回环数据报：

```bash
cmake -S tests/webrtc -B /tmp/uav-ice-candidate-build
cmake --build /tmp/uav-ice-candidate-build --parallel 2
ctest --test-dir /tmp/uav-ice-candidate-build --output-on-failure
```
