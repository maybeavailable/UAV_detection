//=============================================================================
// GB28181 媒体面自测：H.264 → PS → RTP
//
// 验证内容：
//   1. PsMuxer 把 H.264 Annex-B 帧打成 PS：关键帧带 PS 包/系统头/PSM(含 CRC32)，
//      非关键帧只带 PES；PSM 里的 CRC32 用独立实现复算比对
//   2. PsRtpPusher 把 PS 打成 RTP 发出去：12 字节头、PT=96、90kHz 时间戳、
//      一帧最后一包 marker；序号连续
//   3. 本地 UDP 收包并按 RTP 顺序还原出 PS，要求与直接封装的结果**逐字节一致**；
//      落盘后用 ffprobe 解码，确认真能被解析出视频流
//
// 准备测试码流（若无 build/test.h264）：
//   ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=25
//          -c:v libx264 -pix_fmt yuv420p -g 25 -slices 1 -f h264 build/test.h264
//
// 编译运行：
//   g++ -std=c++17 -I src tools/gb28181_ps_rtp_selftest.cpp src/gb28181/ps_mux.cpp
//       src/gb28181/ps_rtp_pusher.cpp src/log/log.cpp src/log/RotateFileWriter.cpp
//       -lpthread -o build/gb28181_ps_rtp_selftest
//   ./build/gb28181_ps_rtp_selftest [h264文件] [输出目录]
//=============================================================================
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "gb28181/ps_mux.h"
#include "gb28181/ps_rtp_pusher.h"
#include "log/log.h"

static int g_fail = 0;
#define CHECK(cond, name)                              \
    do {                                               \
        if (cond) { printf("[PASS] %s\n", name); }     \
        else { printf("[FAIL] %s\n", name); g_fail++; } \
    } while (0)

// ---------------------------------------------------------------------------
// 独立的 MPEG-2 CRC32 实现（用于交叉校验 PsMuxer 写入 PSM 的 CRC）
// ---------------------------------------------------------------------------
static std::uint32_t refMpegCrc32(const std::uint8_t *data, std::size_t len)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int b = 0; b < 8; ++b)
        {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Annex-B 工具
// ---------------------------------------------------------------------------
struct NalSpan
{
    const std::uint8_t *data{nullptr};   // 指向起始码
    std::size_t size{0};                 // 含起始码
};

// 被测编码：H.264（GB28181-2016）与 H.265（GB28181-2022）
static bool g_isH265 = false;

static std::vector<NalSpan> splitNals(const std::vector<std::uint8_t> &buf)
{
    std::vector<NalSpan> out;
    std::size_t i = 0;
    std::size_t start = std::string::npos;
    while (i + 3 <= buf.size())
    {
        bool sc3 = (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1);
        bool sc4 = (i + 4 <= buf.size() && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1);
        if (sc3 || sc4)
        {
            if (start != std::string::npos)
            {
                out.push_back({buf.data() + start, i - start});
            }
            start = i;
            i += sc4 ? 4 : 3;
            continue;
        }
        ++i;
    }
    if (start != std::string::npos)
    {
        out.push_back({buf.data() + start, buf.size() - start});
    }
    return out;
}

// H.264：NAL 头低 5 位为类型；H.265：低 1 位 forbidden + 6 位类型
static int nalType(const NalSpan &nal)
{
    std::size_t off = 3;
    if (nal.size >= 4 && nal.data[2] == 0)
    {
        off = 4;
    }
    if (nal.size <= off)
    {
        return -1;
    }
    const std::uint8_t b = nal.data[off];
    return g_isH265 ? ((b >> 1) & 0x3F) : (b & 0x1F);
}

static bool isParameterSet(int type)
{
    return g_isH265 ? (type == 32 || type == 33 || type == 34)   // VPS/SPS/PPS
                    : (type == 7 || type == 8);                  // SPS/PPS
}

static bool isVcl(int type)
{
    return g_isH265 ? (type >= 0 && type <= 31) : (type == 1 || type == 5);
}

static bool isKeyFrameNal(int type)
{
    return g_isH265 ? (type >= 16 && type <= 21)                 // BLA/IDR/CRA
                    : (type == 5);                               // IDR
}

struct Frame
{
    std::vector<std::uint8_t> data;   // Annex-B，含起始码
    bool key{false};
};

// 把 Annex-B 裸流切成访问单元：每个 VCL NAL 收一帧，
// 其前面累积的参数集/SEI/AUD 归入该帧；关键帧前补参数集（便于平台中途接入）
static std::vector<Frame> splitFrames(const std::vector<std::uint8_t> &buf)
{
    const auto nals = splitNals(buf);

    std::vector<std::uint8_t> paramSets;
    for (const auto &nal : nals)
    {
        if (isParameterSet(nalType(nal)))
        {
            paramSets.insert(paramSets.end(), nal.data, nal.data + nal.size);
        }
    }

    std::vector<Frame> frames;
    std::vector<std::uint8_t> pending;
    for (const auto &nal : nals)
    {
        const int t = nalType(nal);
        if (isVcl(t))
        {
            Frame f;
            f.key = isKeyFrameNal(t);
            if (f.key)
            {
                f.data = paramSets;              // 关键帧前补参数集
            }
            f.data.insert(f.data.end(), pending.begin(), pending.end());
            f.data.insert(f.data.end(), nal.data, nal.data + nal.size);
            frames.push_back(std::move(f));
            pending.clear();
        }
        else
        {
            pending.insert(pending.end(), nal.data, nal.data + nal.size);
        }
    }
    return frames;
}

static bool writeFile(const std::string &path, const std::vector<std::uint8_t> &data)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    return f.good();
}

