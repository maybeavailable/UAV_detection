//=============================================================================
// gb28181 SIP 信令闭环自测（宿主机原生运行，不需要真实平台）
//
// 用一个"假平台"（原始 UDP socket 实现的极小 SIP registrar）与设备侧
// SipSession 对接，验证完整链路：
//   1. REGISTER → 401 挑战 → 带 Authorization 重新注册 → 200 OK（摘要认证，
//      假平台会用 MD5 独立复算一遍 digest response 来校验设备算得对不对）
//   2. 平台下发 MESSAGE（DeviceControl/PTZCmd）→ 设备解析 → 回 200 OK
//   3. 设备用 XmlPacketBuilder 组 DeviceControl 应答 → 作为 MESSAGE 发回平台
//   4. 设备按 heartbeatInterval 周期上报 Keepalive
//   5. 完整点播链路：平台 INVITE(带SDP) → 设备 200 OK → ACK → 设备把 H.264 帧
//      打成 PS/RTP 推给平台 → 平台收流还原 PS 并用 ffprobe 解码验证 → BYE 停流
//
// 准备（宿主机原生 eXosip2，仅本自测使用；板端走 aarch64 版本）：
//   ROOT=$(git rev-parse --show-toplevel); cd "$ROOT/build/thirdparty-src"
//   # 源码目录由 third_party/*/build-aarch64.sh 下载解压得到
//   mkdir -p libosip2-5.3.2/build-host
//   (cd libosip2-5.3.2/build-host && ../../libosip2-5.3.2/configure
//      --prefix="$ROOT/build/host-exosip" --disable-shared --enable-static 
//      --disable-dependency-tracking CFLAGS="-O2 -fPIC" && make -j && make install)
//   mkdir -p libexosip2-5.3.0/build-host
//   (cd libexosip2-5.3.0/build-host && ../../libexosip2-5.3.0/configure
//      --prefix="$ROOT/build/host-exosip" --disable-shared --enable-static 
//      --disable-openssl --disable-tools --disable-srvrec --disable-dependency-tracking 
//      PKG_CONFIG_PATH="$ROOT/build/host-exosip/lib/pkgconfig" && make -j && make install)
//
// 编译运行（在仓库根目录，下面每条命令单独一行，勿用续行符拼接）：
//   g++ -std=c++17 -I src -I build/host-exosip/include -I /usr/include/libxml2
//       tools/gb28181_sip_selftest.cpp src/gb28181/xmlpacket.cpp src/gb28181/xmlpacketparser.cpp
//       src/gb28181/sipsession.cpp src/gb28181/ps_mux.cpp src/gb28181/ps_rtp_pusher.cpp
//       src/gb28181/gb28181_media.cpp src/log/log.cpp src/log/RotateFileWriter.cpp
//       -L build/host-exosip/lib -leXosip2 -losip2 -losipparser2 -lxml2 -lcrypto -lpthread
//       -o build/gb28181_selftest
//   ./build/gb28181_selftest
//=============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gb28181/gb28181_media.h"
#include "gb28181/gb28181device.h"
#include "gb28181/sipsession.h"
#include "log/log.h"

