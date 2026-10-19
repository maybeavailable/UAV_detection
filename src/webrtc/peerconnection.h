#pragma once

#include"webrtc/dtlstransport.h"
#include"webrtc/datachannel.h"

#include<functional>
#include<memory>
#include<optional>
#include<string>
#include<string_view>
#include<vector>

namespace webrtc
{
enum class PeerSignalingState { Stable, HaveLocalOffer, HaveRemoteOffer, Closed };
enum class PeerConnectionState { New, Connecting, Connected, Disconnected, Failed, Closed };
struct PeerSessionDescription
{
    SdpDescriptionType type{SdpDescriptionType::Offer};
    SdpSession session;
    std::string toSdp() const;
    static bool parse(SdpDescriptionType type, std::string_view text, PeerSessionDescription& out, std::string* reason = nullptr);
};
struct PeerMedia
{
    std::string mid; // 空值自动分配；同一 m-line 的 MID 在后续协商中保持不变
    std::string kind{"video"}; // audio / video / application
    SdpDirection direction{SdpDirection::SendRecv};
    std::vector<SdpCodec> codecs; // RTP 能力和 offer 的 PT；answer 使用远端 offer 的 PT
    std::vector<SdpRtcpFeedback> feedback;
    std::vector<SdpExtMap> extmaps;
    std::uint16_t sctpPort{5000};
    std::uint64_t maxMessageSize{65536};
    static PeerMedia audio(std::string mid, std::vector<SdpCodec> codecs, SdpDirection direction = SdpDirection::SendRecv);
    static PeerMedia video(std::string mid, std::vector<SdpCodec> codecs, SdpDirection direction = SdpDirection::SendRecv);
    static PeerMedia data(std::string mid = "data", std::uint16_t sctpPort = 5000, std::uint64_t maxMessageSize = 65536);
};
struct PeerIceCandidate
{
    std::string candidate; // candidate:...；空串为 end-of-candidates
    std::optional<std::string> sdpMid;
    std::optional<std::size_t> sdpMLineIndex;
    std::string usernameFragment; // 建议始终提供，隔离 ICE restart 前后的候选
    std::optional<UdpSocket::Endpoint> resolvedEndpoint; // 外部 mDNS 解析结果
};
struct PeerNegotiatedMedia
{
    std::string mid, kind;
    bool rejected{false};
    SdpDirection localDirection{SdpDirection::Inactive};
    std::vector<SdpCodec> codecs;
    std::vector<SdpRtcpFeedback> feedback;
    std::vector<SdpExtMap> extmaps; // negotiated IDs; directions expressed from the local side
    std::optional<std::uint16_t> localSctpPort, remoteSctpPort;
    std::optional<std::uint64_t> remoteMaxMessageSize;
};

// C++ 协商/传输层：单个 max-bundle UDP ICE transport + RTCP mux + DTLS。
// 管理 SDP、trickle ICE、证书及连接生命周期；可启用 SCTP/DCEP DataChannel，媒体编解码由上层提供。
// public API 可跨线程，完成/事件回调在内部 strand 串行调用且不得抛异常。
class PeerConnection
{
public:
    using Bytes = UdpSocket::Bytes;
    using ErrorCode = UdpSocket::ErrorCode;
    using Completion = IceTransport::Completion;
    using SendHandler = UdpSocket::SendHandler;
    using DescriptionHandler = std::function<void(ErrorCode, std::optional<PeerSessionDescription>)>;
    struct Config
    {
        IceTransport::Config ice;
        DtlsTransport::Config dtls; // DTLS、SRTP/SRTCP 及 RTCP 报告/反馈配置
        DataChannelTransport::Config dataChannel;
        bool enableDataChannels{false}; // 自动接入 SCTP；createDataChannel 也会启用，默认保留 raw DTLS 模式
        std::vector<PeerMedia> media; // 显式声明能力；enableDataChannels/createDataChannel 自动补 application
        std::size_t maxPendingOperations{128}, maxPendingCandidates{256};
    };
    struct OfferOptions { bool iceRestart{false}; };
    struct Snapshot
    {
        PeerSignalingState signalingState{PeerSignalingState::Stable};
        PeerConnectionState connectionState{PeerConnectionState::New};
        IceCredentials localCredentials;
        SdpFingerprint localFingerprint;
        std::optional<PeerSessionDescription> currentLocalDescription, currentRemoteDescription;
        std::optional<PeerSessionDescription> pendingLocalDescription, pendingRemoteDescription;
        std::vector<PeerNegotiatedMedia> negotiatedMedia;
        std::optional<IceTransport::Snapshot> ice;
        std::optional<DtlsTransport::Snapshot> dtls;
        std::optional<DataChannelTransport::Snapshot> dataChannels;
        std::optional<bool> canTrickleIceCandidates;
    };
    struct Callbacks
    {
        std::function<void(PeerSignalingState)> onSignalingState;
        std::function<void(PeerConnectionState)> onConnectionState;
        std::function<void(IceGatheringState)> onGatheringState;
        std::function<void(PeerIceCandidate)> onIceCandidate; // 已附 MID/index/ufrag，空 candidate 为结束标记
        std::function<void(Bytes)> onDtlsData; // 未启用内置 DataChannel 时，供外部 SCTP transport 使用
        std::function<void(std::shared_ptr<DataChannel>)> onDataChannel; // 远端通过 DCEP 打开的通道
        std::function<void(DtlsSrtpKeyMaterial)> onSrtpKeys;
        std::function<void(IceTransportPacket)> onSecurePacket; // dtls.enableSrtp=false 时交付 SRTP/SRTCP 密文
        std::function<void(IceTransportPacket)> onRtpPacket, onRtcpPacket; // 下层 transport 认证解密后的明文
        std::function<void(RtcpReportEvent)> onReceiverReport;
        std::function<void(RtcpNackEvent)> onNack;
        std::function<void(RtcpKeyFrameEvent)> onKeyFrameRequest;
        std::function<void(TwccFeedbackEvent)> onTwccFeedback;
        std::function<void(ErrorCode, std::string)> onError;
        std::function<void(TransportPacketsFeedback)> onTransportFeedback;
    };

