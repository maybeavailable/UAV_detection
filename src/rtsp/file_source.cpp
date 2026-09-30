#include"rtsp/file_source.h"

#include"log/log.h"

#include<chrono>
#include<fstream>
#include<iterator>
#include<utility>

FileRtpSource::FileRtpSource(std::string path, std::string filePath, double fps)
    : m_path_(std::move(path)),
      m_filePath_(std::move(filePath)),
      m_fps_(fps > 0 ? fps : 25.0)
{
    m_thread_ = std::thread(&FileRtpSource::loop, this);
}

FileRtpSource::~FileRtpSource()
{
    m_stop_.store(true);
    if (m_thread_.joinable())
        m_thread_.join();
}

std::string FileRtpSource::makeSdp(const std::string& host, int port) const
{
    (void)port;
    std::string sdp;
    sdp.reserve(384);
    sdp += "v=0\r\n";
    sdp += "o=- 0 1 IN IP4 " + host + "\r\n";
    sdp += "s=UAV detection stream\r\n";
    sdp += "i=" + m_path_ + "\r\n";
    sdp += "c=IN IP4 0.0.0.0\r\n";
    sdp += "t=0 0\r\n";
    sdp += "a=tool:UAV_detection/1.0\r\n";
    sdp += "a=type:broadcast\r\n";
    sdp += "a=control:*\r\n";
    sdp += "m=video 0 RTP/AVP 96\r\n";
    sdp += "b=AS:2048\r\n";
    sdp += "a=rtpmap:96 H264/90000\r\n";
    // 不写 fmtp/spop-parameter-sets：SPS/PPS 由文件内容经 RTP 内嵌下发，
    // 避免 SDP 里写死参数与真实码流不一致
    sdp += "a=control:track1\r\n";
    return sdp;
}

void FileRtpSource::loop()
{
    std::ifstream file(m_filePath_, std::ios::binary);
    if (!file)
    {
        UAV_LOG_ERROR << "FileRtpSource: cannot open h264 file: " << m_filePath_;
        return;
    }

    std::vector<std::uint8_t> all((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    if (all.empty())
    {
        UAV_LOG_ERROR << "FileRtpSource: empty h264 file: " << m_filePath_;
        return;
    }

    const auto nals = splitAnnexB(all.data(), all.size());
    if (nals.empty())
    {
        UAV_LOG_ERROR << "FileRtpSource: no NAL found in: " << m_filePath_;
        return;
    }

    // 缓存 SPS/PPS（带起启码），IDR 帧之前拼进去，保证客户端随时能解码
    std::vector<std::uint8_t> spsPps;
    for (const auto& nal : nals)
    {
        const int type = nal.data[0] & 0x1F;
        if (type == 7 || type == 8)
        {
            const std::uint8_t startCode[4] = {0, 0, 0, 1};
            spsPps.insert(spsPps.end(), startCode, startCode + 4);
            spsPps.insert(spsPps.end(), nal.data, nal.data + nal.size);
        }
    }

    const auto frameInterval = std::chrono::microseconds(
        static_cast<long long>(1000000.0 / m_fps_));
    auto nextFrameTime = std::chrono::steady_clock::now();
    std::uint64_t frameIndex = 0;

    UAV_LOG_INFO << "FileRtpSource started: path=" << m_path_
                 << " file=" << m_filePath_ << " fps=" << m_fps_;

    while (!m_stop_.load())
    {
        for (const auto& nal : nals)
        {
            if (m_stop_.load())
                break;

            const int type = nal.data[0] & 0x1F;
            if (type != 1 && type != 5)
                continue;                     // 只推切片；SPS/PPS/SEI 随 IDR 下发

            std::vector<std::uint8_t> frame;
            if (type == 5 && !spsPps.empty())
                frame = spsPps;               // IDR 前缀 SPS+PPS
            const std::uint8_t startCode[4] = {0, 0, 0, 1};
            frame.insert(frame.end(), startCode, startCode + 4);
            frame.insert(frame.end(), nal.data, nal.data + nal.size);

            const std::uint64_t ptsUs =
                frameIndex * 1000000u / static_cast<std::uint64_t>(m_fps_);
            pushFrame(frame.data(), frame.size(), ptsUs);
            ++frameIndex;

            // 节流：落后太多（如中途卡顿）则重置节拍，避免追帧风暴
            const auto now = std::chrono::steady_clock::now();
            if (now > nextFrameTime + frameInterval * 10)
                nextFrameTime = now;

            nextFrameTime += frameInterval;
            if (nextFrameTime > now)
                std::this_thread::sleep_until(nextFrameTime);
        }
    }
}