static int g_fail = 0;
#define CHECK(cond, name)                                     \
    do {                                                      \
        if (cond) { printf("[PASS] %s\n", name); }            \
        else { printf("[FAIL] %s\n", name); g_fail++; }        \
    } while (0)

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
static std::string md5_hex(const std::string &in)
{
    unsigned char digest[EVP_MAX_MD_SIZE] = {0};
    unsigned int len = 0;
    EVP_Digest(in.data(), in.size(), digest, &len, EVP_md5(), nullptr);
    char out[EVP_MAX_MD_SIZE * 2 + 1] = {0};
    for (unsigned int i = 0; i < len; i++)
    {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(out);
}

struct SipMessage
{
    bool is_request{false};
    std::string method;      // 请求方法或状态码
    std::string uri;         // 请求 URI
    std::string status_line;
    std::map<std::string, std::string> headers;   // key 统一小写
    std::string body;

    std::string header(const std::string &name) const
    {
        auto it = headers.find(name);
        return (it == headers.end()) ? std::string() : it->second;
    }
};

static std::string lower(const std::string &s)
{
    std::string r = s;
    for (auto &c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
}

static SipMessage parse_sip(const std::string &raw)
{
    SipMessage m;
    size_t hdr_end = raw.find("\r\n\r\n");
    std::string head = (hdr_end == std::string::npos) ? raw : raw.substr(0, hdr_end);
    if (hdr_end != std::string::npos)
    {
        m.body = raw.substr(hdr_end + 4);
    }

    size_t pos = 0;
    bool first = true;
    while (pos <= head.size())
    {
        size_t eol = head.find("\r\n", pos);
        std::string line = head.substr(pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
        if (first)
        {
            first = false;
            if (line.rfind("SIP/2.0", 0) == 0)
            {
                m.is_request = false;
                m.status_line = line;
                size_t sp = line.find(' ');
                m.method = (sp == std::string::npos) ? "" : line.substr(sp + 1, 3);
            }
            else
            {
                m.is_request = true;
                size_t sp1 = line.find(' ');
                size_t sp2 = line.find(' ', sp1 + 1);
                m.method = line.substr(0, sp1);
                m.uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
            }
        }
        else if (!line.empty())
        {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                m.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }
    return m;
}

// 从 Digest 头里取参数
static std::string digest_param(const std::string &header, const std::string &key)
{
    std::string k = key + "=";
    size_t p = header.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    std::string val;
    if (p < header.size() && header[p] == '"')
    {
        size_t e = header.find('"', p + 1);
        val = header.substr(p + 1, e - p - 1);
    }
    else
    {
        size_t e = header.find_first_of(", ", p);
        val = header.substr(p, (e == std::string::npos) ? std::string::npos : e - p);
    }
    return trim(val);
}

// ---------------------------------------------------------------------------
// 平台侧 RTP 接收器：按序收包并还原出 PS（模拟平台收流）
// ---------------------------------------------------------------------------
class RtpReceiver
{
public:
    ~RtpReceiver() { stop(); }

    bool start(std::uint16_t port)
    {
        stop();
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
        {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int reuse = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        m_running = true;
        m_thread = std::thread(&RtpReceiver::loop, this);
        return true;
    }

    void stop()
    {
        m_running = false;
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        if (m_fd >= 0)
        {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    std::vector<std::uint8_t> payload() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_ps;
    }
    std::size_t packetCount() const { return m_packets; }
    std::size_t markerCount() const { return m_markers; }
    std::size_t badPackets() const { return m_bad; }
    std::uint8_t lastPt() const { return m_pt; }

private:
    void loop()
    {
        std::vector<std::uint8_t> buf(4096);
        while (m_running.load())
        {
            const ssize_t n = ::recv(m_fd, buf.data(), buf.size(), 0);
            if (n < 12 || (buf[0] >> 6) != 2)
            {
                if (n > 0)
                {
                    ++m_bad;
                }
                continue;
            }
            m_pt = buf[1] & 0x7F;
            if (buf[1] & 0x80)
            {
                ++m_markers;
            }
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_ps.insert(m_ps.end(), buf.begin() + 12, buf.begin() + n);
            }
            ++m_packets;
        }
    }

    int m_fd{-1};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    std::vector<std::uint8_t> m_ps;
    std::size_t m_packets{0};
    std::size_t m_markers{0};
    std::size_t m_bad{0};
    std::uint8_t m_pt{0};
};

// ---------------------------------------------------------------------------
// Annex-B 工具：把 H.264 裸流切成访问单元（每个 VCL NAL 一帧，关键帧带参数集）
// ---------------------------------------------------------------------------
struct TestFrame
{
    std::vector<std::uint8_t> data;
    bool key{false};
};

static std::vector<TestFrame> loadH264Frames(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<TestFrame> frames;
    if (!in)
    {
        return frames;
    }
    std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());

    // 切 NAL
    struct Span { const std::uint8_t *d; std::size_t n; };
    std::vector<Span> nals;
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i + 3 <= buf.size();)
    {
        const bool sc4 = (i + 4 <= buf.size() && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1);
        const bool sc3 = (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1);
        if (sc3 || sc4)
        {
            if (start != std::string::npos)
            {
                nals.push_back({buf.data() + start, i - start});
            }
            start = i;
            i += sc4 ? 4 : 3;
            continue;
        }
        ++i;
    }
    if (start != std::string::npos)
    {
        nals.push_back({buf.data() + start, buf.size() - start});
    }

    auto nalType = [](const Span &sp) -> int {
        std::size_t off = (sp.n >= 4 && sp.d[2] == 0) ? 4 : 3;
        return (sp.n > off) ? (sp.d[off] & 0x1F) : -1;
    };

    std::vector<std::uint8_t> paramSets;
    for (const auto &sp : nals)
    {
        const int t = nalType(sp);
        if (t == 7 || t == 8)
        {
            paramSets.insert(paramSets.end(), sp.d, sp.d + sp.n);
        }
    }

    std::vector<std::uint8_t> pending;
    for (const auto &sp : nals)
    {
        const int t = nalType(sp);
        if (t == 1 || t == 5)
        {
            TestFrame f;
            f.key = (t == 5);
            if (f.key)
            {
                f.data = paramSets;
            }
            f.data.insert(f.data.end(), pending.begin(), pending.end());
            f.data.insert(f.data.end(), sp.d, sp.d + sp.n);
            frames.push_back(std::move(f));
            pending.clear();
        }
        else
        {
            pending.insert(pending.end(), sp.d, sp.d + sp.n);
        }
    }
    return frames;
}

// ---------------------------------------------------------------------------
// 假平台（registrar）
// ---------------------------------------------------------------------------
class FakePlatform
{
public:
    bool start(const char *ip, int port)
    {
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, ip, &addr.sin_addr);
        if (::bind(m_fd, (sockaddr *)&addr, sizeof(addr)) != 0)
        {
            printf("假平台 bind 失败: %s\n", strerror(errno));
            return false;
        }

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        m_running = true;
        m_thread = std::thread(&FakePlatform::loop, this);
        return true;
    }

    void stop()
    {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    }

    bool registered() const { return m_registered.load(); }
    bool invite_ok() const { return m_invite_ok.load(); }
    // 设备对某类查询的应答原文（按应答里的 CmdType 归类）
    std::string response(const std::string &cmdType) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_responses.find(cmdType);
        return (it == m_responses.end()) ? std::string() : it->second;
    }
    // INVITE 的 200 OK 里设备 SDP 的 m=video 端口
    std::vector<int> inviteSdpPorts() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_inviteSdpPorts;
    }
    bool bye_ok() const { return m_bye_ok.load(); }
    bool digest_ok() const { return m_digest_ok.load(); }
    bool got_message_ok() const { return m_got_message_ok.load(); }
    bool got_ptz_ack() const { return m_got_ptz_ack.load(); }
    bool got_keepalive() const { return m_got_keepalive.load(); }
    std::string ptz_ack_body() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_ptz_ack_body;
    }

    // 平台向设备下发一条 MESSAGE（这里用 DeviceControl/PTZCmd: 左转 速度0xFF）
    bool send_ptz_control(const std::string &device_id, const std::string &sn,
                          const std::string &ptz_hex)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_device_addr_valid) return false;

        char body[512] = {0};
        snprintf(body, sizeof(body),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                 "<Control>\r\n"
                 "<CmdType>DeviceControl</CmdType>\r\n"
                 "<SN>%s</SN>\r\n"
                 "<DeviceID>%s</DeviceID>\r\n"
                 "<PTZCmd>%s</PTZCmd>\r\n"
                 "</Control>\r\n",
                 sn.c_str(), device_id.c_str(), ptz_hex.c_str());

        char msg[2048] = {0};
        int n = snprintf(msg, sizeof(msg),
                 "MESSAGE sip:%s@127.0.0.1:%d SIP/2.0\r\n"
                 "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK-ptz-%u;rport\r\n"
                 "From: <sip:34020000002000000001@3402000000>;tag=plat-%u\r\n"
                 "To: <sip:%s@3402000000>\r\n"
                 "Call-ID: ptz-%u@127.0.0.1\r\n"
                 "CSeq: 1 MESSAGE\r\n"
                 "Max-Forwards: 70\r\n"
                 "Content-Type: Application/MANSCDP+xml\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n"
                 "%s",
                 device_id.c_str(), m_device_port,
                 m_port, m_seq, m_seq,
                 device_id.c_str(), m_seq,
                 strlen(body), body);
        m_seq++;

        ssize_t sent = ::sendto(m_fd, msg, (size_t)n, 0, (sockaddr *)&m_device_addr, sizeof(m_device_addr));
        return sent == n;
    }

    // 平台发一条查询类 MESSAGE（Catalog/DeviceInfo/DeviceStatus/PresetQuery）
    bool send_query(const std::string &cmdType, const std::string &deviceId, const std::string &sn)
    {
        char body[512] = {0};
        snprintf(body, sizeof(body),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                 "<Query>\r\n<CmdType>%s</CmdType>\r\n<SN>%s</SN>\r\n"
                 "<DeviceID>%s</DeviceID>\r\n</Query>\r\n",
                 cmdType.c_str(), sn.c_str(), deviceId.c_str());

        std::lock_guard<std::mutex> lk(m_mutex);
        return send_message_locked(body, sn);
    }

    // 平台向设备发起实时点播：INVITE + SDP（媒体地址/端口/SSRC 都在这）
    bool send_invite(const std::string &channelId, int devicePort, int mediaPort, std::uint32_t ssrc)
    {
        char sdp[512] = {0};
        const int sdpLen = snprintf(sdp, sizeof(sdp),
            "v=0\r\n"
            "o=34020000002000000001 0 0 IN IP4 127.0.0.1\r\n"
            "s=Play\r\n"
            "c=IN IP4 127.0.0.1\r\n"
            "t=0 0\r\n"
            "m=video %d RTP/AVP 96\r\n"
            "a=recvonly\r\n"
            "a=rtpmap:96 PS/90000\r\n"
            "y=%010u\r\n"
            "f=v/2/5/25/1/4000a\r\n",
            mediaPort, ssrc);

        std::lock_guard<std::mutex> lk(m_mutex);
        ++m_seq;
        m_inviteCallId = "invite-" + std::to_string(m_seq) + "@127.0.0.1";
        m_inviteFromTag = "plat-inv-" + std::to_string(m_seq);
        m_inviteChannel = channelId;

        char msg[2048] = {0};
        const int n = snprintf(msg, sizeof(msg),
            "INVITE sip:%s@127.0.0.1:%d SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK-inv-%u;rport\r\n"
            "From: <sip:34020000002000000001@3402000000>;tag=%s\r\n"
            "To: <sip:%s@3402000000>\r\n"
            "Call-ID: %s\r\n"
            "CSeq: 1 INVITE\r\n"
            "Contact: <sip:34020000002000000001@127.0.0.1:%d>\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            channelId.c_str(), devicePort,
            m_port, m_seq,
            m_inviteFromTag.c_str(),
            channelId.c_str(),
            m_inviteCallId.c_str(),
            m_port,
            sdpLen, sdp);

        const bool ok = ::sendto(m_fd, msg, (size_t)n, 0,
                                 (sockaddr *)&m_device_addr, sizeof(m_device_addr)) == n;
        ++m_seq;
        m_rtp.start(static_cast<std::uint16_t>(mediaPort));   // 平台侧开始收 RTP
        return ok;
    }

    // 收到 200 OK 后回 ACK，会话才真正建立
    bool send_ack()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_inviteToTag.empty())
        {
            return false;
        }
        // ACK 是独立事务：新的 branch + 相同的 Call-ID/From tag/To tag，请求 URI 指向对端 Contact
        const std::string reqUri = m_inviteContact.empty()
            ? ("sip:" + m_inviteChannel + "@127.0.0.1:" + std::to_string(m_device_port))
            : m_inviteContact;
        char msg[1024] = {0};
        const int n = snprintf(msg, sizeof(msg),
            "ACK %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK-ack-%u;rport\r\n"
            "From: <sip:34020000002000000001@3402000000>;tag=%s\r\n"
            "To: <sip:%s@3402000000>;tag=%s\r\n"
            "Call-ID: %s\r\n"
            "CSeq: 1 ACK\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            reqUri.c_str(),
            m_port, m_seq,
            m_inviteFromTag.c_str(),
            m_inviteChannel.c_str(), m_inviteToTag.c_str(),
            m_inviteCallId.c_str());
        ++m_seq;
        return ::sendto(m_fd, msg, (size_t)n, 0,
                        (sockaddr *)&m_device_addr, sizeof(m_device_addr)) == n;
    }

    // 平台结束点播：BYE
    bool send_bye()
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_inviteToTag.empty())
        {
            return false;
        }
        const std::string reqUri = m_inviteContact.empty()
            ? ("sip:" + m_inviteChannel + "@127.0.0.1:" + std::to_string(m_device_port))
            : m_inviteContact;
        char msg[1024] = {0};
        const int n = snprintf(msg, sizeof(msg),
            "BYE %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK-bye-%u;rport\r\n"
            "From: <sip:34020000002000000001@3402000000>;tag=%s\r\n"
            "To: <sip:%s@3402000000>;tag=%s\r\n"
            "Call-ID: %s\r\n"
            "CSeq: 2 BYE\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            reqUri.c_str(),
            m_port, m_seq,
            m_inviteFromTag.c_str(),
            m_inviteChannel.c_str(), m_inviteToTag.c_str(),
            m_inviteCallId.c_str());
        ++m_seq;
        return ::sendto(m_fd, msg, (size_t)n, 0,
                        (sockaddr *)&m_device_addr, sizeof(m_device_addr)) == n;
    }

    // 平台侧收到的 RTP（还原成 PS）
    std::vector<std::uint8_t> rtpPayload() const { return m_rtp.payload(); }
    std::size_t rtpPackets() const { return m_rtp.packetCount(); }
    std::size_t rtpMarkers() const { return m_rtp.markerCount(); }
    std::size_t rtpBad() const { return m_rtp.badPackets(); }
    std::uint8_t rtpLastPt() const { return m_rtp.lastPt(); }

