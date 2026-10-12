#include"rtsp/rtp_transport.h"

#include"common/rtp_packet.h"
#include"log/log.h"

#include<chrono>
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

    m_config_.serverRtp = m_udpRtp_.local_endpoint().port();
    m_config_.serverRtcp = m_udpRtcp_.local_endpoint().port();
    m_udpRtp_.non_blocking(true, ec);
    m_udpRtcp_.non_blocking(true, ec);
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

void RtpTransport::start()
{
    if (m_started_ || !isOpen()) return;
    m_started_ = true;
    if (m_config_.mode == Mode::UDP) startRtcpReceiveLoop();
}

void RtpTransport::setFailureCallback(std::function<void()> callback)
{
    m_onFailure_ = std::move(callback);
}

std::size_t RtpTransport::pendingBytes() const
{
    std::lock_guard<std::mutex> lock(m_pendingMutex_);
    return m_pendingBytes_;
}

void RtpTransport::deliverRtp(const std::shared_ptr<const std::vector<std::uint8_t>>& packet)
{
    if (!packet || packet->size() < 12) return;
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex_);
        if (!isOpen() || !m_playing_.load() || m_overflow_) return;
        if (packet->size() > m_config_.maxPendingBytes ||
            m_pendingBytes_ > m_config_.maxPendingBytes - packet->size() ||
            m_pending_.size() >= m_config_.maxPendingPackets)
        {
            m_overflow_ = true;
            m_pending_.clear();
            m_pendingBytes_ = 0;
        }
        else
        {
            m_pending_.push_back(packet);
            m_pendingBytes_ += packet->size();
        }
        if (!m_drainScheduled_)
        {
            m_drainScheduled_ = true;
            schedule = true;
        }
    }
    // 每个会话至多一个 drain 在途，不能为每个 RTP 包建立无界 post 队列。
    if (schedule)
        boost::asio::post(m_io_, [self = shared_from_this()] { self->drainRtp(); });
}

void RtpTransport::drainRtp()
{
    for (unsigned i = 0; i < 64; ++i)
    {
        std::shared_ptr<const std::vector<std::uint8_t>> packet;
        bool overflow;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex_);
            overflow = m_overflow_;
            if (!isOpen() || !m_playing_.load() || overflow || m_pending_.empty())
            {
                m_pending_.clear();
                m_pendingBytes_ = 0;
                m_drainScheduled_ = false;
            }
            else
            {
                packet = std::move(m_pending_.front());
                m_pending_.pop_front();
                m_pendingBytes_ -= packet->size();
            }
        }
        if (overflow) { fail(); return; }
        if (!packet) return;
        doDeliverRtp(packet);
    }
    // 分批发送，给 RTSP 控制命令和其他客户端留出调度机会。
    boost::asio::post(m_io_, [self = shared_from_this()] { self->drainRtp(); });
}

void RtpTransport::fail()
{
    close();
    if (m_onFailure_) m_onFailure_();
}

void RtpTransport::doDeliverRtp(const std::shared_ptr<const std::vector<std::uint8_t>>& packet)
{
    if (!isOpen() || !m_playing_.load()) return;
    const RtpPacketParser rtp(packet->data(), packet->size());
    if (!rtp.valid()) return;
    if (m_config_.mode == Mode::TCP)
    {
        if (!m_tcpWriter_) return;
        m_tcpWriter_(m_config_.interleavedRtp, packet);
        if (!isOpen()) return;  // 写队列溢出可同步关闭整个会话
    }
    else
    {
        boost::system::error_code ec;
        const auto sent = m_udpRtp_.send(boost::asio::buffer(*packet), 0, ec);
        if (ec || sent != packet->size()) return;
    }
    m_lastRtpTimestamp_ = rtp.timestamp();
    m_lastRtpTime_ = std::chrono::steady_clock::now();
    ++m_sentPackets_;
    m_sentOctets_ += rtp.payloadSize();
    ensureRtcpTimer();
}

void RtpTransport::setPlaying(bool playing)
{
    m_playing_.store(playing);
    if (playing) start();
    else
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex_);
        m_pending_.clear();
        m_pendingBytes_ = 0;
    }
}

