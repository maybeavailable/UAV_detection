#include"rtsp/rtp_transport.h"

#include"common/rtp_packet.h"
#include"log/log.h"

#include<chrono>
#include<ctime>
#include<sstream>

RtpTransport::RtpTransport(boost::asio::io_context& io, Config config, InterleavedWriter tcpWriter)
    : m_io_(io),
      m_config_(std::move(config)),
      m_tcpWriter_(std::move(tcpWriter)),
      m_udpRtp_(io),
      m_udpRtcp_(io),
      m_rtcpTimer_(io)
{
    if (m_config_.mode != Mode::UDP)
        return;                       // TCP 模式复用 RTSP 连接，无需 socket

    boost::system::error_code ec;

    m_udpRtp_.open(boost::asio::ip::udp::v4(), ec);
    if (!ec)
        m_udpRtp_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (!ec)
        m_udpRtp_.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), m_config_.serverRtp), ec);

    if (!ec)
        m_udpRtcp_.open(boost::asio::ip::udp::v4(), ec);
    if (!ec)
        m_udpRtcp_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (!ec)
        m_udpRtcp_.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), m_config_.serverRtcp), ec);

    if (ec)
    {
        UAV_LOG_ERROR << "RTP transport bind failed on " << m_config_.serverRtp << "/"
                      << m_config_.serverRtcp << ": " << ec.message();
        m_open_.store(false);
        return;
    }

    // connect() 后该 socket 只与这一个客户端通信：
    // 多个会话可同时 bind 5004/5005（reuse_address），按对端地址分流
    try
    {
        m_udpRtp_.connect(boost::asio::ip::udp::endpoint(
            boost::asio::ip::make_address(m_config_.clientAddress), m_config_.clientRtp), ec);
        if (!ec)
            m_udpRtcp_.connect(boost::asio::ip::udp::endpoint(
                boost::asio::ip::make_address(m_config_.clientAddress), m_config_.clientRtcp), ec);
    }
    catch (const std::exception& e)
    {
        UAV_LOG_ERROR << "RTP transport connect failed: " << e.what();
        ec = boost::asio::error::invalid_argument;
    }

    if (ec)
    {
        UAV_LOG_ERROR << "RTP transport connect failed: " << ec.message();
        m_open_.store(false);
        return;
    }

    startRtcpReceiveLoop();
}

RtpTransport::~RtpTransport()
{
    close();
}

std::string RtpTransport::transportEcho() const
{
    std::ostringstream oss;
    if (m_config_.mode == Mode::TCP)
    {
        oss << "RTP/AVP/TCP;unicast;interleaved="
            << m_config_.interleavedRtp << "-" << m_config_.interleavedRtcp;
    }
    else
    {
        oss << "RTP/AVP;unicast;client_port=" << m_config_.clientRtp << "-" << m_config_.clientRtcp
            << ";server_port=" << m_config_.serverRtp << "-" << m_config_.serverRtcp
            << ";ssrc=" << std::hex << m_config_.ssrc << std::dec;
    }
    return oss.str();
}

void RtpTransport::deliverRtp(const std::shared_ptr<const std::vector<std::uint8_t>>& packet)
{
    if (!m_open_.load(std::memory_order_relaxed) || !packet || packet->size() < 12)
        return;

    boost::asio::post(m_io_, [self = shared_from_this(), packet] {
        self->doDeliverRtp(packet);
    });
}

void RtpTransport::doDeliverRtp(const std::shared_ptr<const std::vector<std::uint8_t>>& packet)
{
    if (!m_open_.load(std::memory_order_relaxed))
        return;

    const RtpPacketParser rtp(packet->data(), packet->size());
    if (!rtp.valid())
        return;                           // 非法 RTP 包直接丢
    m_lastRtpTimestamp_ = rtp.timestamp();

    if (m_config_.mode == Mode::TCP)
    {
        if (m_tcpWriter_)
            m_tcpWriter_(m_config_.interleavedRtp, packet);
    }
    else
    {
        boost::system::error_code ec;
        m_udpRtp_.send(boost::asio::buffer(*packet), 0, ec);
        if (ec && ec != boost::asio::error::would_block &&
            ec != boost::asio::error::try_again)
        {
            UAV_LOG_DEBUG << "RTP send failed: " << ec.message() << " (dropping packet)";
        }
        // 实时流惯例：发送缓冲满/对端不可达时丢包，绝不阻塞编码线程
    }

    ++m_sentPackets_;
    m_sentOctets_ += packet->size();
    ensureRtcpTimer();
}

void RtpTransport::setPlaying(bool playing)
{
    m_playing_.store(playing, std::memory_order_relaxed);
}

void RtpTransport::close()
{
    if (!m_open_.exchange(false))
        return;

    boost::system::error_code ec;
    try
    {
        m_rtcpTimer_.cancel();            // Boost 1.88：cancel() 不接受 error_code，失败会抛
    }
    catch (const std::exception&)
    {
    }
    if (m_config_.mode == Mode::UDP)
    {
        m_udpRtp_.close(ec);
        m_udpRtcp_.close(ec);
    }
}

void RtpTransport::startRtcpReceiveLoop()
{
    // 注意用裸 this：本函数在构造函数里被调用，此时 shared_from_this() 会抛 bad_weak_ptr。
    // 安全性：出错（含关闭）时回调只检查 ec 就返回、不触碰任何成员；
    // 对象析构必然先 close() socket，挂起的 async_receive 会以 ec != 0 结束。
    m_udpRtcp_.async_receive(boost::asio::buffer(m_rtcpBuf_),
        [this](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec)
                return;               // 已关闭
            handleRtcp(reinterpret_cast<const std::uint8_t*>(m_rtcpBuf_.data()), bytes);
            startRtcpReceiveLoop();
        });
}

