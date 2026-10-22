#pragma once
#include"media/media_service.h"
#include"encode/ascend_video_frame.h"

namespace media
{
// Application assembly for one physical video source. Media runtime/context and
// capture remain owned by the application and must outlive this pipeline.
class AscendMediaPipeline
{
public:
    struct Config
    {
        MediaService::Config media;
        void* context{nullptr};
        int firstVencChannel{0};
        std::uint32_t device{0}; std::size_t maxRawBuffers{8};
        std::optional<std::uint16_t> rtspPort{8554}; // nullopt disables RTSP; zero chooses an ephemeral port.
        std::string rtspPath{"/live/rgb"};
    };
    explicit AscendMediaPipeline(Config config);
    ~AscendMediaPipeline();
    void start();
    void stop(); // Application lifecycle thread; stop capture submissions before destroying the pipeline.
    bool submitBorrowed(const hi_video_frame_info& frame, std::uint64_t sourceEpoch = 0) noexcept;
    MediaService& service(); // Application can attach VideoSender and MoQ VideoSource here.
    std::uint16_t rtspPort() const;
    encode::AscendFramePool::Snapshot framePoolSnapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace media
