#include "rtspserver.h"

#include"log/log.h"
#include"rtsp/rtp_source.h"
#include"rtsp/rtp_transport.h"

#include<algorithm>
#include<atomic>
#include<cctype>
#include<cstdio>
#include<cstdlib>
#include<cstring>
#include<sstream>
#include<utility>

namespace
{

std::atomic<std::uint64_t> s_sessionCounter{1};

constexpr std::size_t kInitialBufferSize = 4096;
constexpr std::size_t kMaxRequestSize    = 256 * 1024;   // 请求头 + 请求体上限
constexpr std::size_t kMaxHeaderSize     = 32 * 1024;    // 找不到 \r\n\r\n 时的头部上限
constexpr std::size_t kMaxBodySize       = 64 * 1024;    // Content-Length 上限

std::string toLower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s)
{
    const char* whitespace = " \t\r\n";
    const auto first = s.find_first_not_of(whitespace);
    if (first == std::string::npos)
        return std::string();
    const auto last = s.find_last_not_of(whitespace);
    return s.substr(first, last - first + 1);
}

std::vector<std::string> splitWs(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token)
        out.push_back(token);
    return out;
}

// 从 URI 里取路径部分：rtsp://host:port/live/uav -> /live/uav
std::string uriPath(const std::string& uri)
{
    std::string path = uri;
    if (path == "*")
        return std::string();

    const auto scheme = path.find("://");
    if (scheme != std::string::npos)
    {
        path = path.substr(scheme + 3);
        const auto slash = path.find('/');
        if (slash == std::string::npos)
            return std::string();
        path = path.substr(slash);
    }

    const auto query = path.find('?');
    if (query != std::string::npos)
        path.resize(query);

    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

std::string pathTail(const std::string& path)
{
    const auto pos = path.rfind('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// 解析 SETUP 的 Transport 头：RTP/AVP;unicast;client_port=x-y 或
// RTP/AVP/TCP;unicast;interleaved=i-j。失败返回 false。
bool parseTransportHeader(const std::string& transport, RtpTransport::Config& config)
{
    const std::string t = toLower(transport);

    if (t.find("rtp/avp/tcp") != std::string::npos)
    {
        config.mode = RtpTransport::Mode::TCP;
        const std::size_t p = t.find("interleaved=");
        if (p == std::string::npos)
            return false;
        int rtp = -1, rtcp = -1;
        if (std::sscanf(t.c_str() + p, "interleaved=%d-%d", &rtp, &rtcp) != 2 ||
            rtp < 0 || rtcp < rtp)
        {
            return false;
        }
        config.interleavedRtp  = rtp;
        config.interleavedRtcp = rtcp;
        return true;
    }

    if (t.find("rtp/avp") != std::string::npos)
    {
        if (t.find("multicast") != std::string::npos)
            return false;                 // 暂不支持组播

        config.mode = RtpTransport::Mode::UDP;
        const std::size_t p = t.find("client_port=");
        if (p == std::string::npos)
            return false;

        unsigned rtp = 0, rtcp = 0;
        const int matched = std::sscanf(t.c_str() + p, "client_port=%u-%u", &rtp, &rtcp);
        if (matched == 2)
        {
            // OK
        }
        else if (matched == 1)
        {
            rtcp = rtp + 1;               // 容忍只给单端口的形式
        }
        else
        {
            return false;
        }

        config.clientRtp  = rtp;
        config.clientRtcp = rtcp;
        return true;
    }

    return false;
}

}  // namespace

// =============================== RtspServer ===============================

RtspServer::RtspServer(boost::asio::io_context& ioContext)
    : m_ioContext_(ioContext),
      m_acceptor_(m_ioContext_,
                  boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), RTSP_PORT))
{
    start();
}

RtspServer::~RtspServer()
{
    boost::system::error_code ec;
    m_acceptor_.close(ec);
    for (auto& session : m_sessions_)
    {
        session->setCloseCallback(nullptr);   // 防止回调再触碰已析构的集合
        session->close();
    }
    m_sessions_.clear();
}

