#pragma once

#include"webrtc/ice_transport.h"
#include"webrtc/srtcp_ctl.h"
#include"webrtc/rtcp_session.h"
#include"webrtc/congestion_control/pacer.h"
#include"webrtc/congestion_control/gcc_transport_controller.h"

#include<chrono>
#include<cstddef>
#include<cstdint>
#include<functional>
#include<memory>
#include<optional>
#include<string>
#include<string_view>
#include<vector>

namespace webrtc
{
// 不可变的证书身份，可供同一 PeerConnection 的多个 DTLS 组件复用。
class DtlsCertificate
{
public:
    static std::shared_ptr<const DtlsCertificate> generate(unsigned validDays = 30); // ECDSA P-256 / SHA256
    static std::shared_ptr<const DtlsCertificate> fromPem(std::string_view certificatePem, std::string_view privateKeyPem);
    SdpFingerprint fingerprint(std::string_view algorithm = "sha-256") const;
    std::string certificatePem() const; // 不导出私钥；PEM/密钥错误抛 invalid_argument，密码学失败抛 runtime_error
private:
    struct Impl;
    explicit DtlsCertificate(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class DtlsTransport;
};

enum class DtlsState { New, Connecting, Connected, Failed, Closing, Closed };
struct DtlsStats
{
    std::uint64_t sentDatagrams{0}, receivedDatagrams{0}, discardedDatagrams{0}, retransmissions{0};
    std::uint64_t sentMessages{0}, sentBytes{0}, receivedMessages{0}, receivedBytes{0}; // 明文统计
    std::uint64_t discardedMediaPackets{0}; // 未就绪、旧代、错误来源或未通过认证的 SRTP/SRTCP
};

// 一个组件的一条 DTLS 1.2 association，可在本层管理 SRTP/SRTCP 控制器。
// SSL、BIO、媒体上下文、timer 与回调均由内部 strand 串行访问。
// 通过 Sender 和 handlePacket 连接 IceTransport；不替换 ICE 回调，不拥有或关闭 ICE Socket。
class DtlsTransport
{
public:
    using Bytes = UdpSocket::Bytes;
    using ErrorCode = UdpSocket::ErrorCode;
    using Completion = std::function<void(ErrorCode)>;
    using SendHandler = UdpSocket::SendHandler;
    // 取得完整 UDP 负载的所有权，按调用顺序发送，每次恰好调用一次完成回调；允许同步完成。
    // 启用内置 SRTP 时还输出 SRTP/SRTCP，转发到 ice.asyncSend(payload, component, done)。
    // 仅 DTLS 模式也可用 ice.asyncSendDtls；不能自行拆分/拼接报文。
    using Sender = std::function<void(Bytes, SendHandler)>;
    struct Config
    {
        std::shared_ptr<const DtlsCertificate> certificate; // 空时生成本实例身份
        std::uint16_t component{1};
        std::size_t mtu{1200}; // 最大输出 UDP 负载；输入仍允许最多 65507 字节
        std::size_t maxPlaintextSize{16384}; // 实际发送上限还受协商后的 DTLS data MTU 限制
        std::size_t maxPendingBytes{2 * 1024 * 1024}, maxPendingMessages{256};
        std::size_t maxBufferedDatagrams{128}, maxBufferedBytes{512 * 1024};
        std::chrono::milliseconds handshakeTimeout{15000};
        bool allowSha1Fingerprint{false}; // 默认 SHA512 > SHA384 > SHA256 > SHA224；不向较弱摘要降级
        std::vector<DtlsSrtpProfile> srtpProfiles{DtlsSrtpProfile::AeadAes128Gcm, DtlsSrtpProfile::Aes128CmSha1_80};
        bool requireSrtp{false}; // 纯 SCTP/DataChannel 可不协商 SRTP
        bool enableSrtp{false}; // 本层管理 SRTP/SRTCP；关闭时保留 exporter + 外部媒体加密模式
        SrtpCtl::Config srtp;
        SrtcpCtl::Config srtcp;
        RtcpSession::Config rtcp; // enableSrtp 时本层管理报告、反馈及重传；PT/反馈由 SDP 配置
        Pacer::Config pacer; // 内置媒体模式默认节奏发送 RTP/重传；DTLS/SCTP/RTCP 直接发送
        GccTransportConfig congestionControl; // 自动 GCC；手动控制 pacing 时可关闭 enabled
    };
    struct Snapshot
    {
        DtlsState state{DtlsState::New};
        std::uint64_t generation{0};
        std::uint16_t component{1};
        std::optional<SdpSetupRole> localSetup;
        SdpFingerprint localFingerprint;
        std::optional<SdpFingerprint> peerFingerprint; // 仅验证成功后设置
        std::optional<DtlsSrtpProfile> srtpProfile;
        std::optional<SrtpCtl::Snapshot> srtp;
        std::optional<SrtcpCtl::Snapshot> srtcp;
        std::optional<RtcpStats> rtcp;
        std::optional<Pacer::Snapshot> pacer;
        std::optional<GccTransportSnapshot> congestionControl;
        std::string cipher, protocol;
        std::size_t maxPlaintextSize{0}, pendingMessages{0}, pendingWireDatagrams{0};
        DtlsStats stats;
    };
    struct Callbacks
    {
        std::function<void(DtlsState)> onState;
        std::function<void(Snapshot)> onConnected; // 指纹匹配且 TLS Finished 验证完成
        std::function<void(Bytes)> onData; // 认证后解密的 application record；供 SCTP 等模块消费
        std::function<void(DtlsSrtpKeyMaterial)> onSrtpKeys; // 不含在 Snapshot 中；不得写入日志
        std::function<void(IceTransportPacket)> onRtpPacket, onRtcpPacket; // 内置 SRTP 解密认证后的完整明文
        std::function<void(RtcpReportEvent)> onReceiverReport;
        std::function<void(RtcpNackEvent)> onNack;
        std::function<void(RtcpKeyFrameEvent)> onKeyFrameRequest;
        std::function<void(TwccFeedbackEvent)> onTwccFeedback;
        std::function<void(ErrorCode, std::string)> onError;
        std::function<void(TransportPacketsFeedback)> onTransportFeedback;
        std::function<void(CongestionControlEvent)> onTargetTransferRate;
        std::function<void(RtcpRembEvent)> onRemoteBitrateReport;
    };