void RtpTransport::close()
{
    m_open_.store(false);
    m_playing_.store(false);
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex_);
        m_pending_.clear();
        m_pendingBytes_ = 0;
    }
    try { m_rtcpTimer_.cancel(); } catch (const std::exception&) {}
    boost::system::error_code ec;
    m_udpRtp_.close(ec);
    m_udpRtcp_.close(ec);
}

void RtpTransport::startRtcpReceiveLoop()
{
    if (!isOpen()) return;
    // 缓冲独立存活到回调结束；weak_ptr 避免接收循环永久持有传输对象。
    auto buffer = std::make_shared<std::array<std::uint8_t, 2048>>();
    m_udpRtcp_.async_receive(boost::asio::buffer(*buffer),
        [weak = weak_from_this(), buffer](const boost::system::error_code& ec, std::size_t bytes) {
            const auto self = weak.lock();
            if (!self || !self->isOpen() || ec) return;
            self->handleRtcp(buffer->data(), bytes);
            if (self->isOpen()) self->startRtcpReceiveLoop();
        });
}

void RtpTransport::setFeedbackCallback(FeedbackCallback callback)
{
    m_feedbackCallback_ = std::move(callback);
}

bool RtpTransport::lastFeedback(RtcpFeedback& out) const
{
    std::lock_guard<std::mutex> lock(m_feedbackMutex_);
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

            {
                std::lock_guard<std::mutex> lock(m_feedbackMutex_);
                m_lastFeedback_ = feedback;
                m_hasFeedback_.store(true, std::memory_order_relaxed);
            }

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
    if (!isOpen()) { m_rtcpTimerActive_ = false; return; }
    m_rtcpTimer_.expires_after(m_config_.rtcpInterval);
    m_rtcpTimer_.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
        const auto self = weak.lock();
        if (!self) return;
        if (ec || !self->isOpen()) { self->m_rtcpTimerActive_ = false; return; }
        self->sendRtcpSr();
        self->scheduleRtcp();
    });
}

void RtpTransport::sendRtcpSr()
{
    if (!isOpen() || !m_playing_.load() || m_sentPackets_ == 0) return;
    const auto steadyNow = std::chrono::steady_clock::now();
    const auto ntp = rtcpNtpNow();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(steadyNow - m_lastRtpTime_).count();
    const auto rtpTs = m_lastRtpTimestamp_ + static_cast<std::uint32_t>(
        (elapsed / 1000000) * m_config_.clockRate + (elapsed % 1000000) * m_config_.clockRate / 1000000);
    auto sr = std::make_shared<std::vector<std::uint8_t>>(28, 0);
    auto write32 = [&](std::size_t offset, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) (*sr)[offset + i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
    };
    (*sr)[0] = 0x80; (*sr)[1] = RTCP_SR; (*sr)[3] = 6;
    write32(4, m_config_.ssrc);
    write32(8, static_cast<std::uint32_t>(ntp >> 32));
    write32(12, static_cast<std::uint32_t>(ntp));
    write32(16, rtpTs);
    write32(20, static_cast<std::uint32_t>(m_sentPackets_));
    write32(24, static_cast<std::uint32_t>(m_sentOctets_));

    std::ostringstream name;
    name << "uav-" << std::hex << m_config_.ssrc;
    const std::string cname = name.str();
    const std::size_t sdesSize = (4 + 4 + 2 + cname.size() + 1 + 3) & ~std::size_t(3);
    sr->resize(28 + sdesSize, 0);
    (*sr)[28] = 0x81; (*sr)[29] = RTCP_SDES;
    const auto words = sdesSize / 4 - 1;
    (*sr)[30] = static_cast<std::uint8_t>(words >> 8);
    (*sr)[31] = static_cast<std::uint8_t>(words);
    write32(32, m_config_.ssrc);
    (*sr)[36] = 1; (*sr)[37] = static_cast<std::uint8_t>(cname.size());
    std::copy(cname.begin(), cname.end(), sr->begin() + 38);
    if (m_config_.mode == Mode::TCP)
    {
        if (m_tcpWriter_) m_tcpWriter_(m_config_.interleavedRtcp, sr);
    }
    else
    {
        boost::system::error_code ec;
        m_udpRtcp_.send(boost::asio::buffer(*sr), 0, ec);
    }
}