void RtspServer::start()
{
    do_accept();
}

void RtspServer::registerSource(const std::shared_ptr<RtpSource>& source)
{
    if (!source || source->path().empty())
        return;
    m_sources_[source->path()] = source;
    UAV_LOG_INFO << "RTSP source registered: " << source->path();
}

std::shared_ptr<RtpSource> RtspServer::findSource(const std::string& path) const
{
    const auto it = m_sources_.find(path);
    return it == m_sources_.end() ? nullptr : it->second;
}

void RtspServer::setFeedbackCallback(RtcpFeedbackCallback callback)
{
    m_feedbackCallback_ = std::move(callback);
    for (const auto& session : m_sessions_)
        session->setFeedbackCallback(m_feedbackCallback_);
}

void RtspServer::do_accept()
{
    m_acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket)
        {
            if (ec)
            {
                UAV_LOG_WARN << "RTSP accept failed: " << ec.message();
                return;
            }

            boost::system::error_code epEc;
            const auto remote = socket.remote_endpoint(epEc);
            UAV_LOG_INFO << "RTSP client connected: "
                         << (epEc ? std::string("unknown")
                                  : remote.address().to_string() + ":" +
                                        std::to_string(remote.port()));

            auto session = std::make_shared<RtspSession>(std::move(socket), m_ioContext_,
                [this](const std::string& path) { return findSource(path); });
            m_sessions_.insert(session);

            // 只存 weak_ptr：避免"会话持有回调、回调持有会话"的引用环
            std::weak_ptr<RtspSession> weak = session;
            session->setCloseCallback([this, weak] {
                if (const auto s = weak.lock())
                    m_sessions_.erase(s);
            });
            session->setFeedbackCallback(m_feedbackCallback_);

            session->start();
            do_accept();   // 继续等待下一个客户端
        });
}

// =============================== RtspSession ===============================

RtspSession::RtspSession(boost::asio::ip::tcp::socket socket,
                         boost::asio::io_context& io,
                         std::function<std::shared_ptr<RtpSource>(const std::string&)> sourceLookup)
    : m_socket_(std::move(socket)),
      m_io_(io),
      m_sourceLookup_(std::move(sourceLookup)),
      m_recvBuf_(kInitialBufferSize)
{
}

void RtspSession::start()
{
    do_read();
}

void RtspSession::setCloseCallback(std::function<void()> callback)
{
    m_onClose_ = std::move(callback);
}

void RtspSession::setFeedbackCallback(RtcpFeedbackCallback callback)
{
    m_feedbackCallback_ = std::move(callback);
    for (const auto& kv : m_tracks_)
        kv.second->setFeedbackCallback(m_feedbackCallback_);
}

void RtspSession::close()
{
    if (m_closed_)
        return;
    m_closed_ = true;

    // 把各路传输从源上摘掉并关闭
    for (auto& kv : m_tracks_)
    {
        kv.second->setPlaying(false);
        kv.second->close();
    }
    m_tracks_.clear();

    boost::system::error_code ec;
    m_socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    m_socket_.close(ec);

    if (m_onClose_)
        m_onClose_();
}

// 读取 RTSP 信息：读进缓冲尾部，解析出完整请求后交给 handle_request 握手
void RtspSession::do_read()
{
    ensure_capacity();
    if (m_closed_)
        return;

    auto self = shared_from_this();
    m_socket_.async_read_some(
        boost::asio::buffer(m_recvBuf_.data() + m_bufOffset_,
                            m_recvBuf_.size() - m_bufOffset_),
        [self](const boost::system::error_code& ec, std::size_t bytes)
        {
            if (!ec)
                self->on_read(bytes);
            else
                self->on_read_error(ec);
        });
}