static std::string ffprobe(const std::string &path)
{
    // 用 ffprobe 解析 mpeg-ps，输出视频流的编码/分辨率/帧数
    std::string cmd = "ffprobe -v error -f mpeg -count_frames -select_streams v:0 "
                      "-show_entries stream=codec_name,width,height,nb_read_frames "
                      "-of default=noprint_wrappers=1 " + path + " 2>&1";
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (p == nullptr)
    {
        return "popen failed";
    }
    char line[512];
    while (fgets(line, sizeof(line), p) != nullptr)
    {
        out += line;
    }
    pclose(p);
    return out;
}

// ---------------------------------------------------------------------------
// RTP 接收器（模拟平台收流）
// ---------------------------------------------------------------------------
class RtpReceiver
{
public:
    bool start(std::uint16_t port)
    {
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
        {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(m_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            return false;
        }
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
    bool seqContinuous() const { return m_seqOk; }
    std::size_t badTimestampFrames() const { return m_badTs; }

private:
    void loop()
    {
        std::vector<std::uint8_t> buf(4096);
        std::uint16_t expectSeq = 0;
        bool firstPacket = true;
        std::uint32_t lastTs = 0;
        bool tsSeenInFrame = false;

        while (m_running.load())
        {
            const ssize_t n = ::recv(m_fd, buf.data(), buf.size(), 0);
            if (n <= 0)
            {
                continue;
            }

            if (n < 12 || (buf[0] >> 6) != 2)
            {
                ++m_bad;
                continue;
            }

            const std::uint8_t pt = buf[1] & 0x7F;
            const bool marker = (buf[1] & 0x80) != 0;
            const std::uint16_t seq = static_cast<std::uint16_t>((buf[2] << 8) | buf[3]);
            const std::uint32_t ts = (static_cast<std::uint32_t>(buf[4]) << 24) |
                                     (static_cast<std::uint32_t>(buf[5]) << 16) |
                                     (static_cast<std::uint32_t>(buf[6]) << 8) |
                                     static_cast<std::uint32_t>(buf[7]);
            m_lastPt = pt;
            m_lastSsrc = (static_cast<std::uint32_t>(buf[8]) << 24) |
                         (static_cast<std::uint32_t>(buf[9]) << 16) |
                         (static_cast<std::uint32_t>(buf[10]) << 8) |
                         static_cast<std::uint32_t>(buf[11]);

            if (!firstPacket && seq != static_cast<std::uint16_t>(expectSeq + 1))
            {
                m_seqOk = false;
            }
            expectSeq = seq;
            firstPacket = false;

            // 同一帧内所有分片时间戳必须相同，跨帧必须递增
            if (tsSeenInFrame && ts != lastTs)
            {
                ++m_badTs;
            }
            if (tsSeenInFrame && ts < lastTs)
            {
                ++m_badTs;
            }
            lastTs = ts;
            tsSeenInFrame = !marker;   // marker 表示一帧结束，下一包开始新帧
            if (marker)
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
    std::atomic<bool> m_running{true};
    mutable std::mutex m_mutex;
    std::vector<std::uint8_t> m_ps;
    std::size_t m_packets{0};
    std::size_t m_markers{0};
    std::size_t m_bad{0};
    bool m_seqOk{true};
    std::size_t m_badTs{0};

public:
    std::uint8_t m_lastPt{0};
    std::uint32_t m_lastSsrc{0};
};

// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    LogConfig cfg;
    cfg.level = LogLevel::ERROR;
    LogManager::configure(cfg);

    // 第三个参数选编码：h264（默认）/ h265
    g_isH265 = (argc > 3 && std::string(argv[3]) == "h265");
    const std::string codecName = g_isH265 ? "H.265" : "H.264";

    const std::string h264Path = (argc > 1) ? argv[1] : (g_isH265 ? "build/test.h265" : "build/test.h264");
    const std::string outDir = (argc > 2) ? argv[2] : "build";
    const std::string psOnlyPath = outDir + (g_isH265 ? "/ps_only_h265.ps" : "/ps_only.ps");
    const std::string psRtpPath = outDir + (g_isH265 ? "/ps_from_rtp_h265.ps" : "/ps_from_rtp.ps");
    const std::uint16_t rtpPort = g_isH265 ? 16001 : 16000;

    std::ifstream in(h264Path, std::ios::binary);
    if (!in)
    {
        printf("找不到测试码流 %s\n请先生成：\n"
               "  ffmpeg -y -f lavfi -i testsrc=duration=3:size=640x360:rate=25 -c:v %s "
               "-pix_fmt yuv420p -g 25 -slices 1 -f %s %s\n",
               h264Path.c_str(), g_isH265 ? "libx265" : "libx264",
               g_isH265 ? "hevc" : "h264", h264Path.c_str());
        return 1;
    }
    std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    const auto frames = splitFrames(raw);
    printf("测试码流: %s (%zu 字节, %zu 帧, %s)\n", h264Path.c_str(), raw.size(),
           frames.size(), codecName.c_str());
    CHECK(!frames.empty(), (codecName + " 码流解析出帧").c_str());

    // ---------------- 1. 纯 PS 封装 ----------------
    PsMuxer muxer;
    PsMuxer::Config psCfg;
    psCfg.codec = g_isH265 ? PsMuxer::Codec::H265 : PsMuxer::Codec::H264;
    psCfg.insertAud = true;
    muxer.setConfig(psCfg);

    std::vector<std::uint8_t> psAll;
    std::vector<std::size_t> frameOffsets;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        frameOffsets.push_back(psAll.size());
        const std::uint64_t ptsUs = i * 1000000ull / 25ull;   // 25fps
        if (muxer.packFrame(frames[i].data.data(), frames[i].data.size(), ptsUs, frames[i].key, psAll) <= 0)
        {
            CHECK(false, "PsMuxer::packFrame 失败");
            break;
        }
    }
    CHECK(!psAll.empty(), "PS 封装产出数据");
    CHECK(writeFile(psOnlyPath, psAll), ("写出 " + psOnlyPath).c_str());

    // 关键帧结构：PS 包(BA) + 系统头(BB) + PSM(BC) + PES(E0)
    const std::size_t k0 = frameOffsets[0];
    auto at = [&](std::size_t off) -> std::uint32_t {
        return (static_cast<std::uint32_t>(psAll[off]) << 24) |
               (static_cast<std::uint32_t>(psAll[off + 1]) << 16) |
               (static_cast<std::uint32_t>(psAll[off + 2]) << 8) |
               static_cast<std::uint32_t>(psAll[off + 3]);
    };
    CHECK(at(k0) == 0x000001BAu, "关键帧以 PS 包头(00 00 01 BA)开始");
    CHECK(at(k0 + 14) == 0x000001BBu, "PS 包头后紧跟系统头(00 00 01 BB)");
    CHECK(at(k0 + 29) == 0x000001BCu, "系统头后紧跟 PSM(00 00 01 BC)");
    CHECK(at(k0 + 49) == 0x000001E0u, "PSM 后是视频 PES(00 00 01 E0)");

    // PSM 内容：stream_type（H.264=0x1B / H.265=0x24）、elementary_stream_id=0xE0、CRC32 正确
    const std::uint8_t expectStreamType = g_isH265 ? 0x24 : 0x1B;
    CHECK(psAll[k0 + 29 + 12] == expectStreamType,
          g_isH265 ? "PSM 中 stream_type = 0x24(H.265)" : "PSM 中 stream_type = 0x1B(H.264)");
    CHECK(psAll[k0 + 29 + 13] == 0xE0, "PSM 中 elementary_stream_id = 0xE0");
    {
        const std::uint8_t *psm = psAll.data() + k0 + 29;
        const std::uint32_t crcInFile = (static_cast<std::uint32_t>(psm[16]) << 24) |
                                        (static_cast<std::uint32_t>(psm[17]) << 16) |
                                        (static_cast<std::uint32_t>(psm[18]) << 8) |
                                        static_cast<std::uint32_t>(psm[19]);
        const std::uint32_t crcCalc = refMpegCrc32(psm, 16);
        CHECK(crcInFile == crcCalc, "PSM 尾部 CRC32 与独立实现一致");
        CHECK(refMpegCrc32(reinterpret_cast<const std::uint8_t *>("123456789"), 9) == 0x0376E6E7u,
              "CRC32 实现符合 MPEG-2 标准校验值");
    }

    // 非关键帧：只带 PES
    std::size_t nonKeyIdx = frames.size();
    for (std::size_t i = 1; i < frames.size(); ++i)
    {
        if (!frames[i].key)
        {
            nonKeyIdx = i;
            break;
        }
    }
    if (nonKeyIdx < frames.size())
    {
        CHECK(at(frameOffsets[nonKeyIdx]) == 0x000001E0u, "非关键帧只带 PES(00 00 01 E0)");
    }

    // 第二个关键帧应重发 PS 头/系统头/PSM
    for (std::size_t i = 1; i < frames.size(); ++i)
    {
        if (frames[i].key)
        {
            CHECK(at(frameOffsets[i]) == 0x000001BAu, "后续关键帧重发 PS 包头");
            break;
        }
    }

    // ffprobe 验证纯 PS
    {
        const std::string probe = ffprobe(psOnlyPath);
        printf("--- ffprobe(纯 PS) ---\n%s", probe.c_str());
        CHECK(probe.find(g_isH265 ? "codec_name=hevc" : "codec_name=h264") != std::string::npos,
              (codecName + " 被 ffprobe 识别").c_str());
        CHECK(probe.find("width=640") != std::string::npos && probe.find("height=360") != std::string::npos,
              "ffprobe 解析出 640x360");
        CHECK(probe.find("nb_read_frames=N/A") == std::string::npos, "ffprobe 能数出帧数");
    }

    // ---------------- 2. PS → RTP → 本地收流 ----------------
    RtpReceiver receiver;
    CHECK(receiver.start(rtpPort), "本地 RTP 接收器启动");

    PsRtpPusher pusher;
    PsRtpPusher::Config rtpCfg;
    rtpCfg.dstIp = "127.0.0.1";
    rtpCfg.dstPort = rtpPort;
    rtpCfg.ssrc = 123456789;
    rtpCfg.payloadType = 96;
    rtpCfg.mtu = 1400;
    rtpCfg.tcp = false;
    rtpCfg.ps = psCfg;
    CHECK(pusher.open(rtpCfg) == 0, "PsRtpPusher 打开 UDP 通道");

    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const std::uint64_t ptsUs = i * 1000000ull / 25ull;
        if (pusher.pushFrame(frames[i].data.data(), frames[i].data.size(), ptsUs, frames[i].key) <= 0)
        {
            CHECK(false, "pushFrame 失败");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));   // 避免 UDP 缓冲溢出
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    receiver.stop();
    pusher.close();

    const auto psFromRtp = receiver.payload();
    printf("RTP 收包: %zu 包, marker %zu 个, 还原 PS %zu 字节\n",
           receiver.packetCount(), receiver.markerCount(), psFromRtp.size());

    CHECK(receiver.badPackets() == 0, "收到的 RTP 包全部合法");
    CHECK(receiver.m_lastPt == 96, "RTP 负载类型 = 96");
    CHECK(receiver.m_lastSsrc == 123456789u, "RTP SSRC 与配置一致");
    CHECK(receiver.seqContinuous(), "RTP 序号连续");
    CHECK(receiver.badTimestampFrames() == 0, "同帧分片时间戳一致且跨帧递增");
    CHECK(receiver.markerCount() == frames.size(), "marker 数量等于帧数(每帧最后一包置位)");
    CHECK(psFromRtp == psAll, "RTP 还原出的 PS 与直接封装结果逐字节一致");
    CHECK(writeFile(psRtpPath, psFromRtp), ("写出 " + psRtpPath).c_str());

    // RTP 包数 = ceil(PS 总长 / (mtu-12))
    {
        const std::size_t maxPayload = rtpCfg.mtu - 12;
        std::size_t expect = 0;
        std::size_t off = 0;
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const std::size_t end = (i + 1 < frameOffsets.size()) ? frameOffsets[i + 1] : psAll.size();
            std::size_t len = end - frameOffsets[i];
            off = 0;
            while (off < len)
            {
                const std::size_t n = std::min(maxPayload, len - off);
                ++expect;
                off += n;
            }
        }
        printf("期望 RTP 包数 %zu, 实收 %zu\n", expect, receiver.packetCount());
        CHECK(receiver.packetCount() == expect, "RTP 分片数量与 MTU 计算一致");
    }

