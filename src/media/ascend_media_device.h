#pragma once
#include"media/ascend_media_pipeline.h"
#include<array>

namespace media
{
// Three independent VPSS outputs, each with its own RTSP/WebRTC/MoQ encoder trio.
class AscendMediaDevice
{
public:
    struct Source
    {
        std::string id, deviceId, path;
        std::uint32_t width, height, fps{30};
        bool enabled{true};
    };
    struct Config
    {
        void* context{nullptr}; std::uint32_t device{0};
        int firstVencChannel{0}; std::uint16_t rtspPort{8554};
        std::array<Source, 3> sources{{
            {"rgb.main", "rgb", "/live/rgb", 2566, 1520},
            {"rgb.sub", "rgb", "/live/rgb/sub", 1920, 1080},
            {"thermal.main", "thermal", "/live/thermal", 640, 512}}};
    };
    explicit AscendMediaDevice(Config config);
    ~AscendMediaDevice();
    void start();
    void stop();
    bool submitBorrowed(std::size_t source, const hi_video_frame_info& frame, std::uint64_t epoch = 0) noexcept;
    const Source& source(std::size_t index) const;
    MediaService& service(std::size_t index);
    std::uint16_t rtspPort() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