void RtspSession::on_read(std::size_t bytes)
{
    m_bufOffset_ += bytes;

    for (;;)
    {
        Request request;
        const int result = parse_request(request);
        if (result == 0)
            break;                        // 数据不够，等下一波
        if (result < 0)
        {
            UAV_LOG_WARN << "RTSP protocol error from client, closing";
            send_error_and_close(400, "Bad Request");
            return;
        }

        handle_request(request);
        if (m_closed_)
            return;
    }

    do_read();
}

void RtspSession::on_read_error(const boost::system::error_code& ec)
{
    if (ec == boost::asio::error::eof ||
        ec == boost::asio::error::connection_reset ||
        ec == boost::asio::error::operation_aborted)
    {
        UAV_LOG_DEBUG << "RTSP client disconnected: " << ec.message();
    }
    else
    {
        UAV_LOG_WARN << "RTSP read error: " << ec.message();
    }
    close();
}

void RtspSession::ensure_capacity()
{
    if (m_bufOffset_ < m_recvBuf_.size())
        return;                           // 还有空闲空间

    if (m_recvBuf_.size() >= kMaxRequestSize)
    {
        UAV_LOG_WARN << "RTSP request too large (" << m_recvBuf_.size() << " bytes), closing";
        send_error_and_close(400, "Bad Request");
        return;
    }
    m_recvBuf_.resize(std::min<std::size_t>(m_recvBuf_.size() * 2, kMaxRequestSize));
}

void RtspSession::consume(std::size_t bytes)
{
    if (bytes == 0)
        return;
    if (bytes >= m_bufOffset_)
    {
        m_bufOffset_ = 0;
        return;
    }
    std::memmove(m_recvBuf_.data(), m_recvBuf_.data() + bytes, m_bufOffset_ - bytes);
    m_bufOffset_ -= bytes;
}

// 从滑动缓冲里解析一条完整请求。
// 返回：1 = 完整请求（request 已填充并消费），0 = 数据不够，-1 = 协议错误
int RtspSession::parse_request(Request& request)
{
    const char* data = m_recvBuf_.data();
    std::size_t used = m_bufOffset_;

    // TCP interleaved 二进制帧：SETUP 协商过 RTP/AVP/TCP 后，
    // 客户端可能在同一连接上传送 $ 帧（RTP/RTCP），跳过不做解析
    if (used > 0 && data[0] == '$')
    {
        bool tcpInterleaved = false;
        for (const auto& kv : m_tracks_)
            tcpInterleaved = tcpInterleaved || kv.second->config().mode == RtpTransport::Mode::TCP;
        if (!tcpInterleaved)
            return -1;

        while (used > 0 && data[0] == '$')
        {
            if (used < 4)
                return 0;
            const std::size_t frameLen = (static_cast<unsigned char>(data[2]) << 8) |
                                         static_cast<unsigned char>(data[3]);
            if (frameLen > kMaxBodySize)
                return -1;
            if (used < 4 + frameLen)
                return 0;

            // 客户端发来的 RTCP（RR 反馈等）走 interleaved 的 RTCP 通道：
            // 转发给对应传输解析；RTP 通道帧暂不处理（本方向不接收媒体）
            const int channel = static_cast<unsigned char>(data[1]);
            for (const auto& kv : m_tracks_)
            {
                if (kv.second->config().mode == RtpTransport::Mode::TCP &&
                    channel == kv.second->config().interleavedRtcp)
                {
                    kv.second->handleRtcp(reinterpret_cast<const std::uint8_t*>(data + 4),
                                          frameLen);
                }
            }

            consume(4 + frameLen);
            data = m_recvBuf_.data();
            used = m_bufOffset_;
        }
    }

    // 找请求头结束标记 \r\n\r\n
    static constexpr char kHeaderEnd[] = "\r\n\r\n";
    std::size_t headerEnd = std::string::npos;
    for (std::size_t i = 0; i + 4 <= used; ++i)
    {
        if (std::memcmp(data + i, kHeaderEnd, 4) == 0)
        {
            headerEnd = i;
            break;
        }
    }
    if (headerEnd == std::string::npos)
    {
        if (used > kMaxHeaderSize)
            return -1;                    // 迟迟没有头结束标记：协议错误
        return 0;
    }
    const std::size_t bodyStart = headerEnd + 4;
    // 注意 +2：定界符 "\r\n\r\n" 的前一个 CRLF 属于最后一行头部的结尾，
    // 不带上它的话最后一行头（通常是 CSeq/Transport）会被整行丢掉
    const std::string headerBlock(data, headerEnd + 2);

    // 请求行：METHOD SP URI SP VERSION
    std::size_t lineStart = 0;
    while (lineStart + 1 < headerBlock.size() &&
           headerBlock[lineStart] == '\r' && headerBlock[lineStart + 1] == '\n')
    {
        lineStart += 2;                   // 容忍行首空行
    }
    const std::size_t lineEnd = headerBlock.find("\r\n", lineStart);
    if (lineEnd == std::string::npos)
        return -1;

    const std::vector<std::string> parts = splitWs(headerBlock.substr(lineStart, lineEnd - lineStart));
    if (parts.size() != 3)
        return -1;
    request.method  = toUpper(parts[0]);
    request.uri     = parts[1];
    request.version = parts[2];

    // 头部：Name: Value
    std::size_t pos = lineEnd + 2;
    while (pos < headerBlock.size())
    {
        const std::size_t eol = headerBlock.find("\r\n", pos);
        if (eol == std::string::npos)
            break;
        const std::string line = headerBlock.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty())
            continue;

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
            return -1;                    // 非法头行
        const std::string name  = toLower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (!name.empty())
            request.headers[name] = value;
    }

    // 请求体：按 Content-Length 判定完整性
    std::size_t contentLength = 0;
    const auto cl = request.headers.find("content-length");
    if (cl != request.headers.end())
    {
        char* end = nullptr;
        const long v = std::strtol(cl->second.c_str(), &end, 10);
        if (end == cl->second.c_str() || *end != '\0' || v < 0)
            return -1;
        contentLength = static_cast<std::size_t>(v);
    }
    if (contentLength > kMaxBodySize)
        return -1;
    if (used < bodyStart + contentLength)
        return 0;                         // 等 body 到齐

    if (contentLength > 0)
        request.body.assign(data + bodyStart, contentLength);

    consume(bodyStart + contentLength);
    return 1;
}

