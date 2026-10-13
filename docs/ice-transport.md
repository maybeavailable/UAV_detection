# ICE transport

`src/webrtc/ice_transport.h/.cpp` 提供 `webrtc::IceTransport`，管理一个独立媒体或一个 BUNDLE 组的 UDP 传输。它拥有 `IceAgent`，将候选、信令、restart、关闭和应用数据收发集中到同一入口。检查、重传、提名、角色冲突、来源认证及 consent 仍由 agent 执行。

报文分流依据 [RFC 7983 §7](https://www.rfc-editor.org/rfc/rfc7983.html#section-7)，RTP/RTCP 第二字节的区分依据 [RFC 5761 §4](https://www.rfc-editor.org/rfc/rfc5761.html#section-4)。DTLS 角色由 SDP setup 决定，参考 [RFC 5763 §5](https://www.rfc-editor.org/rfc/rfc5763.html#section-5)，与 ICE controlling/controlled 角色独立。

## 接入方式

```cpp
#include "webrtc/ice_transport.h"

boost::asio::io_context io;
webrtc::IceTransport::Config config;
config.ice.role = webrtc::StunIceRole::Controlling;
config.ice.bindings = {{{boost::asio::ip::make_address("192.0.2.10"), 0}, 1, 65535}};
// 示例地址运行时替换为本机接口地址；bindings 为空则按 agent 规则枚举本机接口。

webrtc::IceTransport::Callbacks callbacks;
callbacks.onLocalCandidate = [&](webrtc::IceCandidate candidate) {
    auto candidateForSignaling = candidate.toCandidateString();
    // 信令发送 candidate，并附带对应的 MID 和本代 ufrag。
};
callbacks.onGatheringState = [&](webrtc::IceGatheringState state) {
    if (state == webrtc::IceGatheringState::Complete) {
        // 信令发送本代的 end-of-candidates。
    }
};
callbacks.onDtlsReady = [&](webrtc::IceDtlsReady ready) {
    // ready.pair.local.component 确定组件。
    // ready.parameters.localSetup：Active 为 DTLS client，Passive 为 server。
    // ready.parameters.remoteFingerprints 交给 DTLS 模块，用来验证实际对端证书。
    // 这里初始化/启动 DTLS；ready.generation 用于隔离 restart 前后的握手。
};
callbacks.onPacket = [&](webrtc::IceTransportPacket packet) {
    switch (packet.type) {
    case webrtc::IcePacketType::Dtls:
        // 将 packet.datagram.payload 交给 DTLS 解码。
        break;
    case webrtc::IcePacketType::Rtp:
        // 将加密数据交给 SRTP 模块。
        break;
    case webrtc::IcePacketType::Rtcp:
        // 将加密数据交给 SRTCP 模块。
        break;
    default: break;
    }
};

webrtc::IceTransport transport(io, config, callbacks);
auto localCredentials = transport.snapshot().ice.localCredentials;
// 将 localCredentials 写入本地 SDP，指纹仍由本地证书模块生成。
transport.gather();
// 远端描述到达时：
// transport.setRemoteDescription(remoteSdp, "data", webrtc::SdpDescriptionType::Answer, onApplied);
// transport.addRemoteCandidate(candidate, onAdded, resolvedEndpoint, remoteUfrag);
// transport.endOfRemoteCandidates(remoteUfrag);
transport.start();
// DTLS 模块产生完整 UDP 负载后：transport.asyncSendDtls(std::move(packet), component, onSent);
// SRTP / SRTCP 模块加密后：transport.asyncSendRtp(...) / transport.asyncSendRtcp(...);
// 结束会话：transport.close(onClosed);
io.run();
```

`setRemoteDescription` 依据请求的 MID 和 Offer/Answer 类型解析 BUNDLE 传输归属，保存所属媒体的有效 ICE 凭据、fingerprint 和 setup。`snapshot().dtls->transportMid` 是传输所属媒体的 MID。

对远端 `actpass` offer，默认选择本地 `active`，`preferDtlsClient=false` 可选择 `passive`。对远端 answer，`active` 对应本地 `passive`，`passive` 对应本地 `active`。应用应在导入 offer 的完成回调中读取 `snapshot().dtls->localSetup`，填入本地 answer；发送 answer 不必等待 `onDtlsReady`。证书生成、证书摘要计算、DTLS 握手及 SRTP 密钥导出由相应模块实现。

## 就绪、状态与报文

`onSelectedPair` 表示某组件已有提名成功的原始 ICE 通道。`onDtlsReady` 进一步要求已经导入远端 SDP 的 fingerprint/setup，每个 generation、每个组件只通知一次。只使用 `setRemoteCredentials` 也可建立 ICE 通道，但没有证书参数时不会触发 `onDtlsReady`；随后导入 SDP 可补发通知。

`isReady(component)` / `selectedPair(component)` 用于读取对应组件的通道；`asyncSend` 仍由 agent 执行最终 consent 检查。全部配置组件提名成功后传输状态进入 `Connected` / `Completed`，agent 只有 valid pair 而尚未提名时，传输仍为 `Checking`。`Disconnected` 期间只要 consent 尚未过期仍可发送；`Failed`、`Closed` 和 restart 期间禁止发送。

`classifyIcePacket` 只作协议分流：首字节 0~3 为 STUN、16~19 为 ZRTP、20~63 为 DTLS、64~79 为 TURN ChannelData、128~191 为 RTP/RTCP。RTP/RTCP 第二字节 192~223 判为 RTCP，其余判为 RTP。RTP 的 payload type 64~95 与 RTCP mux 冲突，SDP 层已拒绝这些值。分类不表示报文格式正确或已完成密码学认证，实际协议模块仍须验证自己的数据。

Agent 消费 STUN，只把选中源端点的数据交给传输。传输仅向 `onPacket` 转交 DTLS、SRTP、SRTCP，丢弃空包、未知类型、未实现的 ZRTP/TURN 封装及超长报文。`onPacket` 保留组件、generation、源端点和接收时间，payload 拥有自己的内存。若先手动建立 ICE 再导入 SDP，数据回调可能早于 DTLS 参数就绪，由上层协调 DTLS 的初始化。

`asyncSend` 自动识别上述三类应用报文；`asyncSendDtls/Rtp/Rtcp` 额外检查期望类型。它们不加密或解密数据。未选中通道返回 `not_connected`，类型不符返回 `invalid_argument`，超过 `maxDatagramSize` 返回 `message_size`；传输不会积压等待握手的应用数据。默认最大负载 65507 字节，实际发送仍受 Socket 队列及网络限制。

`snapshot().stats` 累计对象整个生命周期内的应用发送包数/字节数、接收包数/字节数、丢弃包数及失败发送数，内部 STUN 不计入。接收丢弃数只覆盖已交到传输层的数据，agent 拒绝的错误来源/STUN 不计入。成功发送表示数据交给本机网络栈，不保证对端交付。

## Restart、线程和关闭

信令操作在传输内部排队，前一个操作完成后才执行下一个；队列默认最多 256 项，超限返回 `no_buffer_space`。所有用户回调在传输 strand 上串行执行，不与 agent 的内部回调并行。`snapshot` 通过互斥锁取得一致副本。

`restart` 取消未完成的信令操作，清除选中通道与 DTLS 参数，递增 generation，再重置 agent。重新读取本地凭据并交换新 SDP 后调用 `start`。旧代的状态、数据和就绪事件不会进入新代；已交给 Socket 的发送仍按实际完成结果回调和计数。同一代修改远端 ICE 凭据或 DTLS 身份/角色返回 `already_started`，需先 restart。重复导入等价的指纹（包含大小写差异）不会重新触发 DTLS 就绪通知。

SDP 导入中的某个候选解析失败可能发生在有效凭据和其他候选已导入之后，此时保留已经接受的参数，并通过操作完成回调报告错误。无效 SDP 或同一代 DTLS 身份冲突在交给 agent 之前拒绝。

`close` 幂等，取消信令操作并关闭 agent，完成回调等待所有已接受的数据发送完成回调。对象析构会请求关闭；io_context 必须存活并继续运行至关闭完成。用户回调不得抛异常。

传输范围沿用 agent：支持 UDP full ICE、远端 ICE-lite、host/srflx/prflx、双组件和系统 DNS/mDNS 或显式解析端点；不实现本地 TURN Allocate/ChannelData、ICE-TCP 或 DTLS 本身。

候选里的 UDP/TCP 枚举已由 `IceTransport` 改名为 `IceCandidateTransport`，避免与传输类重名；相关候选、agent 和测试引用已同步。外部代码的 `IceTransport::Udp/Tcp` 或枚举类型声明应相应改名。

## 验证

```bash
cmake -S tests/webrtc -B /tmp/uav-webrtc-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/uav-webrtc-build --parallel 4
ctest --test-dir /tmp/uav-webrtc-build --output-on-failure
```

传输测试包含全部首/第二字节分类边界、IPv4/IPv6 真实回环收发、BUNDLE 参数继承、DTLS 角色选择、晚到 SDP、指纹冲突、DTLS/SRTP/SRTCP 分流、双组件、统计、长度/类型/未就绪错误、restart、操作取消、队列上限、发送中的关闭与多线程生命周期。