private:
    void loop()
    {
        char buf[8192];
        while (m_running.load())
        {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            ssize_t n = ::recvfrom(m_fd, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &from_len);
            if (n <= 0) continue;
            buf[n] = 0;

            SipMessage msg = parse_sip(std::string(buf, (size_t)n));
            if (msg.is_request && msg.method == "REGISTER")
            {
                handle_register(msg, from);
            }
            else if (msg.is_request && msg.method == "MESSAGE")
            {
                handle_device_message(msg);
            }
            else if (!msg.is_request)
            {
                const std::string cseq = msg.header("cseq");
                const bool is200 = (msg.status_line.find("200") != std::string::npos);
                if (cseq.find("INVITE") != std::string::npos)
                {
                    if (is200)
                    {
                        // 从 200 OK 的 To 头里取 tag，ACK/BYE 必须带上
                        std::string to = msg.header("to");
                        const std::size_t pos = to.find("tag=");
                        std::string contact = msg.header("contact");
                        // Contact 形如 <sip:xxx@ip:port>，取尖括号里的 URI
                        const std::size_t lt = contact.find('<');
                        const std::size_t gt = contact.find('>');
                        if (lt != std::string::npos && gt != std::string::npos && gt > lt)
                        {
                            contact = contact.substr(lt + 1, gt - lt - 1);
                        }
                        std::lock_guard<std::mutex> lk(m_mutex);
                        if (pos != std::string::npos)
                        {
                            m_inviteToTag = to.substr(pos + 4);
                        }
                        m_inviteContact = contact;
                        // 记录设备应答 SDP 里的 m=video 端口，用于校验多路并发端口是否互不相同
                        const std::size_t mp = msg.body.find("m=video ");
                        if (mp != std::string::npos)
                        {
                            m_inviteSdpPorts.push_back(atoi(msg.body.c_str() + mp + 8));
                        }
                        m_invite_ok = true;
                    }
                }
                else if (cseq.find("BYE") != std::string::npos)
                {
                    if (is200)
                    {
                        m_bye_ok = true;
                    }
                }
                else if (cseq.find("MESSAGE") != std::string::npos && is200)
                {
                    m_got_message_ok = true;
                }
            }
        }
    }

    void handle_register(const SipMessage &req, const sockaddr_in &from)
    {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_device_addr = from;
            m_device_addr_valid = true;
            m_device_port = ntohs(from.sin_port);
        }

        std::string auth = req.header("authorization");
        std::string cseq = req.header("cseq");

        if (auth.empty())
        {
            // 第一次注册：发 401 挑战
            char resp[1024] = {0};
            snprintf(resp, sizeof(resp),
                     "SIP/2.0 401 Unauthorized\r\n"
                     "Via: %s\r\n"
                     "From: %s\r\n"
                     "To: %s;tag=plat401\r\n"
                     "Call-ID: %s\r\n"
                     "CSeq: %s\r\n"
                     "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n",
                     req.header("via").c_str(), req.header("from").c_str(),
                     req.header("to").c_str(), req.header("call-id").c_str(),
                     cseq.c_str(), REALM, NONCE);
            ::sendto(m_fd, resp, strlen(resp), 0, (sockaddr *)&from, sizeof(from));
            printf("[平台] 已发 401 挑战(带 Digest 认证要求)\n");
            return;
        }

        // 第二次注册：校验摘要
        std::string username = digest_param(auth, "username");
        std::string realm = digest_param(auth, "realm");
        std::string nonce = digest_param(auth, "nonce");
        std::string uri = digest_param(auth, "uri");
        std::string response = digest_param(auth, "response");
        std::string qop = digest_param(auth, "qop");
        std::string nc = digest_param(auth, "nc");
        std::string cnonce = digest_param(auth, "cnonce");

        std::string ha1 = md5_hex(username + ":" + realm + ":" + PASSWORD);
        std::string ha2 = md5_hex(std::string("REGISTER") + ":" + uri);
        std::string expect;
        if (!qop.empty())
        {
            expect = md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
        }
        else
        {
            expect = md5_hex(ha1 + ":" + nonce + ":" + ha2);
        }

        bool ok = (response == expect) && (username == DEVICE_ID) && (nonce == NONCE);
        m_digest_ok = ok;
        printf("[平台] 收到认证注册: user=%s realm=%s 摘要校验=%s\n",
               username.c_str(), realm.c_str(), ok ? "通过" : "失败");

        char resp[1024] = {0};
        snprintf(resp, sizeof(resp),
                 "SIP/2.0 200 OK\r\n"
                 "Via: %s\r\n"
                 "From: %s\r\n"
                 "To: %s;tag=plat200\r\n"
                 "Call-ID: %s\r\n"
                 "CSeq: %s\r\n"
                 "Contact: %s\r\n"
                 "Expires: 3600\r\n"
                 "Content-Length: 0\r\n"
                 "\r\n",
                 req.header("via").c_str(), req.header("from").c_str(),
                 req.header("to").c_str(), req.header("call-id").c_str(),
                 cseq.c_str(), req.header("contact").c_str());
        ::sendto(m_fd, resp, strlen(resp), 0, (sockaddr *)&from, sizeof(from));

        m_registered = true;
        printf("[平台] 已回 200 OK，设备注册成功\n");
    }

    void handle_device_message(const SipMessage &msg)
    {
        const std::string &body = msg.body;

        // 按应答里的 CmdType 归档，便于逐条校验设备是否真的回了正确内容
        {
            const std::string key = "<CmdType>";
            const std::size_t p1 = body.find(key);
            const std::size_t p2 = body.find("</CmdType>");
            if (p1 != std::string::npos && p2 != std::string::npos && p2 > p1)
            {
                const std::string cmdType = body.substr(p1 + key.size(), p2 - p1 - key.size());
                std::lock_guard<std::mutex> lk(m_mutex);
                m_responses[cmdType] = body;
                if (cmdType == "DeviceControl")
                {
                    m_ptz_ack_body = body;
                    m_got_ptz_ack = true;
                }
            }
        }
        if (body.find("Keepalive") != std::string::npos)
        {
            m_got_keepalive = true;
            printf("[平台] 收到设备心跳(%zu 字节)\n", body.size());
        }
        if (body.find("DeviceControl") != std::string::npos)
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_ptz_ack_body = body;
            m_got_ptz_ack = true;
            printf("[平台] 收到设备发出的 DeviceControl 应答:\n%s\n", body.c_str());
        }

        // 对设备的 MESSAGE 也得回 200 OK，否则设备会重传
        char resp[1024] = {0};
        snprintf(resp, sizeof(resp),
                 "SIP/2.0 200 OK\r\n"
                 "Via: %s\r\n"
                 "From: %s\r\n"
                 "To: %s;tag=platmsg\r\n"
                 "Call-ID: %s\r\n"
                 "CSeq: %s\r\n"
                 "Content-Length: 0\r\n"
                 "\r\n",
                 msg.header("via").c_str(), msg.header("from").c_str(),
                 msg.header("to").c_str(), msg.header("call-id").c_str(),
                 msg.header("cseq").c_str());

        std::lock_guard<std::mutex> lk(m_mutex);
        ::sendto(m_fd, resp, strlen(resp), 0, (sockaddr *)&m_device_addr, sizeof(m_device_addr));
    }

