#pragma once

#include"webrtc/ice_agent.h"
#include"webrtc/sdp.h"

#include<cstddef>
#include<cstdint>
#include<functional>
#include<memory>
#include<optional>
#include<string>

namespace webrtc
{
enum class IcePacketType { Unknown, Stun, Zrtp, Dtls, TurnChannelData, Rtp, Rtcp };
// RFC 7983 首字节分流，RTP/RTCP 按 RFC 5761 的第二字节范围区分。
// 仅分类，不解析/认证 DTLS、SRTP 或 RTCP；空指针、空包、缺少第二字节的 RTP 返回 Unknown。
IcePacketType classifyIcePacket(const void* data, std::size_t size);
IcePacketType classifyIcePacket(const UdpSocket::Bytes& bytes);

enum class IceTransportState { New, Checking, Connected, Completed, Disconnected, Failed, Closed };
struct IceDtlsParameters
{
    std::string transportMid; // BUNDLE 所属传输的 MID
    std::vector<SdpFingerprint> remoteFingerprints; // 信令提供的证书摘要，供 DTLS 验证实际对端证书
    SdpSetupRole remoteSetup{SdpSetupRole::ActPass};
    SdpSetupRole localSetup{SdpSetupRole::Active}; // Active=DTLS client，Passive=server；与 ICE 角色独立
};
struct IceDtlsReady
{
    std::uint64_t generation{0};
    IceCandidatePair pair;
    IceDtlsParameters parameters;
};
struct IceTransportPacket
{
    std::uint64_t generation{0};
    std::uint16_t component{1};
    IcePacketType type{IcePacketType::Unknown};
    UdpSocket::Datagram datagram; // 拥有数据、源端点和接收时间；仍是加密的 DTLS / SRTP / SRTCP
};
struct IceTransportStats
{
    std::uint64_t sentPackets{0}, sentBytes{0}, receivedPackets{0}, receivedBytes{0};
    std::uint64_t discardedPackets{0}, failedSends{0}; // 不计入内部 STUN 检查；整个对象生命周期累计
};

// WebRTC 的 UDP ICE 传输边界，一个实例对应一个 BUNDLE 组或独立媒体传输。
// 拥有 IceAgent，统一候选/信令/生命周期入口，给 DTLS/SRTP 提供已选中通道和数据分流。
// 所有回调在内部 strand 上串行执行且不得抛异常；io_context 由调用者运行并保证其寿命。
class IceTransport
{
public:
    using ErrorCode = UdpSocket::ErrorCode;
    using Completion = IceAgent::Completion;
    using SendHandler = UdpSocket::SendHandler;
    struct Config
    {
        IceAgent::Config ice;
        bool preferDtlsClient{true}; // 对远端 actpass offer，选择本地 answer 的 active/passive
        std::size_t maxDatagramSize{65507};
        std::size_t maxPendingOperations{256}; // 不含数据发送，发送仍受 UdpSocket 队列限制
    };
    struct Callbacks
    {
        std::function<void(IceTransportState)> onState;
        std::function<void(IceGatheringState)> onGatheringState;
        std::function<void(IceCandidate)> onLocalCandidate;
        std::function<void(IceCandidatePair)> onSelectedPair; // 原始 ICE 通道已可用，不依赖是否提供 SDP
        std::function<void(IceDtlsReady)> onDtlsReady; // 已选中且有远端指纹/setup，每代每组件一次
        std::function<void(IceTransportPacket)> onPacket; // 仅 DTLS / SRTP / SRTCP，STUN 留在 agent
        std::function<void(ErrorCode, std::string)> onError;
    };
    struct Snapshot
    {
        IceTransportState state{IceTransportState::New};
        std::uint64_t generation{0};
        bool restarting{false};
        IceAgent::Snapshot ice;
        std::optional<IceDtlsParameters> dtls;
        IceTransportStats stats;
    };

    explicit IceTransport(boost::asio::io_context& io);
    IceTransport(boost::asio::io_context& io, Config config, Callbacks callbacks = {});
    ~IceTransport();
    IceTransport(const IceTransport&) = delete;
    IceTransport& operator=(const IceTransport&) = delete;

    Snapshot snapshot() const; // 可跨线程；异步操作完成/回调后反映变化
    std::optional<IceCandidatePair> selectedPair(std::uint16_t component = 1) const;
    bool isReady(std::uint16_t component = 1) const; // 已提名且未 Failed/Closed/restarting；最终 consent 由 agent 检查
    void gather(Completion done = {});
    void addLocalTransport(std::shared_ptr<UdpSocket> socket, std::uint16_t component = 1,
                           unsigned localPreference = 65535, Completion done = {});
    void setRemoteCredentials(IceCredentials credentials, bool iceLite = false, Completion done = {});
    void addRemoteCandidate(IceCandidate candidate, Completion done = {},
                            std::optional<UdpSocket::Endpoint> resolvedEndpoint = std::nullopt,
                            std::string generationUfrag = {});
    void endOfRemoteCandidates(std::string generationUfrag = {}, Completion done = {});
    // 校验 SDP/BUNDLE，保存远端 fingerprint/setup 并确定本地 DTLS 角色，再交给 agent 导入 ICE 信息。
    // 同一代改变 ICE 凭据或 DTLS 身份/角色必须先 restart；候选解析错误可能保留已导入的有效信息。
    void setRemoteDescription(SdpSession session, std::string mid, SdpDescriptionType type, Completion done = {});
    void start(Completion done = {});

    // 只发送 DTLS / SRTP / SRTCP；不加密/解密、不生成握手。未选中不排队，返回 not_connected。
    // 类型由报文头判别；专用方法额外检查类型。未知、STUN、TURN、ZRTP 返回 invalid_argument。
    void asyncSend(UdpSocket::Bytes payload, std::uint16_t component = 1, SendHandler done = {});
    void asyncSendDtls(UdpSocket::Bytes payload, std::uint16_t component = 1, SendHandler done = {});
    void asyncSendRtp(UdpSocket::Bytes payload, std::uint16_t component = 1, SendHandler done = {});
    void asyncSendRtcp(UdpSocket::Bytes payload, std::uint16_t component = 1, SendHandler done = {});

    // 停止旧代发送，取消待处理信令操作，清除 DTLS 参数/就绪通知；保留本地 Socket/host/srflx。
    void restart(IceCredentials credentials = {}, StunIceRole role = StunIceRole::Controlling, Completion done = {});
    void close(Completion done = {}); // 幂等；完成时所有已提交的发送完成回调和 agent 关闭均已完成

private:
    void send(UdpSocket::Bytes payload, std::uint16_t component, IcePacketType expected, SendHandler done);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace webrtc
