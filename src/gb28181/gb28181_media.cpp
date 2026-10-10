#include "gb28181_media.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log/log.h"

Gb28181Media::~Gb28181Media()
{
    stopAll();
}

// ---------------------------------------------------------------------------
// 平台 SDP 解析
//
// GB28181 实时点播平台的 SDP 形如：
//   v=0
//   o=34020000002000000001 0 0 IN IP4 10.0.0.1
//   s=Play
//   c=IN IP4 10.0.0.1
//   t=0 0
//   m=video 6000 RTP/AVP 96
//   a=recvonly
//   a=rtpmap:96 PS/90000
//   y=0100000001
//   f=v/2/5/25/1/4000a
// ---------------------------------------------------------------------------
Gb28181Media::RemoteMedia Gb28181Media::parseSdp(const char *sdp)
{
    RemoteMedia media;
    if (sdp == nullptr || *sdp == '\0')
    {
        return media;
    }

    // 会话级 c= 在第一个 m= 之前生效，媒体级 c= 覆盖之
    std::string current;
    bool inMediaSection = false;

    const char *p = sdp;
    while (*p != '\0')
    {
        const char *eol = strpbrk(p, "\r\n");
        const std::size_t len = (eol != nullptr) ? static_cast<std::size_t>(eol - p) : strlen(p);
        std::string line(p, len);
        p += len;
        while (*p == '\r' || *p == '\n')
        {
            ++p;
        }

        if (line.size() < 2 || line[1] != '=')
        {
            continue;
        }

        const char type = line[0];
        const std::string value = line.substr(2);

        switch (type)
        {
        case 'c':
        {
            // c=IN IP4 10.0.0.1
            const std::size_t pos = value.find("IP4 ");
            if (pos != std::string::npos)
            {
                std::string ip = value.substr(pos + 4);
                const std::size_t sp = ip.find(' ');
                if (sp != std::string::npos)
                {
                    ip = ip.substr(0, sp);
                }
                if (!ip.empty() && ip != "0.0.0.0")
                {
                    media.ip = ip;
                }
            }
            (void)inMediaSection;
            break;
        }
        case 'm':
        {
            inMediaSection = true;
            if (value.rfind("video", 0) != 0)
            {
                break;   // 只关心视频
            }
            // m=video 6000 RTP/AVP 96  或  m=video 6000 TCP/RTP/AVP 96
            char proto[64] = {0};
            unsigned port = 0;
            if (sscanf(value.c_str(), "video %u %63s", &port, proto) >= 2)
            {
                media.port = static_cast<std::uint16_t>(port);
                media.tcp = (strstr(proto, "TCP") != nullptr);
            }
            break;
        }
        case 'y':
            // y=0100000001（十进制 SSRC）
            media.ssrc = static_cast<std::uint32_t>(strtoul(value.c_str(), nullptr, 10));
            break;
        case 's':
            media.sessionName = value;
            media.playback = (value.find("Playback") != std::string::npos ||
                              value.find("Download") != std::string::npos);
            break;
        case 'a':
            if (value == "setup:passive")
            {
                media.setupPassive = true;
            }
            else if (value == "setup:active")
            {
                media.setupPassive = false;
            }
            break;
        default:
            break;
        }
    }

    media.valid = (media.port != 0);
    if (!media.valid)
    {
        UAV_LOG_WARN << "SDP 解析失败：没有可用的 m=video 端口\n";
    }
    return media;
}

// ---------------------------------------------------------------------------
// 通道登记
// ---------------------------------------------------------------------------
void Gb28181Media::setChannels(const gb28181_system_param *sys_param)
{
    if (sys_param == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_channelIds.clear();
    for (int i = 0; i < sys_param->channel_size && i < GB_CAMERA_MAX; ++i)
    {
        m_channelIds[sys_param->channel_para[i].channel_index] =
            sys_param->channel_para[i].channel_id;
    }
    UAV_LOG_INFO << "GB28181 媒体通道登记: " << m_channelIds.size() << " 路\n";
}

// ---------------------------------------------------------------------------
// 媒体端口池
//
// 每路点播会话独占一个端口：SDP 应答里声明的端口就是该会话实际绑定/监听的端口，
// 多路并发（尤其 TCP 被动模式）才不会互相抢端口。
// ---------------------------------------------------------------------------
int Gb28181Media::allocPort(int did, const char *channelId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto exist = m_ports.find(did);
    if (exist != m_ports.end())
    {
        return exist->second;   // 同一会话重复分配返回原端口
    }

    const int count = (m_cfg.portCount > 0) ? m_cfg.portCount : 1;
    for (int i = 0; i < count; ++i)
    {
        const int offset = (m_nextPortOffset + i) % count;
        const int port = m_cfg.portBase + offset;

        bool used = false;
        for (const auto &kv : m_ports)
        {
            if (kv.second == port)
            {
                used = true;
                break;
            }
        }
        if (used)
        {
            continue;
        }

        m_ports[did] = port;
        m_nextPortOffset = (offset + 1) % count;
        UAV_LOG_INFO << "媒体端口分配: did=" << did
                     << ", 通道=" << (channelId != nullptr ? channelId : "")
                     << ", port=" << port
                     << " (已用 " << m_ports.size() << "/" << count << ")\n";
        return port;
    }

    UAV_LOG_WARN << "媒体端口池已满(" << count << " 路)，did=" << did << " 分配失败\n";
    return -1;
}

void Gb28181Media::freePort(int did)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ports.erase(did);
}

int Gb28181Media::portOf(int did) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_ports.find(did);
    return (it != m_ports.end()) ? it->second : -1;
}