    explicit PeerConnection(boost::asio::io_context& io);
    PeerConnection(boost::asio::io_context& io, Config config, Callbacks callbacks = {});
    ~PeerConnection();
    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;
    Snapshot snapshot() const;

    void addMedia(PeerMedia media, Completion done = {}); // Stable 时可添加能力/新的 m-line
    void setDirection(std::string mid, SdpDirection direction, Completion done = {});
    void createOffer(DescriptionHandler done);
    void createOffer(OfferOptions options, DescriptionHandler done); // 只生成描述，不改变 signaling state
    void createAnswer(DescriptionHandler done); // 必须 HaveRemoteOffer
    void setLocalDescription(PeerSessionDescription description, Completion done = {});
    void setRemoteDescription(PeerSessionDescription description, Completion done = {});
    void setRemoteDescription(SdpDescriptionType type, std::string sdp, Completion done = {});
    void addIceCandidate(PeerIceCandidate candidate, Completion done = {}); // 描述到达前有界暂存
    void rollback(Completion done = {}); // 撤销 pending offer；ICE restart 在最终 answer 应用后执行

    void createDataChannel(DataChannelConfig config, DataChannelTransport::CreateHandler done);
    void createDataChannel(DataChannelConfig config, DataChannel::Callbacks callbacks, DataChannelTransport::CreateHandler done);
    void asyncSendDtls(Bytes plaintext, SendHandler done = {}); // SCTP 编码后的 DTLS 明文，需 application m-line
    void asyncSendSecurePacket(Bytes encryptedPacket, SendHandler done = {}); // 已加密 SRTP/SRTCP，需 RTP m-line
    void asyncSendRtp(Bytes plaintext, SendHandler done = {}); // 转发下层 transport 加密；需 dtls.enableSrtp
    void asyncSendRtcp(Bytes plaintext, SendHandler done = {});
    void requestPli(std::uint32_t mediaSsrc, Completion done = {}); // 转发到下层；需协商 nack pli
    void setPacingRate(std::uint64_t rateBps, Completion done = {}); // 转发到下层 pacer；0 暂停 RTP
    void setPacingRates(std::uint64_t rateBps, std::uint64_t paddingRateBps, Completion done = {});
    void setPacingPaused(bool paused, Completion done = {});
    void close(Completion done = {}); // 幂等；关闭 DataChannel、DTLS（含 SRTP/SRTCP）、ICE 并等待发送回调

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace webrtc
