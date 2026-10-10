#include "ps_rtp_pusher.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <ctime>

#include "log/log.h"

static const std::size_t RTP_HEADER_SIZE = 12;

PsRtpPusher::~PsRtpPusher()
{
    close();
}

// ---------------------------------------------------------------------------
// 建立 / 关闭发送通道
// ---------------------------------------------------------------------------
int PsRtpPusher::open(const Config &config)
{
    close();

    if (config.dstIp.empty() || config.dstPort == 0)
    {
        UAV_LOG_WARN << "PsRtpPusher::open 目标地址/端口为空\n";
        return -1;
    }

    m_cfg = config;
    m_muxer.setConfig(config.ps);

    // SSRC：SDP y= 行没给就自己生成一个（GB28181 约定 10 位十进制）
    if (m_cfg.ssrc != 0)
    {
        m_ssrc = m_cfg.ssrc;
    }
    else
    {
        m_ssrc = static_cast<std::uint32_t>(time(nullptr)) & 0x3FFFFFFFu;
        if (m_ssrc == 0)
        {
            m_ssrc = 1;
        }
    }

    m_dst = sockaddr_in{};
    m_dst.sin_family = AF_INET;
    m_dst.sin_port = htons(m_cfg.dstPort);
    if (inet_pton(AF_INET, m_cfg.dstIp.c_str(), &m_dst.sin_addr) != 1)
    {
        UAV_LOG_WARN << "PsRtpPusher::open 非法目标地址: " << m_cfg.dstIp << "\n";
        return -1;
    }

    const bool tcpServer = m_cfg.tcp && (m_cfg.tcpMode == TcpMode::Server);

    if (tcpServer && m_cfg.localPort <= 0)
    {
        UAV_LOG_WARN << "PsRtpPusher::open TCP Server 模式必须指定 localPort\n";
        return -1;
    }

    const int type = m_cfg.tcp ? SOCK_STREAM : SOCK_DGRAM;

    if (tcpServer)
    {
        // TCP 被动模式：绑定本地媒体端口等平台来连（国标 TCP 点播常见，设备 a=setup:passive）
        m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenFd < 0)
        {
            UAV_LOG_WARN << "PsRtpPusher::open listen socket 失败: " << strerror(errno) << "\n";
            return -1;
        }

        const int reuse = 1;
        ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(static_cast<std::uint16_t>(m_cfg.localPort));
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(m_listenFd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0 ||
            ::listen(m_listenFd, 1) != 0)
        {
            UAV_LOG_WARN << "PsRtpPusher::open 监听 " << m_cfg.localPort
                         << " 失败: " << strerror(errno) << "\n";
            close();
            return -1;
        }

        // 非阻塞：连接在 pushFrame 里尝试 accept，未连上时丢帧（下个关键帧会重发 PSI）
        const int flags = ::fcntl(m_listenFd, F_GETFL, 0);
        ::fcntl(m_listenFd, F_SETFL, flags | O_NONBLOCK);
    }
    else
    {
        m_fd = ::socket(AF_INET, type, 0);
        if (m_fd < 0)
        {
            UAV_LOG_WARN << "PsRtpPusher::open socket 失败: " << strerror(errno) << "\n";
            return -1;
        }

        if (!m_cfg.tcp && m_cfg.localPort > 0)
        {
            // UDP 只发不收时，绑定协商端口只是为了源端口与 SDP 一致、并兼顾对端可能发来的 RTCP。
            // 端口被占用（例如同机多路、或与其他模块冲突）不该导致整路点播失败，
            // 因此这里降级为警告，继续用系统分配的临时端口发送。
            const int reuse = 1;
            ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(static_cast<std::uint16_t>(m_cfg.localPort));
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(m_fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0)
            {
                UAV_LOG_WARN << "PsRtpPusher::open 绑定端口 " << m_cfg.localPort
                             << " 失败(" << strerror(errno) << ")，改用临时端口发送\n";
            }
        }

        if (m_cfg.tcp)
        {
            if (::connect(m_fd, reinterpret_cast<sockaddr *>(&m_dst), sizeof(m_dst)) != 0)
            {
                UAV_LOG_WARN << "PsRtpPusher::open TCP 连接 " << m_cfg.dstIp << ":" << m_cfg.dstPort
                             << " 失败: " << strerror(errno) << "\n";
                close();
                return -1;
            }
            const int nodelay = 1;
            ::setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        }
    }

    m_seq = static_cast<std::uint16_t>(time(nullptr));
    m_psBuf.clear();
    m_psBuf.reserve(128 * 1024);
    // TCP + RFC4571 时每包前面还要 2 字节长度前缀
    const std::size_t prefix = (m_cfg.tcp && m_cfg.tcpFraming == TcpFraming::Rfc4571) ? 2 : 0;
    m_rtpBuf.resize((m_cfg.mtu > RTP_HEADER_SIZE ? m_cfg.mtu : 1400) + prefix);

    UAV_LOG_INFO << "PsRtpPusher 已启动: "
                 << (m_cfg.tcp ? (tcpServer ? "TCP-Server" : "TCP-Client") : "UDP")
                 << (m_cfg.tcp && m_cfg.tcpFraming == TcpFraming::Rfc4571 ? "/RFC4571" : "")
                 << " -> " << m_cfg.dstIp << ":" << m_cfg.dstPort
                 << ", ssrc=" << m_ssrc
                 << ", pt=" << static_cast<int>(m_cfg.payloadType)
                 << ", mtu=" << m_cfg.mtu
                 << (tcpServer ? (", 监听端口=" + std::to_string(m_cfg.localPort)) : std::string())
                 << "\n";
    return 0;
}