    // ffprobe 验证 RTP 还原后的 PS
    {
        const std::string probe = ffprobe(psRtpPath);
        printf("--- ffprobe(RTP 还原的 PS) ---\n%s", probe.c_str());
        CHECK(probe.find(g_isH265 ? "codec_name=hevc" : "codec_name=h264") != std::string::npos,
              "ffprobe 能解码 RTP 还原的 PS");
    }

    // ---------------- 3. TCP 传输（GB28181-2022 要求支持）----------------
    {
        const std::uint16_t tcpPort = g_isH265 ? 16003 : 16002;

        // 平台侧：TCP 监听，收 RTP 直发流
        int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(tcpPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int reuse = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        CHECK(::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0,
              "TCP 监听端口就绪");
        ::listen(listenFd, 1);

        PsRtpPusher tcpPusher;
        PsRtpPusher::Config tcpCfg = rtpCfg;
        tcpCfg.tcp = true;
        // TCP 是字节流且 RTP 头无长度字段，用 RFC4571 的 2 字节长度前缀才能精确定界，
        // 这里以此模式验证；Raw 模式（多数国标设备的裸发方式）另外验证连通性
        tcpCfg.tcpFraming = PsRtpPusher::TcpFraming::Rfc4571;
        tcpCfg.dstPort = tcpPort;
        CHECK(tcpPusher.open(tcpCfg) == 0, "PsRtpPusher 打开 TCP 通道(RFC4571 定界)");

        int connFd = ::accept(listenFd, nullptr, nullptr);
        CHECK(connFd >= 0, "平台侧 TCP 连接建立");

        std::vector<std::uint8_t> tcpPs;
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const std::uint64_t ptsUs = i * 1000000ull / 25ull;
            tcpPusher.pushFrame(frames[i].data.data(), frames[i].data.size(), ptsUs, frames[i].key);
        }
        tcpPusher.close();

        std::vector<std::uint8_t> buf(8192);
        ssize_t n = 0;
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 300 * 1000;
        ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while ((n = ::recv(connFd, buf.data(), buf.size(), 0)) > 0)
        {
            tcpPs.insert(tcpPs.end(), buf.begin(), buf.begin() + n);
        }
        ::close(connFd);
        ::close(listenFd);

        // 按 RFC4571 的 2 字节长度前缀解出每个 RTP 包，再去掉 12 字节头得到 PS
        std::vector<std::uint8_t> tcpPayload;
        std::size_t off = 0;
        std::size_t pkts = 0;
        std::size_t tcpMarkers = 0;
        while (off + 2 <= tcpPs.size())
        {
            const std::size_t pktLen = (static_cast<std::size_t>(tcpPs[off]) << 8) | tcpPs[off + 1];
            if (pktLen < 12 || off + 2 + pktLen > tcpPs.size())
            {
                break;
            }
            const std::uint8_t *rtp = tcpPs.data() + off + 2;
            if ((rtp[0] >> 6) != 2 || (rtp[1] & 0x7F) != 96)
            {
                break;
            }
            if (rtp[1] & 0x80)
            {
                ++tcpMarkers;
            }
            tcpPayload.insert(tcpPayload.end(), rtp + 12, rtp + pktLen);
            off += 2 + pktLen;
            ++pkts;
        }
        printf("TCP 收到 %zu 字节 / %zu 个 RTP 包, 还原 PS %zu 字节\n", tcpPs.size(), pkts, tcpPayload.size());
        CHECK(!tcpPs.empty(), "TCP 通道收到媒体数据");
        CHECK(tcpMarkers == frames.size(), "TCP 上 marker 数量等于帧数");
        CHECK(tcpPayload == psAll, "TCP 还原出的 PS 与直接封装结果逐字节一致");

        const std::string tcpPsPath = outDir + (g_isH265 ? "/ps_tcp_h265.ps" : "/ps_tcp.ps");
        CHECK(writeFile(tcpPsPath, tcpPayload), ("写出 " + tcpPsPath).c_str());
        const std::string probe = ffprobe(tcpPsPath);
        CHECK(probe.find(g_isH265 ? "codec_name=hevc" : "codec_name=h264") != std::string::npos,
              "ffprobe 能解码 TCP 通道还原的 PS");
    }