// =============================== 请求处理 ===============================

void RtspSession::handle_request(const Request& request)
{
    if (request.version != "RTSP/1.0")
        return send_response(505, "RTSP Version Not Supported", request);

    // RFC 2326：请求必须带 CSeq
    if (request.headers.find("cseq") == request.headers.end())
        return send_response(400, "Bad Request", request);

    if (request.method == "OPTIONS")      return handle_options(request);
    if (request.method == "DESCRIBE")     return handle_describe(request);
    if (request.method == "SETUP")        return handle_setup(request);
    if (request.method == "PLAY")         return handle_play(request);
    if (request.method == "PAUSE")        return handle_pause(request);
    if (request.method == "TEARDOWN")     return handle_teardown(request);
    if (request.method == "GET_PARAMETER" || request.method == "SET_PARAMETER")
        return send_response(200, "OK", request);

    return send_response(405, "Method Not Allowed", request,
                         "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n");
}

void RtspSession::handle_options(const Request& request)
{
    send_response(200, "OK", request,
                  "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n");
}

void RtspSession::handle_describe(const Request& request)
{
    m_path_ = uriPath(request.uri);
    if (m_path_.empty())
        m_path_ = "/";

    const auto source = m_sourceLookup_ ? m_sourceLookup_(m_path_) : nullptr;
    if (!source)
        return send_response(404, "Not Found", request);

    // SDP 由源提供：拉流地址对应的就是注册的源
    send_response(200, "OK", request,
                  "Content-Base: " + rtsp_base() + "\r\n",
                  source->makeSdp(local_ip(), RTSP_PORT), "application/sdp");
}

