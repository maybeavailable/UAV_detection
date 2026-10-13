#pragma once

#include"common/udp_socket.h"
#include"webrtc/ice_candidate.h"
#include"webrtc/stun_protocol.h"

#include<chrono>
#include<functional>
#include<memory>
#include<optional>
#include<string>
#include<vector>

namespace webrtc
{
struct SdpSession;
enum class SdpDescriptionType;

struct IceCredentials
{
    std::string ufrag, password;
    bool validate(std::string* reason = nullptr) const;
    static IceCredentials generate(); // OpenSSL CSPRNG，16 字符 ufrag / 32 字符 password
};
enum class IceAgentState { New, Checking, Connected, Completed, Disconnected, Failed, Closed };
enum class IceGatheringState { New, Gathering, Complete };
enum class IcePairState { Frozen, Waiting, InProgress, Succeeded, Failed };

struct IceCandidatePair
{
    std::uint64_t id{0}, priority{0};
    IceCandidate local, remote; // local 在成功后为响应的 mapped 候选
    UdpSocket::Endpoint localEndpoint, remoteEndpoint; // localEndpoint 是实际发送的 base Socket
    IcePairState state{IcePairState::Frozen};
    bool valid{false}, nominated{false};
};

// 一个 agent 管理一个 ICE transport（一个 BUNDLE 组或独立媒体），可含 RTP/RTCP 两个组件。
// 所有操作异步投递到内部 strand；回调串行且不得抛异常；io_context 由调用者运行。
// UDP full ICE，regular nomination；支持远端 ICE-lite，不实现本地 lite / ICE-TCP / TURN Allocate。
class IceAgent
{
public:
    using ErrorCode = UdpSocket::ErrorCode;
    using Completion = std::function<void(ErrorCode)>;
    struct LocalBinding
    {
        UdpSocket::Endpoint endpoint{boost::asio::ip::udp::v4(), 0};
        std::uint16_t component{1};
        unsigned localPreference{65535};
    };
    struct Config
    {
        IceCredentials credentials; // 空值时自动生成
        StunIceRole role{StunIceRole::Controlling}; // 通常 offerer controlling，answerer controlled
        std::optional<std::uint64_t> tieBreaker; // 未指定时 CSPRNG；用于可重复测试时可指定
        std::vector<std::uint16_t> components{1}; // RTCP mux / SCTP 为 {1}，非 mux RTP 为 {1, 2}
        std::vector<LocalBinding> bindings; // 空时枚举本机 IPv4/IPv6；每个具体地址单独绑定 Socket
        bool includeLoopback{false}; // 显式 bindings 的回环地址始终允许
        std::vector<UdpSocket::Endpoint> stunServers; // 已解析的公网 STUN UDP 端点
        std::size_t maxCandidates{256}, maxCandidatePairs{100}, maxPendingTransactions{1024};
        std::chrono::milliseconds checkInterval{50}, initialRto{500}, checkTimeout{60000};
        unsigned maxTransmissions{7}; // 含首次发送；最后发送后再等待 16 * initialRto
        // RFC 7675：每次请求仅发一次，周期随机化为 0.8~1.2 倍；最短实际周期 4 秒。
        std::chrono::milliseconds consentInterval{5000}, disconnectedTimeout{15000}, consentTimeout{30000};
    };
    struct Callbacks
    {
        std::function<void(IceAgentState)> onState;
        std::function<void(IceGatheringState)> onGatheringState;
        std::function<void(IceCandidate)> onLocalCandidate;
        std::function<void(IceCandidatePair)> onSelectedPair;
        std::function<void(std::uint16_t component, UdpSocket::Datagram)> onData; // 已选中对端的非 STUN 数据
        std::function<void(ErrorCode, std::string)> onError; // 操作/网络错误，不回显 ICE 密码
    };
    struct Snapshot
    {
        IceAgentState state{IceAgentState::New};
        IceGatheringState gatheringState{IceGatheringState::New};
        StunIceRole role{StunIceRole::Controlling};
        IceCredentials localCredentials;
        std::optional<IceCredentials> remoteCredentials;
        std::vector<IceCandidate> localCandidates, remoteCandidates;
        std::vector<IceCandidatePair> checklist, selectedPairs;
        bool remoteEndOfCandidates{false};
        std::size_t pendingTransactions{0};
    };

    explicit IceAgent(boost::asio::io_context& io);
    IceAgent(boost::asio::io_context& io, Config config, Callbacks callbacks = {});
    ~IceAgent();
    IceAgent(const IceAgent&) = delete;
    IceAgent& operator=(const IceAgent&) = delete;
    Snapshot snapshot() const; // 任意线程读取一致快照；异步操作完成后才反映变化

    void gather(Completion done = {}); // 幂等；done 表示已启动，Complete 表示包括 STUN 收集已结束
    // 已有具体地址的 Socket 也可注册。接管其接收回调，close 时关闭它，不可同时供其他接收者使用。
    void addLocalTransport(std::shared_ptr<UdpSocket> socket, std::uint16_t component = 1,
                           unsigned localPreference = 65535, Completion done = {});
    void setRemoteCredentials(IceCredentials credentials, bool iceLite = false, Completion done = {});
    // 支持 IP 字面量和系统 resolver 可解析的域名；.local 需要系统 mDNS 或传 resolvedEndpoint。
    // candidate 中的 ufrag 扩展 / generationUfrag 用来拒绝过期 ICE generation。
    void addRemoteCandidate(IceCandidate candidate, Completion done = {},
                            std::optional<UdpSocket::Endpoint> resolvedEndpoint = std::nullopt,
                            std::string generationUfrag = {});
    void endOfRemoteCandidates(std::string generationUfrag = {}, Completion done = {});
    // 自动按 BUNDLE 传输归属读取 ICE 凭据、candidates、ice-lite/end-of-candidates。
    void setRemoteDescription(SdpSession session, std::string mid, SdpDescriptionType type, Completion done = {});
    void start(Completion done = {}); // 可在候选到齐前开始；等待 trickle，不提前 Failed
    // 仅允许选中且 consent 尚未过期的候选对，供 DTLS/SRTP/SCTP 发送；未选中返回 not_connected。
    void asyncSend(UdpSocket::Bytes payload, std::uint16_t component = 1, UdpSocket::SendHandler done = {});
    // 新 generation 清除远端候选/凭据、事务、提名和 consent，保留本地 Socket/候选。
    // 空 credentials 自动生成；已有连接立即停止应用数据，必须交换新 SDP 后重新 start。
    void restart(IceCredentials credentials = {}, StunIceRole role = StunIceRole::Controlling, Completion done = {});
    void close(Completion done = {}); // 幂等；done 在 timers/resolvers/所有 Socket 关闭后调用

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace webrtc
