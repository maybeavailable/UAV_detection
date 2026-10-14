# DataChannel

`webrtc::DataChannel` 和 `DataChannelTransport` 位于 `src/webrtc/datachannel.h/.cpp`。使用 usrsctp 实现 SCTP over DTLS，接入现有 `PeerConnection` → `DtlsTransport` → `IceTransport` → UDP 链路。应用发送文本或二进制完整消息，不需要自行封装 SCTP。

支持 DCEP OPEN/ACK、自动 stream ID 分配、带外协商通道、有序/无序消息、可靠传输、按重传次数或生存时间限制的部分可靠传输、空消息、大消息分片重组、发送背压，以及双向 stream reset 关闭。DCEP 的 label/protocol 使用 UTF-8；文本消息也进行 UTF-8 校验。

## PeerConnection 接入

接收远端通道的一方设置 `Config::enableDataChannels=true`。本地主动调用 `createDataChannel` 也会启用内置 SCTP，并在能力列表中补充 application m-line。第一次启用应在创建 offer 前、Stable 状态下完成；双方仍需通过业务信令交换 SDP 和 ICE candidate，参见 [PeerConnection](peerconnection.md)。

```cpp
#include "webrtc/peerconnection.h"

boost::asio::io_context io;
webrtc::PeerConnection::Config config;
config.enableDataChannels = true;
// 自动添加 application 能力，可同时配置 config.media 中的音视频能力。

auto attach = [](std::shared_ptr<webrtc::DataChannel> channel) {
    std::weak_ptr<webrtc::DataChannel> weak = channel;
    webrtc::DataChannel::Callbacks handlers;
    handlers.onOpen = [weak] {
        if (auto c = weak.lock()) c->sendText("hello", [](auto ec, auto bytes) {
            if (ec) reportSendError(ec);
            // 成功表示 SCTP 接受了整个消息；bytes 为应用消息字节数。
        });
    };
    handlers.onMessage = [](webrtc::DataChannelMessage message) {
        if (message.binary) consumeBinary(std::move(message.data));
        else consumeText(std::string(message.data.begin(), message.data.end()));
    };
    handlers.onClosed = [] { reportChannelClosed(); };
    handlers.onError = [](auto ec, std::string reason) { reportError(ec, reason); };
    channel->setCallbacks(std::move(handlers));
};

webrtc::PeerConnection::Callbacks callbacks;
callbacks.onDataChannel = attach; // 仅远端 DCEP OPEN 触发
callbacks.onIceCandidate = [](auto candidate) { sendCandidateThroughSignaling(candidate); };
callbacks.onError = [](auto ec, std::string reason) { reportError(ec, reason); };
webrtc::PeerConnection peer(io, config, callbacks);

std::shared_ptr<webrtc::DataChannel> commands;
webrtc::DataChannelConfig options;
options.label = "commands";
options.protocol = "json";
peer.createDataChannel(options, [&](auto ec, auto channel) {
    if (ec) { reportError(ec, "createDataChannel"); return; }
    commands = channel;
    attach(channel);
    // 此处开始 createOffer → setLocalDescription → 发送 SDP 的信令流程。
    beginOfferNegotiation(peer);
});
io.run(); // 关闭时调用 peer.close(done)，持续运行 io_context 到完成。
```

示例的业务处理和信令函数由应用提供。`onOpen` 表示该通道可发送消息，不能仅根据 PeerConnection 的 Connected 状态判断；后者只表示 ICE/DTLS 可用，SCTP/DCEP 还可能正在握手。已有 application m-line 协商成功后，创建更多通道不需要再次交换 SDP。

默认 `enableDataChannels=false` 保留已有 raw DTLS API。该模式的 `onDtlsData`/`asyncSendDtls` 用于外部 SCTP 实现；启用内置 DataChannel 后，收到的 DTLS 明文交给内置 SCTP，不再调用 `onDtlsData`。

## 通道配置与状态

