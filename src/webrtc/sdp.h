#pragma once

#include"webrtc/ice_candidate.h"

#include<cstdint>
#include<optional>
#include<string>
#include<string_view>
#include<vector>

// WebRTC SDP 数据模型与编解码。Offer/Answer 类型由信令层提供，不在 SDP 文本中编码。
// 本模块不生成 ICE 凭据/证书，不执行 DTLS、STUN 或媒体编码能力协商。
namespace webrtc
{
enum class SdpDescriptionType { Offer, Answer };
enum class SdpDirection { SendRecv, SendOnly, RecvOnly, Inactive };
enum class SdpSetupRole { ActPass, Active, Passive, HoldConn };
const char* directionString(SdpDirection direction); // 未知值返回 nullptr
const char* setupRoleString(SdpSetupRole role);
SdpDirection answerDirection(SdpDirection offered, bool wantSend, bool wantReceive);
SdpSetupRole answerSetupRole(SdpSetupRole offered, bool preferActive = true); // holdconn/未知值抛 invalid_argument

struct SdpAttribute
{
    std::string name;
    std::optional<std::string> value; // nullopt 为 flag，空字符串表示 name:
};
struct SdpConnection
{
    std::string addressType{"IP4"}, address{"0.0.0.0"}; // IN 网络；裸单播 IP 或域名
};
struct SdpOrigin
{
    std::string username{"-"};
    std::uint64_t sessionId{0}, sessionVersion{0};
    SdpConnection connection;
};
struct SdpTiming
{
    std::uint64_t start{0}, stop{0};
    std::vector<std::string> repeats;
};
struct SdpBandwidth { std::string type; std::uint64_t value{0}; };
struct SdpFingerprint { std::string algorithm, value; };
struct SdpExtMap
{
    unsigned id{0}; // 1~256 或协商用的 4096~4351
    std::optional<SdpDirection> direction;
    std::string uri;
    std::optional<std::string> attributes;
};
struct SdpCommonAttributes
{
    std::optional<SdpDirection> direction;
    std::optional<std::string> iceUfrag, icePwd;
    std::vector<std::string> iceOptions;
    std::vector<SdpFingerprint> fingerprints;
    std::optional<SdpSetupRole> setup;
    bool extmapAllowMixed{false};
    std::vector<SdpExtMap> extmaps;
    std::vector<SdpAttribute> attributes; // msid、ssrc、ssrc-group、rid、simulcast 等保留原值
};
struct SdpCodec
{
    unsigned payloadType{0};
    std::string encoding;
    std::uint32_t clockRate{0};
    unsigned channels{1};
    std::optional<std::string> parameters; // fmtp 原文，不重写 codec 私有参数
    std::optional<std::string> parameter(std::string_view name) const; // 查找首个 key=value 参数
};
struct SdpRtcpFeedback
{
    std::optional<unsigned> payloadType; // nullopt 表示 *
    std::string feedback; // nack、nack pli、ccm fir、transport-cc 等
};
struct SdpRtcp
{
    std::uint16_t port{0};
    std::optional<SdpConnection> connection;
};
struct SdpGroup { std::string semantics; std::vector<std::string> mids; };

struct SdpMedia
{
    std::string kind{"video"};
    std::uint16_t port{9};
    std::optional<unsigned> portCount;
    std::string protocol{"UDP/TLS/RTP/SAVPF"};
    std::vector<std::string> formats; // m= 格式顺序；RTP 为 PT，SCTP 为 webrtc-datachannel
    std::optional<std::string> information, key, mid;
    std::optional<SdpConnection> connection;
    std::vector<SdpBandwidth> bandwidth;
    SdpCommonAttributes common;
    bool bundleOnly{false}, rtcpMux{false}, rtcpMuxOnly{false}, rtcpRsize{false};
    std::optional<SdpRtcp> rtcp;
    std::vector<SdpCodec> codecs; // rtpmap/fmtp，可保留只有 fmtp 的静态 PT
    std::vector<SdpRtcpFeedback> feedback;
    std::vector<IceCandidate> candidates;
    bool endOfCandidates{false};
    std::optional<std::uint16_t> sctpPort;
    std::optional<std::uint64_t> maxMessageSize; // 0 表示不设上限；未声明时保留 nullopt

    bool isRtp() const;
    bool isSctp() const;
    bool isRejected() const; // port=0 且非 bundle-only
    const SdpCodec* codec(unsigned payloadType) const;
    std::vector<const SdpCodec*> codecsNamed(std::string_view encoding) const;
    std::optional<unsigned> rtxPayloadType(unsigned primaryPayload) const;
    const SdpAttribute* attribute(std::string_view name) const; // 未建模属性的首项
};

struct SdpSession
{
    SdpOrigin origin;
    std::string sessionName{"-"};
    std::optional<std::string> information, uri, key, timeZones;
    std::vector<std::string> emails, phones;
    std::optional<SdpConnection> connection;
    std::vector<SdpBandwidth> bandwidth;
    std::vector<SdpTiming> timings{{0, 0, {}}};
    SdpCommonAttributes common;
    std::vector<SdpGroup> groups;
    bool iceLite{false};
    std::vector<SdpMedia> media;

    // 支持 CRLF/LF 和最后一行无换行；失败不修改 out，语法错误 reason 含行号。
    // 最大 256KiB、4096 行、单行 8192 字节、64 个媒体段。
    static bool parse(std::string_view text, SdpSession& out, std::string* reason = nullptr);
    bool validate(std::string* reason = nullptr) const; // 结构、引用及已建模字段校验
    // 额外检查初始 WebRTC Offer/Answer：MID、ICE、DTLS、RTCP mux、SCTP、BUNDLE 等。
    bool validateWebRtc(SdpDescriptionType type, std::string* reason = nullptr) const;
    std::string toSdp() const; // 规范化 CRLF；无效模型抛 std::invalid_argument

    const SdpMedia* findMedia(std::string_view mid) const;
    SdpMedia* findMedia(std::string_view mid);
    const SdpGroup* bundleGroup(std::string_view mid) const;
    // 初始 offer 的普通媒体有独立传输；bundle-only 和 answer 使用 BUNDLE 的 tagged 媒体。
    const SdpMedia& transportMedia(const SdpMedia& section, SdpDescriptionType type) const;
    // 只做会话层 → 媒体层继承，BUNDLE 传输归属通过 bundleGroup 的首个 MID 获取。
    SdpCommonAttributes effectiveAttributes(const SdpMedia& section) const;
    SdpDirection effectiveDirection(const SdpMedia& section) const;
    std::optional<SdpConnection> effectiveConnection(const SdpMedia& section) const;
};
} // namespace webrtc
