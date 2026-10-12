#include"rtsp/file_source.h"

#include"log/log.h"
#include"rtsp/h264_access_unit.h"
#include<cmath>
#include<stdexcept>

#include<chrono>
#include<fstream>
#include<iterator>
#include<utility>

FileRtpSource::FileRtpSource(std::string path, std::string filePath, double fps)
    : m_path_(std::move(path)),
      m_filePath_(std::move(filePath)),
      m_fps_(fps)
{
    if (!std::isfinite(fps) || fps < 1.0e-6 || fps > 1000000)
        throw std::invalid_argument("FileRtpSource fps must be finite and in [1e-6, 1e6]");
    m_thread_ = std::thread(&FileRtpSource::loop, this);
}

FileRtpSource::~FileRtpSource()
{
    { std::lock_guard<std::mutex> lock(m_sleepMutex_); m_stop_.store(true); }
    m_wake_.notify_all();
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
    // 参数集通过 RTP 内嵌下发，分片模式必须与 Single NAL/FU-A 一致。
    sdp += "a=fmtp:96 packetization-mode=1\r\n";
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

    try
    {
        const auto units = parseH264AccessUnits(all.data(), all.size());
        const auto interval = std::chrono::microseconds(static_cast<long long>(1000000.0L / m_fps_));
        auto epoch = std::chrono::steady_clock::now();
        std::uint64_t index = 0;
        UAV_LOG_INFO << "FileRtpSource started: path=" << m_path_
                     << " file=" << m_filePath_ << " fps=" << m_fps_;
        while (!m_stop_.load())
        {
            for (const auto& unit : units)
            {
                // PTS 和发送时刻来自同一个累计时间基，非整数帧率不会按帧累积截断误差。
                const auto pts = static_cast<std::uint64_t>(std::llround(index * 1000000.0L / m_fps_));
                auto deadline = epoch + std::chrono::microseconds(pts);
                const auto now = std::chrono::steady_clock::now();
                if (now > deadline + interval * 10)
                {
                    epoch = now - std::chrono::microseconds(pts);
                    deadline = now;
                }
                std::unique_lock<std::mutex> lock(m_sleepMutex_);
                if (m_wake_.wait_until(lock, deadline, [this] { return m_stop_.load(); })) return;
                lock.unlock();
                pushFrame(unit.data.data(), unit.data.size(), pts);
                ++index;
            }
        }
    }
    catch (const std::exception& e)
    {
        UAV_LOG_ERROR << "FileRtpSource: " << m_filePath_ << ": " << e.what();
    }
}