- 默认 `ordered=true`，不设置可靠性参数时使用可靠传输。
- `ordered=false; maxRetransmits=0` 适合可以丢弃旧数据的遥测。`maxPacketLifeTime` 单位为毫秒，与 `maxRetransmits` 互斥；计时包含本地发送队列中的等待时间。
- 默认 `negotiated=false`，通过 DCEP 协商，SCTP association 初次建立时 DTLS Active 端分配偶数 SID，Passive 端分配奇数 SID。可显式指定符合该规则的 `id`。
- `negotiated=true` 时必须指定 `id`，双方通过外部信令约定相同 id 和配置，并分别调用 `createDataChannel`。不发送 OPEN/ACK，也不会触发远端的 `onDataChannel`；带外通道的 id 不受奇偶规则限制。
- id 范围为 0..65534，还须小于双方协商后的 stream 数量。默认请求双向 1024 个 streams，同时活动通道上限为 128，可通过 transport 配置调整。当前不动态增加 SCTP streams。
- `priority` 对应 DCEP 的 16 位优先级，值越大优先级越高，常用值为 128/256/512/1024；当前映射到 native SCTP 的严格优先级调度，尚未实现 RFC 8831 的加权公平调度。

`snapshot()` 可跨线程读取状态、最终配置/id、发送上限、缓冲字节数和消息/字节累计统计。通道状态依次为 Connecting → Open → Closing → Closed，也可能因握手失败直接关闭。发送 Connecting/Closing/Closed 通道会得到错误。

`sendText` 和 `sendBinary` 保留消息边界。完成回调成功表示整个消息已被本地 SCTP 接受，不表示对端收到或应用处理完成；需要业务确认时请增加应用 ACK。空消息成功时字节数为 0，协议使用单字节占位及对应的 empty PPID。

`close(done)` 的 done 表示已发起关闭；双向 SCTP stream reset 完成或关闭超时后触发 `onClosed`。只有双方 reset 都完成，SID 才可复用。`peer.close(done)` 会取消排队消息，关闭所有通道，再关闭 DTLS/ICE；已接受的发送回调先于 PeerConnection 的关闭完成回调。

## 消息上限、背压与线程

本地最大消息默认 65536 字节，通过 SDP `a=max-message-size` 声明。接收端未声明时按 65536 处理，远端声明 0 时发送仍受本地实现上限限制。发送上限是本地配置上限和远端声明上限的较小值，可从通道 Snapshot 读取；超限返回 `message_size`。应用显式提供的本地 SDP 也应与实际接收配置一致。

可通过 `config.dataChannel.maxMessageSize` 增大上限，最大为 16 MiB。还须让 `maxBufferedSendBytes`、`maxBufferedReceiveBytes` 和 `socketBufferSize` 满足构造校验；接收缓冲及 SCTP socket 缓冲至少可容纳一条最大消息及协议要求的 DCEP 消息。DCEP label/protocol 最长分别为 65535 字节，其控制消息上限独立于应用消息上限。

发送队列默认限制 2 MiB/256 条消息，跨线程调用在投递到 strand 前预留额度，超限异步返回 `no_buffer_space`，空消息也占用消息数额度。`bufferedAmount` 是等待 SCTP 接受的应用消息字节数，不包含 SCTP 自身待确认、拥塞控制和重传缓冲。它从大于阈值降到小于等于 `bufferedAmountLowThreshold` 时触发 `onBufferedAmountLow`；单独修改阈值不触发事件。

暂未安装 `onMessage` 的完整消息在有界接收队列内等待；安装回调后按顺序交付。接收队列满时暂停读取 native SCTP socket，让 SCTP 接收窗口产生背压。SCTP 报文输入投递、native 输出以及交给 DTLS 的队列也有容量限制。

所有异步 API 可跨线程调用，Snapshot 由互斥锁保护。PeerConnection 的 `createDataChannel` 完成及 `onDataChannel` 在 PeerConnection strand 上运行；DataChannel 的事件、发送完成和通道关闭回调在 SCTP transport strand 上运行。这两个 strand 在多线程 io_context 上可能并发，业务共享状态需自行同步。回调不得抛异常，也不要强引用捕获其所属的通道，避免引用环。

## 生命周期与独立 transport

ICE restart/暂时断连时暂停 SCTP 输出，保留 association、通道和有界排队消息。新的 DTLS 建立且对端证书身份、SCTP 端口相同后恢复；新的 generation 不接收旧代报文。保留 association 时沿用初次建立的 SID 分配角色，避免角色改变引起 ID 冲突。native SCTP 重传计时器仍运行，长时间中断可能使 association 失效。