public:
    static constexpr const char *REALM = "3402000000";
    static constexpr const char *NONCE = "9f8a7b6c5d4e3f21";
    static constexpr const char *DEVICE_ID = "34020000001320000001";
    static constexpr const char *PASSWORD = "12345678";

    // 组装并发送一条 MESSAGE（调用方需持有 m_mutex）
    bool send_message_locked(const std::string &body, const std::string &callSuffix)
    {
        char msg[2048] = {0};
        const int n = snprintf(msg, sizeof(msg),
            "MESSAGE sip:%s@127.0.0.1:%d SIP/2.0\r\n"
            "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK-q-%u;rport\r\n"
            "From: <sip:34020000002000000001@3402000000>;tag=plat-%u\r\n"
            "To: <sip:%s@3402000000>\r\n"
            "Call-ID: q-%s@127.0.0.1\r\n"
            "CSeq: 1 MESSAGE\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Type: Application/MANSCDP+xml\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%s",
            FakePlatform::DEVICE_ID, m_device_port,
            m_port, m_seq,
            m_seq,
            FakePlatform::DEVICE_ID,
            callSuffix.c_str(),
            body.size(), body.c_str());
        ++m_seq;
        return ::sendto(m_fd, msg, (size_t)n, 0,
                        (sockaddr *)&m_device_addr, sizeof(m_device_addr)) == n;
    }

