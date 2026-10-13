# WebRTC SDP

`src/webrtc/sdp.h/.cpp` 提供 `webrtc::SdpSession`、`SdpMedia` 和相关属性模型，负责 SDP 解析、校验和序列化。文本结构参考 [RFC 8866](https://www.rfc-editor.org/rfc/rfc8866.html)，WebRTC Offer/Answer 使用规则参考 [RFC 8829](https://www.rfc-editor.org/rfc/rfc8829.html)，BUNDLE 传输归属参考 [RFC 8843 §7.1.3](https://www.rfc-editor.org/rfc/rfc8843.html#section-7.1.3)。

## 解析与访问

```cpp
#include "webrtc/sdp.h"

webrtc::SdpSession offer;
std::string reason;
if (!webrtc::SdpSession::parse(remoteSdp, offer, &reason)) {
    // 语法、结构或引用不合法；offer 保持原值。
}
if (!offer.validateWebRtc(webrtc::SdpDescriptionType::Offer, &reason)) {
    // 不满足本模块支持的初始 WebRTC offer 条件。
}
if (const auto* video = offer.findMedia("1")) {
    auto direction = offer.effectiveDirection(*video);
    auto h264 = video->codecsNamed("H264");
    for (const auto* codec : h264) {
        auto profile = codec->parameter("profile-level-id");
        auto packetization = codec->parameter("packetization-mode");
        auto rtx = video->rtxPayloadType(codec->payloadType);
        // 将完整参数与本地编码器能力比较，确定可接受的格式。
    }
}
auto canonicalSdp = offer.toSdp(); // 统一 CRLF，保留已识别字段和未知属性
```

Offer/Answer 类型由信令层提供，不在 SDP 文本里。`parse` 接受 CRLF、LF 和无结尾换行的输入，检查必需的 `v/o/s/t`、字段顺序、重复单值属性、整数范围和跨字段引用；失败不修改输出。`toSdp` 先校验模型，无效数据抛 `std::invalid_argument`。序列化采用规范顺序和 CRLF，不承诺逐字复现原始空白、属性排列或显式 `/1` 声道标记。

## 支持的内容

- 会话的 origin、会话名、单播连接地址、信息/URI/邮箱/电话、带宽、时间信息和属性。
- 音频、视频、application 及保留的其他媒体类型；端口 0 的拒绝段、端口数量、格式顺序和媒体级连接信息。
- BUNDLE/group、MID、bundle-only、媒体方向、会话/媒体层继承。
- ICE ufrag/password/options、ice-lite、`IceCandidate` 候选和 end-of-candidates。
- DTLS fingerprint/setup，包括同一算法对多个证书的不同指纹；指纹只做格式校验，证书验证由 DTLS 层负责。
- RTP rtpmap/fmtp、静态和动态 payload、RTX apt、RTCP 地址、rtcp-mux/mux-only/rsize 和逐 PT/通配反馈。
- RTP extmap、方向、附加值、extmap-allow-mixed，以及 4096~4351 的协商标识范围。
- SCTP sctp-port 和 max-message-size，后者区分未声明与声明为 0。
- 未建模的属性及重复项保留在 `common.attributes`，包括 msid、ssrc、ssrc-group、rid、simulcast、tls-id 和厂商属性。它们只做字符/长度校验，不解释协议私有语义。

连接地址支持 IN/IP4、IN/IP6 的裸单播 IP 或主机名，不解析 DNS，不支持 SDP 多播地址/TTL 或 IPv6 作用域标记。`i/u/e/p/k/r/z` 等辅助内容可保留；WebRTC 校验要求永久会话 `t=0 0`。完整 SDP 限制为 256KiB、4096 行、每行 8192 字节、64 个媒体段、每段最多 128 个格式。ICE candidate 仍遵循自身的 4096 字节限制。

## 校验与 BUNDLE

`validate()` 检查模型的结构和已建模字段，不要求一般 SDP 具有 ICE/DTLS。动态 RTP 格式必须有 rtpmap；RTX 的 apt 必须引用同一媒体段中存在的主编码，不能自引用、引用 RTX 或使用不同的时钟频率。BUNDLE/group 不能引用不存在或重复的 MID，一个 MID 不能同时加入多个 BUNDLE 组。

`validateWebRtc(Offer/Answer)` 面向初始 WebRTC 协商，进一步检查 MID、ICE/DTLS、角色、单端口、RTCP mux、安全 RTP transport 和 SCTP data-channel 格式；允许已拒绝的媒体段省略传输属性。初始 offer 使用 actpass，answer 使用 active/passive。RTP/RTCP 复用时拒绝 64~95 的 RTP payload，依据 [RFC 5761 §4](https://www.rfc-editor.org/rfc/rfc5761.html#section-4)。本方法不替代 PeerConnection 的协商状态机和本地能力选择，也不用于验证任意重协商上下文。

`effectiveAttributes(media)` 和 `effectiveConnection(media)` 只处理会话层向媒体层的继承；方向默认为 sendrecv。`transportMedia(media, type)` 处理 BUNDLE 的传输归属：初始 offer 的普通媒体段使用自己的传输，bundle-only 段及 answer 的已分组媒体使用组内第一个 MID 的 tagged 媒体。读取共享 ICE/DTLS、候选和 RTCP mux 时，应先取 transport media；读取编解码器、媒体方向和 MSID 时继续使用原媒体段。

```cpp
const auto& transport = answer.transportMedia(*answer.findMedia("2"),
                                              webrtc::SdpDescriptionType::Answer);
auto transportParameters = answer.effectiveAttributes(transport);
const auto& candidates = transport.candidates;
```

tagged 媒体可以是 application，此时允许它携带组内 RTP 媒体需要的 RTCP mux 属性。为兼容重复传输字段的 answer，允许非 tag 媒体重复相同的 ICE/DTLS 信息，拒绝与 tag 冲突的凭据、setup 和 fingerprint。候选仍由 ICE agent 处理。

## 构建本地 SDP

```cpp
std::string makeH264Offer(std::uint64_t sessionId,
                         const std::string& localUfrag,
                         const std::string& localPassword,
                         const std::string& localSha256Fingerprint)
{
    using namespace webrtc;
    SdpSession session;
    session.origin.sessionId = sessionId;
    session.origin.sessionVersion = 1;
    session.common.iceUfrag = localUfrag;
    session.common.icePwd = localPassword;
    session.common.fingerprints = {{"sha-256", localSha256Fingerprint}};
    session.common.setup = SdpSetupRole::ActPass;

    SdpMedia video;
    video.mid = "0";
    video.connection = SdpConnection{};
    video.common.direction = SdpDirection::SendOnly;
    video.rtcpMux = true;
    video.formats = {"102"};
    video.codecs = {{102, "H264", 90000, 1,
                    "packetization-mode=1;profile-level-id=42e01f"}};
    session.media.push_back(std::move(video));
    session.groups = {{"BUNDLE", {"0"}}};
    std::string reason;
    if (!session.validateWebRtc(SdpDescriptionType::Offer, &reason))
        throw std::invalid_argument(reason);
    return session.toSdp();
}
```

示例编码参数须与实际编码器一致。ICE 凭据、证书指纹和候选由本地运行时提供。构造 answer 时保留 offer 的媒体数量、顺序和 MID，按本地能力选择格式或拒绝媒体；`answerDirection` 和 `answerSetupRole` 可计算方向/角色，但不会自动选择 H.264 profile、修改 fmtp、生成证书或建立连接。

测试：

```bash
cmake -S tests/webrtc -B /tmp/uav-webrtc-build
cmake --build /tmp/uav-webrtc-build --parallel 2
ctest --test-dir /tmp/uav-webrtc-build --output-on-failure
```