void RtspSession::handle_setup(const Request& request)
{
    const std::string path    = uriPath(request.uri);
    const std::string trackId = pathTail(path);
    if (trackId != "track1")   // 每路源目前通告一个 track
        return send_response(404, "Not Found", request);

    // 源地址 = URI 去掉 /track1
    std::size_t slash = path.rfind('/');
    const std::string base = (slash == std::string::npos)
                                 ? std::string("/")
                                 : (slash == 0 ? std::string("/") : path.substr(0, slash));
    if (m_path_.empty())
        m_path_ = base;

    const auto source = m_sourceLookup_ ? m_sourceLookup_(m_path_) : nullptr;
    if (!source)
        return send_response(404, "Not Found", request);

    RtpTransport::Config config;
    if (!parseTransportHeader(header(request, "transport"), config))
        return send_response(461, "Unsupported Transport", request);

    // UDP 模式：RTP 发往 RTSP 连接对端的 IP
    boost::system::error_code epEc;
    const auto remote = m_socket_.remote_endpoint(epEc);
    config.clientAddress = epEc ? std::string() : remote.address().to_string();
    config.ssrc = source->ssrc();         // 回显的 ssrc 与 RTP 包内一致

    std::weak_ptr<RtspSession> weak = weak_from_this();
    auto transport = std::make_shared<RtpTransport>(m_io_, config,
        [weak](int channel, const std::shared_ptr<const std::vector<std::uint8_t>>& data) {
            if (const auto self = weak.lock())
                self->writeInterleaved(channel, data);
        });
    if (!transport->isOpen())
        return send_response(500, "Internal Server Error", request);

    transport->setFeedbackCallback(m_feedbackCallback_);

    // re-SETUP：先摘掉旧传输
    const auto old = m_tracks_.find(trackId);
    if (old != m_tracks_.end())
    {
        old->second->setPlaying(false);
        old->second->close();
        m_tracks_.erase(old);
    }

    if (m_sessionId_.empty())
        m_sessionId_ = "uav-" + std::to_string(s_sessionCounter.fetch_add(1));
    m_tracks_[trackId] = transport;

    send_response(200, "OK", request,
                  "Transport: " + transport->transportEcho() + "\r\n"
                  "Session: " + m_sessionId_ + ";timeout=60\r\n");
}

void RtspSession::handle_play(const Request& request)
{
    if (!session_valid(request))
        return send_response(454, "Session Not Found", request);

    const auto source = m_sourceLookup_ ? m_sourceLookup_(m_path_) : nullptr;
    if (!source)
        return send_response(404, "Not Found", request);

    // 握手完成：把该客户端的所有传输订阅到源，源开始转发 RTP
    for (auto& kv : m_tracks_)
    {
        source->addSink(kv.second);
        kv.second->setPlaying(true);
    }
    m_playing_ = true;

    std::string extra = "Session: " + m_sessionId_ + "\r\n"
                        "Range: npt=0.000-\r\n";
    if (!m_tracks_.empty())
        extra += "RTP-Info: " + rtp_info() + "\r\n";

    send_response(200, "OK", request, std::move(extra));
}

void RtspSession::handle_pause(const Request& request)
{
    if (!session_valid(request))
        return send_response(454, "Session Not Found", request);

    const auto source = m_sourceLookup_ ? m_sourceLookup_(m_path_) : nullptr;
    if (source)
    {
        for (auto& kv : m_tracks_)
        {
            source->removeSink(kv.second.get());
            kv.second->setPlaying(false);
        }
    }
    m_playing_ = false;
    send_response(200, "OK", request, "Session: " + m_sessionId_ + "\r\n");
}

void RtspSession::handle_teardown(const Request& request)
{
    if (!session_valid(request))
        return send_response(454, "Session Not Found", request);

    const auto source = m_sourceLookup_ ? m_sourceLookup_(m_path_) : nullptr;
    if (source)
    {
        for (auto& kv : m_tracks_)
        {
            source->removeSink(kv.second.get());
            kv.second->setPlaying(false);
            kv.second->close();
        }
    }
    m_tracks_.clear();
    m_playing_ = false;

    send_response(200, "OK", request, "Session: " + m_sessionId_ + "\r\n");

    // TEARDOWN 结束会话：之后需要重新 SETUP
    m_sessionId_.clear();
}

