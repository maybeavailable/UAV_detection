#pragma once

#include<boost/asio/ip/udp.hpp>

#include<cstdint>
#include<optional>
#include<string>
#include<string_view>
#include<utility>
#include<vector>

// ICE 候选数据模型：RFC 8445 优先级、RFC 8839 SDP、RFC 6544 TCP 候选。
// 候选收集、STUN/TURN 事务、连通性检查及 mDNS/DNS 解析由 ICE agent 负责。
namespace webrtc
{
enum class IceCandidateType : std::uint8_t { Host, ServerReflexive, PeerReflexive, Relay };
enum class IceCandidateTransport : std::uint8_t { Udp, Tcp };
enum class IceTcpType : std::uint8_t { Active, Passive, SimultaneousOpen };

// 未知枚举值返回 nullptr。
const char* candidateTypeString(IceCandidateType type);
const char* transportString(IceCandidateTransport transport);
const char* tcpTypeString(IceTcpType type);
unsigned defaultTypePreference(IceCandidateType type);

// 非法参数抛 std::invalid_argument；component=1~256，localPreference=0~65535。
std::uint32_t candidatePriority(unsigned typePreference, unsigned localPreference, unsigned component = 1);
std::uint32_t candidatePriority(IceCandidateType type, unsigned localPreference = 65535, unsigned component = 1);
// 角色决定 G/D 的方向；两侧优先级必须为 1~2^31-1，计算使用 64 位。
std::uint64_t candidatePairPriority(std::uint32_t localPriority, std::uint32_t remotePriority, bool localControlling);

struct IceCandidate
{
    using Endpoint = boost::asio::ip::udp::endpoint; // 与 UdpSocket::Endpoint 相同
    using Extension = std::pair<std::string, std::string>;

    std::string foundation; // 1~32 个字母、数字、+、/；由收集器按 base/类型/服务端分配
    std::uint16_t component{1}; // RTP=1，非复用 RTCP=2
    IceCandidateTransport transport{IceCandidateTransport::Udp};
    std::uint32_t priority{0};
    std::string address; // 裸 IPv4/IPv6 或远端主机名，包括浏览器的 .local 地址
    std::uint16_t port{0};
    IceCandidateType type{IceCandidateType::Host};
    std::optional<std::string> relatedAddress;
    std::optional<std::uint16_t> relatedPort;
    std::optional<IceTcpType> tcpType;
    std::vector<Extension> extensions; // 保留 generation、ufrag、network-id 等扩展及顺序

    // 默认对象不是有效候选。validate/parse 成功时清空 reason，失败时给出原因。
    bool validate(std::string* reason = nullptr) const;
    // 接受 candidate:... 或 a=candidate:...，允许末尾单个 CRLF/LF。
    // 支持 UDP/TCP 及四种标准候选类型；拒绝溢出、重复保留字段及多行输入。
    // 输入上限 4096 字节；失败不修改 out；空字符串/end-of-candidates 不属于候选。
    static bool parse(std::string_view line, IceCandidate& out, std::string* reason = nullptr);
    std::string toCandidateString() const; // candidate:...，用于信令；无效对象抛 invalid_argument
    std::string toSdp() const; // a=candidate:...\r\n

    bool isHostname() const;
    bool isMdns() const;
    // 仅对有效、使用 IP 字面量的 UDP 候选返回端点，不发起 DNS/mDNS 解析。
    std::optional<Endpoint> udpEndpoint() const;
    std::optional<std::string> extension(std::string_view name) const; // 名称不区分大小写，取首项

    // 从已知 UDP 地址创建候选；reflexive/relay 必须提供 related，host 必须省略。
    // local 应为具体接口地址，不能把 UdpSocket 绑定的 0.0.0.0/:: 当成本地 host 候选。
    // relay 的 related 是 TURN Allocate 返回的 mapped 地址，不等同于 relay 的 base。
    // foundation/唯一 localPreference 由收集器维护；非法数据抛 invalid_argument。
    static IceCandidate fromUdpEndpoint(const Endpoint& endpoint, IceCandidateType type,
                                        std::string foundation, unsigned component = 1,
                                        unsigned localPreference = 65535,
                                        std::optional<Endpoint> related = std::nullopt);
    static IceCandidate host(const Endpoint& local, std::string foundation,
                              unsigned component = 1, unsigned localPreference = 65535);
};
} // namespace webrtc