    DtlsTransport(boost::asio::io_context& io, Sender sender);
    DtlsTransport(boost::asio::io_context& io, Sender sender, Config config, Callbacks callbacks = {});
    ~DtlsTransport();
    DtlsTransport(const DtlsTransport&) = delete;
    DtlsTransport& operator=(const DtlsTransport&) = delete;
    Snapshot snapshot() const; // 可跨线程
    std::shared_ptr<const DtlsCertificate> certificate() const;

    // 从 IceTransport::onDtlsReady 转发。done 表示握手已启动，不表示 Connected。
    // 同代重复且参数相同为幂等；失败后不可重用同代，须 ICE restart；更高 generation 自动重置。
    void start(IceDtlsReady ready, Completion done = {});
    // 从 ICE onPacket 转发 DTLS/SRTP/SRTCP，保留源端点/组件/generation 校验；仅 DTLS 可提前缓冲。
    void handlePacket(IceTransportPacket packet);
    // 从 ICE onState 转发快照，在 ICE restart / Failed / Closed 时同步失效。
    void handleIceState(IceTransport::Snapshot state);
    void asyncSend(Bytes plaintext, SendHandler done = {}); // 一条明文消息一个 record，不做应用分片
    // 完整 RTP/RTCP 明文；RTP 经 pacer 出队后加密，RTCP 直接加密；完成字节数为原始明文长度。
    void asyncSendRtp(Bytes plaintext, SendHandler done = {});
    void asyncSendRtp(Bytes plaintext, std::uint64_t generation, SendHandler done = {});
    void asyncSendRtcp(Bytes plaintext, SendHandler done = {});
    void asyncSendRtcp(Bytes plaintext, std::uint64_t generation, SendHandler done = {});
    void configureRtcp(RtcpParameters parameters, Completion done = {});
    void requestPli(std::uint32_t mediaSsrc, std::uint64_t generation, Completion done = {}); // 完成回调包含实际发送错误
    void setPacingRate(std::uint64_t rateBps, Completion done = {}); // GCC 活动时由估计器管理；手动模式 0 暂停 RTP
    void setPacingRates(std::uint64_t rateBps, std::uint64_t paddingRateBps, Completion done = {});
    void setPacingPaused(bool paused, Completion done = {}); // 保留有界队列，排队超时仍生效
    void setBitrateConstraints(TargetRateConstraints constraints, Completion done = {});
    void setStreamsConfig(StreamsConfig streams, Completion done = {});
    void setNetworkStateEstimate(NetworkStateEstimate estimate, Completion done = {});
    // 可用于其他协议的 RFC 5705 exporter；nullopt 与空 context 在 TLS 1.2 中有不同语义。
    void exportKeyingMaterial(std::string label, std::size_t size, std::optional<Bytes> context,
                              std::function<void(ErrorCode, Bytes)> done);
    void reset(std::uint64_t generation, Completion done = {}); // 只能前进到新的 ICE generation
    void close(Completion done = {}); // 尽力发送 close_notify，等待已提交输出完成；不关闭 ICE

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace webrtc
