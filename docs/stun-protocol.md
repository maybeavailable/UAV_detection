# STUN protocol

`src/webrtc/stun_protocol.h/.cpp` 实现 STUN 报文层，使用 C++17 类型、Asio UDP 端点和 OpenSSL Crypto。报文格式与认证参考 [RFC 8489](https://www.rfc-editor.org/rfc/rfc8489.html)，兼容 RFC 5389 的 SHA1 短期凭据；ICE Binding 属性参考 [RFC 8445 §7.2](https://www.rfc-editor.org/rfc/rfc8445.html#section-7.2)。

发送时使用可编辑的 `StunMessage`，接收时使用拥有原始数据的 `StunPacket`。头文件提供声明，实现放在 `.cpp` 中，主工程已加入该源文件并复用已有的 `OpenSSL::Crypto` 链接。

支持 Binding Request、Indication、Success Response、Error Response；IPv4/IPv6 的 MAPPED-ADDRESS、XOR-MAPPED-ADDRESS、ALTERNATE-SERVER；USERNAME、REALM、NONCE、SOFTWARE、ERROR-CODE、UNKNOWN-ATTRIBUTES、USERHASH、PASSWORD-ALGORITHM(S)；以及 ICE 的 PRIORITY、USE-CANDIDATE、ICE-CONTROLLING/CONTROLLED。扩展 method/属性可转换为相应 enum，未知必选属性通过 `unknownRequiredAttributes()` 返回去重、排序的类型列表，供 420 响应使用。

## ICE 连通性检查

```cpp
#include "webrtc/stun_protocol.h"
#include "webrtc/ice_candidate.h"
#include "common/udp_socket.h"

using namespace webrtc;

// 这两个密码分别来自本地与远端 SDP 的 ice-pwd，ufrag 同理。
auto localKey = stunShortTermKey("LocalPassword0123456789");
auto remoteKey = stunShortTermKey("RemotePassword0123456789");

// 本地为 controlling；tieBreaker 由 ICE agent 生成并在会话期间维护。
auto request = StunMessage::iceBindingRequest(
    "locl", "remt", candidatePriority(IceCandidateType::PeerReflexive),
    StunIceRole::Controlling, 0x1122334455667788ULL, true);
StunEncodeOptions outgoing{StunIntegrity::Sha1, remoteKey, true};
// socket.asyncSendTo(request.encode(outgoing), remoteEndpoint);

// 收到对端发往本地的请求时：datagram 为 UdpSocket::Datagram。
auto onReceive = [&](UdpSocket& socket, UdpSocket::Datagram datagram) {
    StunPacket packet;
    std::string reason;
    if (!StunPacket::parse(datagram.payload, packet, &reason)) return;
    if (!packet.validateIceBindingRequest("locl", "remt", localKey, &reason)) return;
    // 此处交由 ICE agent 处理角色冲突、peer-reflexive 候选及提名状态。
    auto response = StunMessage::bindingSuccess(packet, datagram.remoteEndpoint);
    socket.asyncSendTo(response.encode({StunIntegrity::Sha1, localKey, true}),
                       datagram.remoteEndpoint);
};
```

请求 USERNAME 为 `remoteUfrag:localUfrag`，发送请求使用 **远端密码**；接收请求使用 **本地密码**。响应使用认证该请求的同一 key，不带 USERNAME。`validateIceBindingRequest` 接收参数的 local/remote 均以接收者为视角，同时验证 Binding 类型、用户名、优先级、唯一角色、提名权限、未知必选属性、HMAC 和 FINGERPRINT。它不决定 487 角色冲突；角色冲突必须在认证之后由 ICE agent 根据本地角色和 tie-breaker 处理。

收到响应后，先通过 `isResponseTo(request)` 关联 method/transaction ID，检查来源端点，再调用 `verifyFingerprint()` 和 `verifyIntegrity(remoteKey)`。成功响应的 `mappedAddress()` 返回端点，可交给 `IceCandidate::fromUdpEndpoint` 创建 reflexive 候选。错误响应通过 `find(StunAttributeType::ErrorCode)->asError()` 读取；`bindingError` 支持 300~699，420 必须传入未知属性列表。400/401 等认证失败响应如何处理及是否附加完整性，由凭据机制和上层状态决定。

SDP 使用 BUNDLE 时，先用 `transportMedia(section, descriptionType)` 找到传输所属媒体，再用 `effectiveAttributes` 读取 ICE 凭据。

## 公网 STUN 地址收集

```cpp
auto request = webrtc::StunMessage::bindingRequest("UAV_detection");
auto wire = request.encode(); // 不附加认证，用于无需凭据的公网 STUN 服务
// 使用同一个 UdpSocket 发送和接收，服务器返回的 mapped 地址才对应这个 Socket。
```

这类响应可能没有完整性属性。解析和事务/来源匹配后读取 `mappedAddress()`；如果存在 FINGERPRINT，仍须验证。长期凭据可用 `stunLongTermKey(username, realm, password, algorithm)` 派生二进制 key，MD5 为默认，SHA256 由 PASSWORD-ALGORITHM 协商。HMAC 算法和 key 派生算法是独立的选择。Nonce 挑战、过期和算法协商状态由调用者维护。Unicode 凭据须由调用者完成适用的 OpaqueString/SASLprep 处理，本模块不执行 Unicode 规范化。

## 校验与边界

`parse` 只验证头部、精确长度、TLV 边界和已知属性格式，不表示报文已认证。解析失败不修改输出，错误原因不回显密码。`looksLikeStun` 仅作头部快速判断，不能代替 `parse` 或认证。

TLV 按四字节对齐，发送补零，接收允许任意 padding。普通重复属性保留首项，重复或倒序完整性属性被拒绝，FINGERPRINT 必须在末尾。不支持的地址族不转换为端点；`mappedAddress()` 可回退到可用的 MAPPED-ADDRESS。首个完整性属性之后的普通属性被忽略，不进入 `attributes()`/`find()` 或 ICE 验证；原始 `wire()` 仅供诊断，不能将其中被忽略的属性用于业务决策。这遵循 [RFC 8489 §9](https://www.rfc-editor.org/rfc/rfc8489.html#section-9) 的认证范围。

`encode` 自动按顺序附加 HMAC-SHA1、HMAC-SHA256 和 CRC32 FINGERPRINT，并按每个完整性属性的末尾调整 HMAC 输入中的头部长度。禁止手工附加安全属性，避免重用过期摘要。`verifyIntegrity` 使用 constant-time 比较，指定算法缺失时失败；`Sha1AndSha256` 要求两者都正确。SHA256 默认要求完整 32 字节，仅明确支持截短的 STUN usage 可显式允许 16~32 字节、四字节倍数的截短值。

STUN FINGERPRINT 是 CRC32，用于识别报文及检测损坏，不是认证，也不是 SDP 中供 DTLS 校验证书的 fingerprint。DTLS 仍使用 SDP 的证书指纹。

协议字段最大总长度为 65552 字节，实际 UDP 发送还受 `UdpSocket` 的数据报大小限制。事务 ID 使用 OpenSSL CSPRNG 生成；重传必须复用原请求及 transaction ID。UDP 事务重传、超时、服务器解析、候选收集流程、ICE 角色冲突和状态机不在报文模块中。

## 验证

测试覆盖 [RFC 5769](https://www.rfc-editor.org/rfc/rfc5769.html) 的请求、IPv4/IPv6 响应和长期凭据四组向量，以及 RFC 8489 B.1 的 SHA256/USERHASH 向量（应用已确认的 [勘误 6268](https://www.rfc-editor.org/errata_search.php?eid=6268)，明确使用向量指定的已处理密码）。还包括全部 method/class 位映射、长度边界、截断、篡改、算法缺失、认证之后的属性注入、SHA256 截短策略、ICE 请求验证，以及真实 UdpSocket 的本机请求/响应和候选转换。

```bash
cmake -S tests/webrtc -B /tmp/uav-webrtc-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/uav-webrtc-build --parallel 4
ctest --test-dir /tmp/uav-webrtc-build --output-on-failure
```
