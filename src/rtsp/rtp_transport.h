#pragma once

#include"common/common.h"
#include"common/rtcp_packet.h"

#include<array>
#include<atomic>
#include<cstdint>
#include<functional>
#include<memory>
#include<vector>

// 一个拉流客户端的一路传输管道：
//   SETUP 时创建（协商 UDP 端口对 / TCP interleaved 通道号），
//   PLAY 后被订阅到 RtpSource，源把打好的 RTP 包直接转发到这里；
//   本类负责"投递"（UDP sendto / 往 RTSP 连接写 $ 帧）、
//   周期性 RTCP SR（Sender Report）保活，以及
//   解析客户端发回的 RTCP RR 反馈（丢包率/累计丢包/抖动/RTT）。
//
// 线程约定：deliverRtp 可被源线程调用，内部 post 回 io_context 线程做实际 I/O；
// handleRtcp 只在 io_context 线程被调用（UDP 收包回调 / session 解析到 $ 帧）。
class RtpTransport : public std::enable_shared_from_this<RtpTransport>
{
public:
    enum class Mode { UDP, TCP };

    struct Config
    {
        Mode         mode{Mode::UDP};
        unsigned     clientRtp{0};                      // UDP：客户端收 RTP 的端口
        unsigned     clientRtcp{0};                     // UDP：客户端收 RTCP 的端口
        unsigned     serverRtp{RTSP_RTP_PORT};          // UDP：服务端收发 RTP 的端口
        unsigned     serverRtcp{RTSP_RTCP_PORT};        // UDP：服务端收发 RTCP 的端口
        int          interleavedRtp{0};                 // TCP：RTP 通道号（偶数）
        int          interleavedRtcp{1};                // TCP：RTCP 通道号（奇数）
        std::uint32_t ssrc{0};                          // 用源的 ssrc，SETUP 回显与包内一致
        std::string  clientAddress;                     // UDP：RTP 发送目的地（RTSP 对端 IP）
    };

    // TCP 模式投递回调：session 提供，把 $ 帧写进 RTSP 连接的写队列（仅在 io 线程调用）
    using InterleavedWriter = std::function<void(
        int channel, const std::shared_ptr<const std::vector<std::uint8_t>>& data)>;

    // 收到客户端 RR 反馈时回调（在 io 线程调用）；encode 模块可据此做码率自适应
    using FeedbackCallback = RtcpFeedbackCallback;

    RtpTransport(boost::asio::io_context& io, Config config, InterleavedWriter tcpWriter);
    ~RtpTransport();

    RtpTransport(const RtpTransport&) = delete;
    RtpTransport& operator=(const RtpTransport&) = delete;

    const Config& config() const { return m_config_; }
    std::string   transportEcho() const;          // SETUP 响应的 Transport 头

    bool isOpen() const { return m_open_.load(std::memory_order_relaxed); }

    // 源线程调用：把 RTP 包投递到客户端
    void deliverRtp(const std::shared_ptr<const std::vector<std::uint8_t>>& packet);

    void setPlaying(bool playing);                // PLAY/PAUSE 控制 RTCP SR

    // ---- RR 反馈 ----
    void setFeedbackCallback(FeedbackCallback callback);
    // 取最近一次反馈；从未收到过返回 false
    bool lastFeedback(RtcpFeedback& out) const;
    // 解析一段 RTCP 复合包（UDP 收包 / TCP interleaved 帧，仅在 io 线程调用）
    void handleRtcp(const std::uint8_t* data, std::size_t size);

    void close();

private:
    void doDeliverRtp(const std::shared_ptr<const std::vector<std::uint8_t>>& packet);
    void startRtcpReceiveLoop();                  // UDP：收客户端 RTCP 并解析 RR
    void ensureRtcpTimer();
    void scheduleRtcp();
    void sendRtcpSr();

    boost::asio::io_context& m_io_;
    Config m_config_;
    InterleavedWriter m_tcpWriter_;

    boost::asio::ip::udp::socket m_udpRtp_;
    boost::asio::ip::udp::socket m_udpRtcp_;
    std::array<char, 2048> m_rtcpBuf_{};

    boost::asio::steady_timer m_rtcpTimer_;
    bool m_rtcpTimerActive_{false};

    std::atomic<bool> m_open_{true};
    std::atomic<bool> m_playing_{false};
    std::uint64_t m_sentPackets_{0};
    std::uint64_t m_sentOctets_{0};
    std::uint32_t m_lastRtpTimestamp_{0};

    FeedbackCallback m_feedbackCallback_;
    RtcpFeedback     m_lastFeedback_;
    std::atomic<bool> m_hasFeedback_{false};
};