    // ---------------- 4. TCP 裸发模式（Raw）连通行验证 ----------------
    {
        const std::uint16_t rawPort = g_isH265 ? 16005 : 16004;
        int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(rawPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int reuse = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        ::bind(listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        ::listen(listenFd, 1);

        PsRtpPusher rawPusher;
        PsRtpPusher::Config rawCfg = rtpCfg;
        rawCfg.tcp = true;
        rawCfg.tcpFraming = PsRtpPusher::TcpFraming::Raw;
        rawCfg.dstPort = rawPort;
        CHECK(rawPusher.open(rawCfg) == 0, "PsRtpPusher 打开 TCP 通道(Raw 裸发)");
        int connFd = ::accept(listenFd, nullptr, nullptr);

        rawPusher.pushFrame(frames[0].data.data(), frames[0].data.size(), 0, frames[0].key);
        rawPusher.close();

        std::vector<std::uint8_t> head(256, 0);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 300 * 1000;
        ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const ssize_t got = ::recv(connFd, head.data(), head.size(), 0);
        ::close(connFd);
        ::close(listenFd);

        CHECK(got > 12, "Raw 模式收到数据");
        CHECK((head[0] >> 6) == 2 && (head[1] & 0x7F) == 96, "Raw 模式首字节即 RTP 头(PT=96)");
        // RTP 头之后应直接是 PS：关键帧以 00 00 01 BA 开头
        CHECK(head[12] == 0x00 && head[13] == 0x00 && head[14] == 0x01 && head[15] == 0xBA,
              "Raw 模式 RTP 头后紧跟 PS 包头(00 00 01 BA)");
    }

    printf("\nRESULT: %s\n", g_fail == 0 ? "ALL PASS" : "HAS FAILURES");
    return g_fail == 0 ? 0 : 1;
}