// ---------------------------------------------------------------------------
// 会话生命周期
// ---------------------------------------------------------------------------
int Gb28181Media::startSession(int did, const char *channelId, const char *remoteSdp)
{
    const RemoteMedia media = parseSdp(remoteSdp);
    if (!media.valid)
    {
        UAV_LOG_WARN << "startSession 失败: SDP 无有效视频端口, did=" << did << "\n";
        return -1;
    }
    if (media.ip.empty())
    {
        UAV_LOG_WARN << "startSession 失败: SDP 缺少 c= 媒体地址, did=" << did << "\n";
        return -1;
    }

    PsRtpPusher::Config cfg;
    cfg.dstIp = media.ip;
    cfg.dstPort = media.port;
    cfg.ssrc = media.ssrc;
    cfg.payloadType = 96;
    cfg.mtu = static_cast<std::uint16_t>(m_cfg.mtu);
    cfg.tcp = media.tcp;
    cfg.tcpFraming = m_cfg.tcpFraming;
    cfg.ps = m_cfg.ps;
    // 平台声明 passive 时由设备主动连；否则（平台 active）设备监听等它连入
    cfg.tcpMode = (media.tcp && media.setupPassive) ? PsRtpPusher::TcpMode::Client
                                                    : PsRtpPusher::TcpMode::Server;
    // 用该会话已分配的端口（SDP 应答里声明的就是它）；没分配过则现场补一个
    int localPort = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_ports.find(did);
        if (it != m_ports.end())
        {
            localPort = it->second;
        }
    }
    if (localPort < 0)
    {
        localPort = allocPort(did, channelId);
    }
    if (localPort > 0)
    {
        cfg.localPort = localPort;
    }

    auto pusher = std::make_shared<PsRtpPusher>();
    if (pusher->open(cfg) != 0)
    {
        UAV_LOG_WARN << "startSession 失败: 媒体通道打开失败, did=" << did << "\n";
        return -1;
    }

    auto session = std::make_shared<Session>();
    session->did = did;
    session->channelId = (channelId != nullptr) ? channelId : "";
    session->media = media;
    session->pusher = pusher;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions[did] = session;
    }

    UAV_LOG_INFO << "点播会话已建立: did=" << did
                 << ", 通道=" << session->channelId
                 << ", 本地端口=" << cfg.localPort
                 << ", " << media.ip << ":" << media.port
                 << ", " << (media.tcp ? "TCP" : "UDP")
                 << ", ssrc=" << media.ssrc
                 << (media.playback ? ", 回放" : ", 实时") << "\n";
    return 0;
}

void Gb28181Media::stopSession(int did)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(did);
        if (it == m_sessions.end())
        {
            return;
        }
        session = it->second;
        m_sessions.erase(it);
        m_ports.erase(did);          // 释放端口，供后续点播复用
    }

    if (session != nullptr && session->pusher != nullptr)
    {
        session->pusher->close();
        const auto &st = session->pusher->stats();
        UAV_LOG_INFO << "点播会话已结束: did=" << did
                     << ", 通道=" << session->channelId
                     << ", 发送 " << st.frames << " 帧 / " << st.rtpPackets << " 个 RTP 包"
                     << " / " << st.psBytes << " 字节 PS\n";
    }
}

void Gb28181Media::stopAll()
{
    std::map<int, std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        sessions.swap(m_sessions);
        m_ports.clear();
    }
    for (auto &kv : sessions)
    {
        if (kv.second != nullptr && kv.second->pusher != nullptr)
        {
            kv.second->pusher->close();
        }
    }
}

// ---------------------------------------------------------------------------
// 帧分发
// ---------------------------------------------------------------------------
int Gb28181Media::pushFrame(int channelIndex, const std::uint8_t *annexB, std::size_t size,
                            std::uint64_t ptsUs, bool keyFrame)
{
    if (annexB == nullptr || size == 0)
    {
        return 0;
    }

    // 锁内只做会话筛选，实际发送放到锁外，避免阻塞信令线程
    std::vector<std::shared_ptr<PsRtpPusher>> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.empty())
        {
            return 0;
        }

        // 未登记的通道序号：不发送（避免把某一路视频误当成"任意通道"广播出去）
        auto it = m_channelIds.find(channelIndex);
        if (it == m_channelIds.end())
        {
            return 0;
        }
        const std::string &channelId = it->second;

        for (auto &kv : m_sessions)
        {
            const Session &session = *kv.second;
            // 平台 INVITE 未指定通道（channelId 为空）时，视为点播本设备任意一路
            if (session.channelId.empty() || session.channelId == channelId)
            {
                targets.push_back(session.pusher);
            }
        }
    }

    int sent = 0;
    for (auto &pusher : targets)
    {
        if (pusher == nullptr)
        {
            continue;
        }
        // pushFrame 返回 0 表示 TCP 尚未连入而跳过，不计入成功
        if (pusher->pushFrame(annexB, size, ptsUs, keyFrame) > 0)
        {
            ++sent;
        }
    }
    return sent;
}

std::vector<Gb28181Media::SessionSnapshot> Gb28181Media::sessions() const
{
    std::vector<SessionSnapshot> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    out.reserve(m_sessions.size());
    for (const auto &kv : m_sessions)
    {
        if (kv.second == nullptr || kv.second->pusher == nullptr)
        {
            continue;
        }
        SessionSnapshot snap;
        snap.did = kv.second->did;
        snap.channelId = kv.second->channelId;
        snap.media = kv.second->media;
        snap.connected = kv.second->pusher->isConnected();
        snap.stats = kv.second->pusher->stats();
        out.push_back(std::move(snap));
    }
    return out;
}

std::size_t Gb28181Media::sessionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}