private:
    int m_fd{-1};
    int m_port{0};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_registered{false};
    std::atomic<bool> m_digest_ok{false};
    std::atomic<bool> m_got_message_ok{false};
    std::atomic<bool> m_got_ptz_ack{false};
    std::atomic<bool> m_got_keepalive{false};
    std::atomic<bool> m_invite_ok{false};
    std::atomic<bool> m_bye_ok{false};
    std::string m_inviteCallId;
    std::string m_inviteChannel;
    std::string m_inviteToTag;
    std::string m_inviteFromTag;
    std::string m_inviteContact;
    std::map<std::string, std::string> m_responses;   // CmdType → 设备应答原文
    std::vector<int> m_inviteSdpPorts;                // 各次 INVITE 应答 SDP 的媒体端口
    RtpReceiver m_rtp;
    std::string m_ptz_ack_body;
    mutable std::mutex m_mutex;
    sockaddr_in m_device_addr{};
    int m_device_port{0};
    bool m_device_addr_valid{false};
    unsigned int m_seq{100};
};

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    // 默认压掉 info 日志噪音；带 -v 参数时打开全部日志，便于排查
    bool verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "-v")
        {
            verbose = true;
        }
    }
    LogConfig cfg;
    cfg.level = verbose ? LogLevel::INFO : LogLevel::ERROR;
    LogManager::configure(cfg);

    FakePlatform platform;
    if (!platform.start("127.0.0.1", 15060))
    {
        printf("假平台启动失败\n");
        return 1;
    }

    gb28181_system_param sys{};
    strcpy(sys.sipserver_id, "34020000002000000001");
    strcpy(sys.sipserver_region, FakePlatform::REALM);
    strcpy(sys.sipserver_ip, "127.0.0.1");
    sys.sipserver_port = 15060;
    strcpy(sys.sipdev_reg_id, FakePlatform::DEVICE_ID);
    strcpy(sys.sipdev_password, FakePlatform::PASSWORD);
    sys.heartbeatInterval = 1;      // 1 秒心跳，便于自测观察
    sys.deadline = 3600;
    strcpy(sys.local_ip, "127.0.0.1");
    sys.sip_localport = 15061;
    sys.sip_transport = 0;          // UDP
    sys.channel_size = 1;
    strcpy(sys.channel_para[0].channel_id, "34020000001320000002");
    strcpy(sys.channel_para[0].channel_ip, "192.168.1.10");
    sys.channel_para[0].channel_index = 0;

    // 设备侧用门面类：内部完成 SIP 注册 + 指令分发（应答）+ 点播会话与推流
    Gb28181Device device;
    Gb28181Device::Config devCfg;
    devCfg.sys = sys;
    // 媒体端口池：平台收流端口用 17000，这里从 17001 起分配，避免同机冲突
    devCfg.media.portBase = 17001;
    devCfg.media.portCount = 4;
    devCfg.media.ps.codec = PsMuxer::Codec::H264;
    devCfg.media.ps.insertAud = true;

    static std::mutex g_state_mutex;
    static std::vector<std::pair<ptz_cmd_e, int>> g_ptz_cmds;   // 云台回调记录(动作, 速度)
    static std::vector<int> g_ptz_channels;

    device.set_ptz_handler([&](int channelIndex, ptz_cmd_e cmd, int speed, int presetNum) {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_ptz_cmds.emplace_back(cmd, speed);
        g_ptz_channels.push_back(channelIndex);
        (void)presetNum;
        return true;      // 执行成功 → 应答 Result=OK
    });

    if (device.init(devCfg) != 0)
    {
        printf("Gb28181Device::init 失败\n");
        platform.stop();
        return 1;
    }

    if (device.start() != 0)
    {
        printf("SipSession::start 失败\n");
        platform.stop();
        return 1;
    }

    // 1. 等注册完成（含 401 认证往返）
    for (int i = 0; i < 50 && !platform.registered(); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(platform.registered(), "SIP 注册成功(经 401 挑战 + 摘要认证)");
    CHECK(platform.digest_ok(), "认证摘要经平台校验通过");
    CHECK(device.is_registered(), "设备内部注册状态为已注册");

    // 2. 平台下发 PTZ 指令（左转，速度 0xFF）
    CHECK(platform.send_ptz_control(FakePlatform::DEVICE_ID, "2001", "A50103FF00000000"),
          "平台下发 DeviceControl/PTZCmd MESSAGE");

    for (int i = 0; i < 30 && !platform.got_ptz_ack(); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        // 分发器（Gb28181Device）把 DeviceControl 解析后回调到了 ptz_handler
        CHECK(!g_ptz_cmds.empty(), "云台指令已由设备分发到 ptz_handler");
        if (!g_ptz_cmds.empty())
        {
            CHECK(g_ptz_cmds[0].first == PTZ_CMD_LEFT, "PTZCmd 解析为左转");
            CHECK(g_ptz_cmds[0].second == 0xFF, "PTZ 速度解析为 0xFF");
        }
    }

    CHECK(platform.got_message_ok(), "设备对平台 MESSAGE 回了 200 OK");
    CHECK(platform.got_ptz_ack(), "设备回发了 DeviceControl 应答 MESSAGE");

    {
        std::string body = platform.ptz_ack_body();
        CHECK(body.find("<CmdType>DeviceControl</CmdType>") != std::string::npos, "应答含 DeviceControl");
        CHECK(body.find("<SN>2001</SN>") != std::string::npos, "应答 SN 与原请求一致");
        CHECK(body.find("<Result>OK</Result>") != std::string::npos, "应答 Result=OK");
    }

    // 2b. 查询类指令：设备应自动应答（分发器）
    CHECK(platform.send_query("Catalog", "34020000001320000001", "3001"), "平台查询设备目录(Catalog)");
    CHECK(platform.send_query("DeviceInfo", "34020000001320000001", "3002"), "平台查询设备信息(DeviceInfo)");
    CHECK(platform.send_query("DeviceStatus", "34020000001320000001", "3003"), "平台查询设备状态(DeviceStatus)");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    {
        const std::string catalog = platform.response("Catalog");
        printf("--- 设备回的 Catalog ---\n%s\n", catalog.c_str());
        CHECK(!catalog.empty(), "设备应答了 Catalog");
        CHECK(catalog.find("<SN>3001</SN>") != std::string::npos, "Catalog 应答 SN 与查询一致");
        CHECK(catalog.find("34020000001320000002") != std::string::npos, "Catalog 含通道国标ID");
        CHECK(catalog.find("<DeviceList Num=\"1\">") != std::string::npos ||
              catalog.find("<DeviceList") != std::string::npos, "Catalog 含 DeviceList");

        const std::string info = platform.response("DeviceInfo");
        CHECK(!info.empty(), "设备应答了 DeviceInfo");
        CHECK(info.find("<DeviceName>") != std::string::npos, "DeviceInfo 含设备名称");
        CHECK(info.find("<Channel>1</Channel>") != std::string::npos, "DeviceInfo 通道数正确");

        const std::string status = platform.response("DeviceStatus");
        CHECK(!status.empty(), "设备应答了 DeviceStatus");
        CHECK(status.find("<Online>ONLINE</Online>") != std::string::npos, "DeviceStatus 在线状态正确");
        CHECK(status.find("<Encode>ON</Encode>") != std::string::npos, "DeviceStatus 编码状态正确");
    }

    // 2c. 云台：变倍(zoom) 与 变焦(focus)，速度 0x32
    CHECK(platform.send_ptz_control("34020000001320000002", "3101", "A502013200000000"), "平台下发变倍+(ZOOM_ADD)");
    CHECK(platform.send_ptz_control("34020000001320000002", "3102", "A502023200000000"), "平台下发变倍-(ZOOM_REDUCE)");
    CHECK(platform.send_ptz_control("34020000001320000002", "3103", "A503013200000000"), "平台下发变焦+(FOCUS_ADD)");
    CHECK(platform.send_ptz_control("34020000001320000002", "3104", "A503023200000000"), "平台下发变焦-(FOCUS_REDUCE)");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        // 第 1 条是前面那条"左转"(目标写的是设备ID，不是通道 → 通道号应为 -1)
        CHECK(g_ptz_channels.size() >= 5, "云台回调被触发 5 次");
        CHECK(g_ptz_channels[0] == -1, "PTZ 目标为设备ID时通道号解析为 -1");
        CHECK(g_ptz_channels.size() >= 4 &&
              g_ptz_channels[g_ptz_channels.size() - 4] == 0 &&
              g_ptz_channels[g_ptz_channels.size() - 1] == 0,
              "PTZ 目标为通道ID时解析出通道号 0");

        auto has = [&](ptz_cmd_e cmd, int speed) {
            for (const auto &c : g_ptz_cmds)
            {
                if (c.first == cmd && c.second == speed)
                {
                    return true;
                }
            }
            return false;
        };
        CHECK(has(PTZ_CMD_ZOOM_ADD, 0x32), "变倍+ 回调正确(含速度 0x32)");
        CHECK(has(PTZ_CMD_ZOOM_REDUCE, 0x32), "变倍- 回调正确(含速度 0x32)");
        CHECK(has(PTZ_CMD_FOCUS_ADD, 0x32), "变焦+ 回调正确(含速度 0x32)");
        CHECK(has(PTZ_CMD_FOCUS_REDUCE, 0x32), "变焦- 回调正确(含速度 0x32)");
        CHECK(has(PTZ_CMD_LEFT, 0xFF), "方向(左转)回调正确");
    }

    // 3. 等心跳（heartbeatInterval=1s）
    for (int i = 0; i < 40 && !platform.got_keepalive(); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(platform.got_keepalive(), "设备周期性上报 Keepalive");

    // ======================= 4. 完整点播链路 =======================
    const std::string h264Path = (argc > 1) ? argv[1] : "build/test.h264";
    const auto frames = loadH264Frames(h264Path);
    if (frames.empty())
    {
        printf("提示: 未找到测试码流 %s，跳过点播推流验证\n"
               "  可生成：ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=25 "
               "-c:v libx264 -pix_fmt yuv420p -g 25 -slices 1 -f h264 %s\n",
               h264Path.c_str(), h264Path.c_str());
    }

    const int mediaPort = 17000;
    const int deviceSipPort = 15061;
    CHECK(platform.send_invite("34020000001320000002", deviceSipPort, mediaPort, 100000001u),
          "平台发起 INVITE 点播(带 SDP)");

    for (int i = 0; i < 50 && !platform.invite_ok(); i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(platform.invite_ok(), "设备应答 INVITE 200 OK(含 SDP)");

    CHECK(platform.send_ack(), "平台回 ACK，会话建立");

    for (int i = 0; i < 30 && device.session_count() == 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(device.session_count() == 1, "设备已按 SDP 建立媒体推流会话");

    {
        const auto snap = device.media().sessions();
        if (!snap.empty())
        {
            CHECK(snap[0].channelId == "34020000001320000002", "会话通道 ID 取自 INVITE 目标");
            CHECK(snap[0].media.port == mediaPort, "SDP 媒体端口解析正确");
            CHECK(snap[0].media.ip == "127.0.0.1", "SDP 媒体地址解析正确");
            CHECK(snap[0].media.ssrc == 100000001u, "SDP SSRC(y=) 解析正确");
            CHECK(!snap[0].media.tcp, "SDP 传输方式识别为 UDP");
        }
    }

    // 编码线程喂帧：只有点播该通道的会话会收到
    if (!frames.empty())
    {
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const std::uint64_t ptsUs = i * 1000000ull / 25ull;
            const int sent = device.pushFrame(0, frames[i].data.data(), frames[i].data.size(),
                                             ptsUs, frames[i].key);
            if (i == 0)
            {
                CHECK(sent == 1, "首帧推给了点播该通道的 1 个会话");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // 未点播的通道不应产生流量
        CHECK(device.pushFrame(1, frames[0].data.data(), frames[0].data.size(), 0, true) == 0,
              "未被点播的通道不发送数据");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    CHECK(platform.rtpPackets() > 0, "平台侧收到 RTP 媒体流");
    CHECK(platform.rtpBad() == 0, "收到的 RTP 包全部合法");
    CHECK(platform.rtpLastPt() == 96, "RTP 负载类型 = 96");
    CHECK(platform.rtpMarkers() == frames.size(), "marker 数量等于帧数");

    {
        const std::string psPath = "build/e2e_from_rtp.ps";
        const auto ps = platform.rtpPayload();
        std::ofstream out(psPath, std::ios::binary);
        out.write(reinterpret_cast<const char *>(ps.data()), static_cast<std::streamsize>(ps.size()));
        out.close();
        printf("平台侧收到 %zu 个 RTP 包, 还原 PS %zu 字节 → %s\n",
               platform.rtpPackets(), ps.size(), psPath.c_str());

        std::string cmd = "ffprobe -v error -f mpeg -count_frames -select_streams v:0 "
                          "-show_entries stream=codec_name,width,height,nb_read_frames "
                          "-of default=noprint_wrappers=1 " + psPath + " 2>&1";
        std::string probe;
        if (FILE *fp = popen(cmd.c_str(), "r"))
        {
            char line[256];
            while (fgets(line, sizeof(line), fp) != nullptr)
            {
                probe += line;
            }
            pclose(fp);
        }
        printf("--- ffprobe(平台侧收到的流) ---\n%s", probe.c_str());
        CHECK(probe.find("codec_name=h264") != std::string::npos, "ffprobe 能解码平台收到的流");
        CHECK(probe.find("width=640") != std::string::npos, "解码分辨率正确(640x360)");
    }

    // BYE 结束点播：设备应停流
    const std::size_t packetsBeforeBye = platform.rtpPackets();
    CHECK(platform.send_bye(), "平台发送 BYE");
    for (int i = 0; i < 30 && device.session_count() != 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(platform.bye_ok(), "设备应答 BYE 200 OK");
    CHECK(device.session_count() == 0, "BYE 后推流会话已释放");

    if (!frames.empty())
    {
        const std::size_t before = platform.rtpPackets();
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            device.pushFrame(0, frames[i].data.data(), frames[i].data.size(), 0, frames[i].key);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(platform.rtpPackets() == before, "BYE 之后不再发送媒体数据");
        (void)packetsBeforeBye;
    }

    // ======================= 5. 多路并发点播端口池 =======================
    CHECK(platform.send_invite("34020000001320000003", deviceSipPort, 17002, 100000002u),
          "平台发起第二路 INVITE(另一通道)");
    for (int i = 0; i < 50 && platform.inviteSdpPorts().size() < 2; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    {
        const auto ports = platform.inviteSdpPorts();
        printf("各次 INVITE 应答 SDP 的媒体端口: ");
        for (int p : ports)
        {
            printf("%d ", p);
        }
        printf("\n");
        CHECK(ports.size() >= 2, "两路 INVITE 都收到设备 SDP 应答");
        if (ports.size() >= 2)
        {
            CHECK(ports[0] != ports[1], "多路并发点播分配到互不相同的媒体端口(端口池生效)");
        }
    }

    device.stop();
    device.uninit();
    platform.stop();

    printf("\nRESULT: %s\n", g_fail == 0 ? "ALL PASS" : "HAS FAILURES");
    return g_fail == 0 ? 0 : 1;
}
