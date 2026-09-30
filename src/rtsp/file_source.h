#pragma once

#include"rtsp/rtp_source.h"

#include<string>
#include<thread>
#include<vector>

// 测试/占位媒体源：循环播放一个 H.264 Annex-B 文件。
// 用途：encode 模块就绪前用它验证整条 RTSP 拉流链路
//（DESCRIBE 的 SDP / SETUP 协商 / PLAY 后的 RTP 转发）。
// encode 模块就绪后：让真实源继承 RtpSource，产出 H.264 帧后调用 pushFrame 即可。
//
// 示例（ffmpeg 生成测试文件）：
//   ffmpeg -f lavfi -i testsrc=duration=3:size=640x360:rate=25 -c:v libx264 -pix_fmt yuv420p -f h264 test.h264
class FileRtpSource : public RtpSource
{
public:
    FileRtpSource(std::string path, std::string filePath, double fps = 25.0);
    ~FileRtpSource() override;

    std::string path() const override { return m_path_; }
    std::string makeSdp(const std::string& host, int port) const override;

private:
    void loop();

    std::string m_path_;
    std::string m_filePath_;
    double      m_fps_;

    std::thread       m_thread_;
    std::atomic<bool> m_stop_{false};
};