void PsRtpPusher::close()
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_listenFd >= 0)
    {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
}

// TCP Server：非阻塞接受平台连入（成功则后续数据发到该连接）
void PsRtpPusher::tryAccept()
{
    if (m_listenFd < 0)
    {
        return;
    }

    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    const int conn = ::accept(m_listenFd, reinterpret_cast<sockaddr *>(&peer), &peerLen);
    if (conn < 0)
    {
        return;   // 还没有连入（EAGAIN），下帧再试
    }

    const int nodelay = 1;
    ::setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    m_fd = conn;
    m_dst = peer;

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    UAV_LOG_INFO << "TCP 媒体连接已建立: " << ip << ":" << ntohs(peer.sin_port) << "\n";
}

// ---------------------------------------------------------------------------
// 发送一帧：帧 → PS → RTP 分片
// ---------------------------------------------------------------------------
int PsRtpPusher::pushFrame(const std::uint8_t *annexB, std::size_t size,
                           std::uint64_t ptsUs, bool keyFrame)
{
    // TCP Server：平台还没连入时先尝试接受，未连上就跳过本帧
    // （下个关键帧会重发 PS 头/系统头/PSM，所以丢几帧不影响解码）
    if (m_listenFd >= 0 && m_fd < 0)
    {
        tryAccept();
        if (m_fd < 0)
        {
            ++m_stats.skipped;
            return 0;
        }
    }

    if (m_fd < 0)
    {
        UAV_LOG_WARN << "PsRtpPusher::pushFrame 通道未打开\n";
        return -1;
    }
    if (annexB == nullptr || size == 0)
    {
        return -1;
    }

    m_psBuf.clear();
    const int psLen = m_muxer.packFrame(annexB, size, ptsUs, keyFrame, m_psBuf);
    if (psLen <= 0)
    {
        ++m_stats.errors;
        return -1;
    }

    const std::uint32_t ts = PsMuxer::toTimestamp90k(ptsUs);
    if (sendPackets(m_psBuf.data(), m_psBuf.size(), ts) != 0)
    {
        ++m_stats.errors;
        return -1;
    }

    ++m_stats.frames;
    m_stats.psBytes += static_cast<std::uint64_t>(m_psBuf.size());
    return psLen;
}

