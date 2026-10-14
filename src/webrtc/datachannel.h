#pragma once

#include"common/udp_socket.h"
#include"webrtc/sdp.h"

#include<chrono>
#include<functional>
#include<memory>
#include<optional>
#include<string>

namespace webrtc
{
struct DataChannelConfig
{
    std::string label, protocol; // UTF-8，分别最多 65535 字节
    bool ordered{true}, negotiated{false}; // negotiated=true 时双方通过信令约定相同 id，不使用 DCEP
    std::optional<std::uint16_t> id;
    std::optional<std::uint16_t> maxRetransmits, maxPacketLifeTime; // 互斥；未设置时可靠传输，lifetime 单位 ms
    std::uint16_t priority{256};
    std::size_t bufferedAmountLowThreshold{0};
    bool validate(std::string* reason = nullptr) const;
};
struct DcepOpen
{
    DataChannelConfig channel;
    UdpSocket::Bytes encode() const; // 无效配置抛 invalid_argument；只编码 OPEN，不包含 SCTP SID/PPID
    static bool parse(const UdpSocket::Bytes& message, DcepOpen& out, std::string* reason = nullptr);
};
enum class DataChannelState { Connecting, Open, Closing, Closed };
struct DataChannelMessage { bool binary{false}; UdpSocket::Bytes data; }; // 完整消息；文本已经通过 UTF-8 校验
class DataChannelTransport;
class DataChannel
{
public:
    using Bytes = UdpSocket::Bytes;
    using ErrorCode = UdpSocket::ErrorCode;
    using Completion = std::function<void(ErrorCode)>;
    using SendHandler = UdpSocket::SendHandler;
    struct Snapshot
    {
        DataChannelState state{DataChannelState::Connecting};
        DataChannelConfig config; // 自动分配 SID 后更新 id
        std::size_t bufferedAmount{0}, maxMessageSize{0};
        std::uint64_t sentMessages{0}, sentBytes{0}, receivedMessages{0}, receivedBytes{0};
    };
    struct Callbacks
    {
        std::function<void()> onOpen, onClosed, onBufferedAmountLow;
        std::function<void(DataChannelMessage)> onMessage;
        std::function<void(ErrorCode, std::string)> onError;
    };
    ~DataChannel();
    DataChannel(const DataChannel&) = delete;
    DataChannel& operator=(const DataChannel&) = delete;
    Snapshot snapshot() const;
    // 回调在 SCTP transport strand 调用。后安装回调可取得当前 Open/Closed 状态和有界暂存消息。
    void setCallbacks(Callbacks callbacks);
    // 完成表示整个消息已被 SCTP 接受，不表示已到达对端。Connecting/Closing/Closed 时返回错误。
    void sendText(std::string text, SendHandler done = {});
    void sendBinary(Bytes message, SendHandler done = {});
    void setBufferedAmountLowThreshold(std::size_t threshold, Completion done = {});
    void close(Completion done = {}); // 发起双向 SCTP stream reset；onClosed 表示完成，超时也会结束
private:
    struct Impl;
    explicit DataChannel(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class DataChannelTransport;
};
enum class DataChannelTransportState { New, Connecting, Connected, Failed, Closed };
// SCTP over DTLS + DCEP。Sender 接收完整 SCTP 报文，应转发到 DtlsTransport::asyncSend。
// 不自行打开 UDP Socket。API 可跨线程；回调串行且不得抛异常，io_context 由调用者持续运行。
class DataChannelTransport
{
public:
    using Bytes = UdpSocket::Bytes;
    using ErrorCode = UdpSocket::ErrorCode;
    using Completion = std::function<void(ErrorCode)>;
    using Sender = std::function<void(Bytes, UdpSocket::SendHandler)>;
    using CreateHandler = std::function<void(ErrorCode, std::shared_ptr<DataChannel>)>;
    struct Config
    {
        std::uint16_t maxStreams{1024}; // 1..65535，最终受对端 SCTP INIT 协商结果限制
        std::size_t maxChannels{128}, maxMessageSize{65536};
        std::size_t maxBufferedSendBytes{2 * 1024 * 1024}, maxBufferedSendMessages{256};
        std::size_t maxBufferedReceiveBytes{4 * 1024 * 1024}, maxBufferedReceiveMessages{256};
        std::size_t maxPendingWireBytes{1024 * 1024}, maxPendingWirePackets{512};
        std::size_t socketBufferSize{4 * 1024 * 1024};
        std::chrono::milliseconds connectTimeout{15000}, channelOpenTimeout{15000}, channelCloseTimeout{5000};
        bool validate(std::string* reason = nullptr) const;
    };
    struct Parameters
    {
        std::uint64_t generation{0};
        SdpSetupRole localSetup{SdpSetupRole::Active}; // Active 使用偶数 SID，Passive 使用奇数 SID
        std::uint16_t localPort{5000}, remotePort{5000};
        std::uint64_t remoteMaxMessageSize{65536}; // SDP 未声明时 65536；0 表示对端不声明上限
        std::size_t localMaxMessageSize{65536}, maxPacketSize{1100}; // maxPacketSize <= DTLS maxPlaintextSize
        std::string remoteIdentity; // 已验证的对端证书指纹；用于判断换代时能否继续 SCTP association
    };
    struct Snapshot
    {
        DataChannelTransportState state{DataChannelTransportState::New};
        std::uint64_t generation{0};
        bool paused{false};
        std::uint16_t inboundStreams{0}, outboundStreams{0};
        std::size_t channels{0}, maxMessageSize{0}, pendingSendBytes{0}, pendingWirePackets{0};
    };
    struct Callbacks
    {
        std::function<void(DataChannelTransportState)> onState;
        std::function<void(std::shared_ptr<DataChannel>)> onDataChannel; // 远端 DCEP OPEN 创建的通道
        std::function<void(ErrorCode, std::string)> onError;
    };
    DataChannelTransport(boost::asio::io_context& io, Sender sender);
    DataChannelTransport(boost::asio::io_context& io, Sender sender, Config config, Callbacks callbacks = {});
    ~DataChannelTransport();
    DataChannelTransport(const DataChannelTransport&) = delete;
    DataChannelTransport& operator=(const DataChannelTransport&) = delete;
    Snapshot snapshot() const;
    void createDataChannel(DataChannelConfig config, CreateHandler done);
    void createDataChannel(DataChannelConfig config, DataChannel::Callbacks callbacks, CreateHandler done);
    void start(Parameters parameters, Completion done = {}); // DTLS Connected 后调用；done 不表示 SCTP 已 Connected
    void handlePacket(Bytes decryptedSctpPacket, std::uint64_t generation);
    void pause(std::uint64_t generation); // ICE/DTLS 暂不可用；保留 SCTP 状态，有界缓存发送
    void close(Completion done = {}); // 取消发送、关闭所有通道，等待已交给 Sender 的回调；幂等
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class DataChannel;
};
} // namespace webrtc