若 SCTP/DTLS 已失败，原通道进入 Closed；更高 ICE generation 可创建新的 SCTP transport，应用需重建通道。现有 association 的证书身份或 SCTP 端口改变返回 `already_started`，这类身份迁移应建立新的 PeerConnection。未实现旧式 partial PPID 52/54，也未启用 RFC 8260 I-DATA message interleaving。

也可单独构造 `DataChannelTransport(io, sender, config, callbacks)`。Sender 接收一整个 SCTP 报文，应调用 `DtlsTransport::asyncSend` 并恰好完成一次回调。DTLS Connected 后调用 `start(parameters)`，设置经过验证的 fingerprint 身份、实际 DTLS 角色、SDP 的端口/消息上限以及 `maxPacketSize <= dtls.snapshot().maxPlaintextSize`；解密后的 SCTP 报文通过 `handlePacket(bytes, generation)` 输入。start 的完成只表示 SCTP 初始化成功，association 建立另由 `onState(Connected)` 通知。

transport 的 `close(done)` 等待已交给 Sender 的回调及已接受的发送请求完成。usrsctp 在进程中初始化一次，其协议 timer 线程只向 Asio strand 投递有界报文，用户事件由 strand 交付；本模块不会调用全局 `usrsctp_finish()`。其他 usrsctp 使用者须保持全局运行时有效，避免重复初始化覆盖输出回调。

## 依赖与构建

新增 `cmake/FindUSRSCTP.cmake` 和导入目标 `USRSCTP::USRSCTP`。主工程及 PeerConnection 相关独立测试已链接此依赖。`third_party/usrsctp` 对**所有目标架构**都参与查找（`-DUSRSCTP_ROOT` / `$USRSCTP_ROOT` 优先级更高），因此板端默认用它、宿主机也能通过 configure；但那份库是 aarch64 的，宿主机测试要真正链接仍必须提供同架构的安装前缀，否则会落到链接期"架构不匹配"。

板端构建：

```sh
./third_party/usrsctp/build-aarch64.sh
```

脚本固定 usrsctp 0.9.5.0，校验官方 tag 源码包 SHA256 `260107caf318650a57a8caa593550e39bca6943e93f970c80d6c17e59d62cd92`，构建 PIC 静态库。BSD 许可保留在 `third_party/usrsctp/LICENSE.md`。

原生构建示例（先下载并解压同一版本的 [官方源码](https://github.com/sctplab/usrsctp/tree/0.9.5.0)）：

```sh
cmake -S /path/to/usrsctp-0.9.5.0 -B /tmp/usrsctp-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/tmp/usrsctp-host \
  -Dsctp_build_programs=OFF -Dsctp_build_shared_lib=OFF -Dsctp_debug=OFF
cmake --build /tmp/usrsctp-build -j4
cmake --install /tmp/usrsctp-build

cmake -S tests/webrtc -B /tmp/uav-webrtc-tests \
  -DCMAKE_BUILD_TYPE=Debug -DSRTP_ROOT=/usr -DUSRSCTP_ROOT=/tmp/usrsctp-host
cmake --build /tmp/uav-webrtc-tests -j4
ctest --test-dir /tmp/uav-webrtc-tests --output-on-failure
```

`datachannel_test` 覆盖 DCEP 编码、文本/二进制/空消息、256 KiB 分片重组、最大长度 DCEP、可靠消息丢包重传、带外通道、暂停恢复、可靠性生存时间、消息上限、有界并发发送、关闭取消、Sender 回调等待、同时关闭多个通道和 SID 复用。`datachannel_peer_test` 通过真实 UDP 验证 IPv4/IPv6 的 ICE → DTLS → SCTP → DCEP 收发、SDP 消息上限、ICE restart 和通道关闭。尚未执行浏览器互通测试。

协议参考 [WebRTC Data Channels（RFC 8831）](https://www.rfc-editor.org/rfc/rfc8831.html)、[DCEP（RFC 8832）](https://www.rfc-editor.org/rfc/rfc8832.html) 和 [SDP SCTP 属性（RFC 8841）](https://www.rfc-editor.org/rfc/rfc8841.html)。