int PsRtpPusher::sendPackets(const std::uint8_t *ps, std::size_t len, std::uint32_t timestamp)
{
    // TCP + RFC4571：每个 RTP 包前置 2 字节大端长度，接收方可精确定界
    const std::size_t prefix =
        (m_cfg.tcp && m_cfg.tcpFraming == TcpFraming::Rfc4571) ? 2 : 0;
    const std::size_t capacity = m_cfg.mtu > RTP_HEADER_SIZE ? m_cfg.mtu : 1400;
    if (m_rtpBuf.size() < capacity + prefix)
    {
        m_rtpBuf.resize(capacity + prefix);
    }

    // 单个 RTP 包的 PS 净荷长度
    const std::size_t maxPayload = capacity - RTP_HEADER_SIZE;

    std::size_t offset = 0;
    while (offset < len)
    {
        const std::size_t n = ((len - offset) < maxPayload) ? (len - offset) : maxPayload;
        const bool marker = (offset + n == len);

        std::uint8_t *h = m_rtpBuf.data() + prefix;
        h[0] = 0x80;                                        // V=2, P=0, X=0, CC=0
        h[1] = static_cast<std::uint8_t>((marker ? 0x80 : 0x00) | (m_cfg.payloadType & 0x7F));
        h[2] = static_cast<std::uint8_t>(m_seq >> 8);
        h[3] = static_cast<std::uint8_t>(m_seq & 0xFF);
        h[4] = static_cast<std::uint8_t>(timestamp >> 24);
        h[5] = static_cast<std::uint8_t>((timestamp >> 16) & 0xFF);
        h[6] = static_cast<std::uint8_t>((timestamp >> 8) & 0xFF);
        h[7] = static_cast<std::uint8_t>(timestamp & 0xFF);
        h[8] = static_cast<std::uint8_t>(m_ssrc >> 24);
        h[9] = static_cast<std::uint8_t>((m_ssrc >> 16) & 0xFF);
        h[10] = static_cast<std::uint8_t>((m_ssrc >> 8) & 0xFF);
        h[11] = static_cast<std::uint8_t>(m_ssrc & 0xFF);

        memcpy(h + RTP_HEADER_SIZE, ps + offset, n);

        if (prefix > 0)
        {
            const std::size_t pktLen = RTP_HEADER_SIZE + n;
            m_rtpBuf[0] = static_cast<std::uint8_t>((pktLen >> 8) & 0xFF);
            m_rtpBuf[1] = static_cast<std::uint8_t>(pktLen & 0xFF);
        }

        if (sendAll(m_rtpBuf.data(), prefix + RTP_HEADER_SIZE + n) != 0)
        {
            return -1;
        }

        ++m_seq;
        ++m_stats.rtpPackets;
        m_stats.rtpBytes += RTP_HEADER_SIZE + n;

        offset += n;
    }

    return 0;
}

int PsRtpPusher::sendAll(const std::uint8_t *data, std::size_t len) const
{
    if (m_cfg.tcp)
    {
        // TCP：RTP 包按序直发（GB28181 媒体 TCP 传输不做长度前缀）
        std::size_t sent = 0;
        while (sent < len)
        {
            const ssize_t n = ::send(m_fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                UAV_LOG_WARN << "PsRtpPusher TCP 发送失败: " << strerror(errno) << "\n";
                return -1;
            }
            sent += static_cast<std::size_t>(n);
        }
        return 0;
    }

    const ssize_t n = ::sendto(m_fd, data, len, 0,
                               reinterpret_cast<const sockaddr *>(&m_dst), sizeof(m_dst));
    if (n != static_cast<ssize_t>(len))
    {
        UAV_LOG_WARN << "PsRtpPusher UDP 发送失败: " << strerror(errno) << "\n";
        return -1;
    }
    return 0;
}