void RtpTransport::setFeedbackCallback(FeedbackCallback callback)
{
    m_feedbackCallback_ = std::move(callback);
}

bool RtpTransport::lastFeedback(RtcpFeedback& out) const
{
    if (!m_hasFeedback_.load(std::memory_order_relaxed))
        return false;
    out = m_lastFeedback_;
    return true;
}

// 解析客户端发回的 RTCP 复合包，提取针对我方 SSRC 的 RR 报告块
void RtpTransport::handleRtcp(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return;

    for (RtcpPacketParser parser(data, size); parser.next();)
    {
        if (parser.type() != RTCP_RR && parser.type() != RTCP_SR)
            continue;

        for (const auto& block : parser.reportBlocks())
        {
            if (block.ssrc != m_config_.ssrc)
                continue;             // 只关心反馈"我们这条流"的报告块

            const bool first = !m_hasFeedback_.load(std::memory_order_relaxed);

            RtcpFeedback feedback;
            feedback.ssrc           = block.ssrc;
            feedback.fractionLost   = block.fractionLost;
            feedback.cumulativeLost = block.cumulativeLost;
            feedback.highestSeq     = block.highestSeq;
            feedback.jitter         = block.jitter;
            feedback.rttMs          = rtcpRttMs(block.lsr, block.dlsr);

            m_lastFeedback_ = feedback;
            m_hasFeedback_.store(true, std::memory_order_relaxed);

            const std::string who = (m_config_.mode == Mode::TCP)
                                        ? std::string("tcp-interleaved")
                                        : (m_config_.clientAddress.empty()
                                               ? std::string("unknown")
                                               : m_config_.clientAddress);
            std::ostringstream oss;
            oss << "RTCP RR from " << who
                << ": loss=" << feedback.lossPercent() << "%"
                << " cumLost=" << feedback.cumulativeLost
                << " jitter=" << feedback.jitterMs() << "ms"
                << " rtt=" << feedback.rttMs << "ms";
            if (first)
                UAV_LOG_INFO << oss.str();
            else
                UAV_LOG_DEBUG << oss.str();

            if (m_feedbackCallback_)
                m_feedbackCallback_(feedback);
            return;                   // 一次取一个匹配块即可
        }
    }
}

void RtpTransport::ensureRtcpTimer()
{
    if (m_rtcpTimerActive_)
        return;
    m_rtcpTimerActive_ = true;
    scheduleRtcp();
}

void RtpTransport::scheduleRtcp()
{
    m_rtcpTimer_.expires_after(std::chrono::seconds(5));
    auto self = shared_from_this();
    m_rtcpTimer_.async_wait([self](const boost::system::error_code& ec) {
        if (ec)
        {
            self->m_rtcpTimerActive_ = false;
            return;
        }
        self->sendRtcpSr();
        self->scheduleRtcp();
    });
}

// RTCP Sender Report（RFC 3550）：每 5 秒一次，让客户端知道会话仍活跃
void RtpTransport::sendRtcpSr()
{
    if (!m_open_.load(std::memory_order_relaxed) ||
        !m_playing_.load(std::memory_order_relaxed) ||
        m_sentPackets_ == 0)
    {
        return;
    }

    auto sr = std::make_shared<std::vector<std::uint8_t>>(28);
    std::uint8_t* b = sr->data();

    b[0] = 0x80;      // V=2
    b[1] = 200;       // SR
    b[2] = 0;
    b[3] = 6;         // 长度 = 28/4 - 1

    const std::uint32_t ssrc = m_config_.ssrc;
    b[4]  = static_cast<std::uint8_t>(ssrc >> 24);
    b[5]  = static_cast<std::uint8_t>(ssrc >> 16);
    b[6]  = static_cast<std::uint8_t>(ssrc >> 8);
    b[7]  = static_cast<std::uint8_t>(ssrc);

    // NTP 时间（1900 纪元），简化：秒 + 0 小数
    const std::uint64_t ntpSeconds = static_cast<std::uint64_t>(std::time(nullptr)) + 2208988800ULL;
    b[8]  = static_cast<std::uint8_t>(ntpSeconds >> 24);
    b[9]  = static_cast<std::uint8_t>(ntpSeconds >> 16);
    b[10] = static_cast<std::uint8_t>(ntpSeconds >> 8);
    b[11] = static_cast<std::uint8_t>(ntpSeconds);
    b[12] = b[13] = b[14] = b[15] = 0;

    const std::uint32_t rtpTs = m_lastRtpTimestamp_;
    b[16] = static_cast<std::uint8_t>(rtpTs >> 24);
    b[17] = static_cast<std::uint8_t>(rtpTs >> 16);
    b[18] = static_cast<std::uint8_t>(rtpTs >> 8);
    b[19] = static_cast<std::uint8_t>(rtpTs);

    const std::uint64_t packets = m_sentPackets_;
    const std::uint64_t octets  = m_sentOctets_;
    for (int i = 0; i < 4; ++i)
    {
        b[20 + i] = static_cast<std::uint8_t>(packets >> (24 - 8 * i));
        b[24 + i] = static_cast<std::uint8_t>(octets >> (24 - 8 * i));
    }

    if (m_config_.mode == Mode::TCP)
    {
        if (m_tcpWriter_)
            m_tcpWriter_(m_config_.interleavedRtcp, sr);
    }
    else
    {
        boost::system::error_code ec;
        m_udpRtcp_.send(boost::asio::buffer(*sr), 0, ec);
    }
}