// =============================== 响应与写队列 ===============================

void RtspSession::send_response(int status, const char* reason, const Request& request,
                                std::string extraHeaders, std::string body,
                                const char* contentType)
{
    const std::string cseq = header(request, "cseq");

    std::string response;
    response.reserve(128 + extraHeaders.size() + body.size());
    response += "RTSP/1.0 ";
    response += std::to_string(status);
    response += ' ';
    response += reason;
    response += "\r\n";
    if (!cseq.empty())
        response += "CSeq: " + cseq + "\r\n";
    response += "Server: UAV_detection\r\n";
    response += extraHeaders;
    if (contentType != nullptr)
        response += std::string("Content-Type: ") + contentType + "\r\n";
    if (!body.empty() || contentType != nullptr)
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "\r\n";
    response += body;

    UAV_LOG_INFO << "RTSP " << request.method << " " << request.uri
                 << " -> " << status << " " << reason;

    queue_write(std::move(response));
}

void RtspSession::send_error_and_close(int status, const char* reason)
{
    static const Request kDummyRequest;
    send_response(status, reason, kDummyRequest, "Connection: Close\r\n");
    close();
}

void RtspSession::queue_write(std::string data)
{
    m_writeQueue_.push_back(std::move(data));
    if (!m_writeInProgress_)
    {
        m_writeInProgress_ = true;
        do_write();
    }
}

void RtspSession::do_write()
{
    auto self = shared_from_this();
    boost::asio::async_write(m_socket_, boost::asio::buffer(m_writeQueue_.front()),
        [self](const boost::system::error_code& ec, std::size_t)
        {
            if (!ec)
            {
                self->m_writeQueue_.pop_front();
                if (!self->m_writeQueue_.empty())
                    self->do_write();
                else
                    self->m_writeInProgress_ = false;
            }
            else
            {
                self->m_writeInProgress_ = false;
                self->close();
            }
        });
}

void RtspSession::writeInterleaved(int channel,
                                   const std::shared_ptr<const std::vector<std::uint8_t>>& data)
{
    if (m_closed_ || !data || data->size() > 0xFFFF)
        return;

    std::string frame;
    frame.reserve(data->size() + 4);
    frame += '$';
    frame += static_cast<char>(channel & 0xFF);
    frame += static_cast<char>((data->size() >> 8) & 0xFF);
    frame += static_cast<char>(data->size() & 0xFF);
    frame.append(reinterpret_cast<const char*>(data->data()), data->size());

    queue_write(std::move(frame));
}

// =============================== 工具函数 ===============================

std::string RtspSession::header(const Request& request, const char* name) const
{
    const auto it = request.headers.find(name);
    return it == request.headers.end() ? std::string() : it->second;
}

std::string RtspSession::local_ip() const
{
    boost::system::error_code ec;
    const auto endpoint = m_socket_.local_endpoint(ec);
    if (!ec)
        return endpoint.address().to_string();
    return "0.0.0.0";
}

std::string RtspSession::rtsp_base() const
{
    return "rtsp://" + local_ip() + ":" + std::to_string(RTSP_PORT) + m_path_ + "/";
}

std::string RtspSession::rtp_info() const
{
    const std::string base = rtsp_base();
    std::string info;
    for (const auto& kv : m_tracks_)
    {
        if (!info.empty())
            info += ",";
        info += "url=" + base + kv.first + ";seq=0;rtptime=0";
    }
    return info;
}

bool RtspSession::session_valid(const Request& request) const
{
    if (m_sessionId_.empty())
        return false;

    std::string session = header(request, "session");
    const auto param = session.find(';');
    if (param != std::string::npos)
        session.resize(param);            // 容忍客户端带 ;timeout= 等参数
    return session == m_sessionId_;
}
