#pragma once
#include"media/video_frame.h"
#include"hi_dvpp_venc.h"
#include<functional>

namespace encode
{
// Only platform adapters include this header. The shared owner must keep every
// input plane valid through get_stream, including timeout/stop paths.
struct AscendFrameBuffer final : media::FrameBuffer
{
    hi_video_frame_info frame{};
    std::shared_ptr<void> owner;
};
class AscendFramePool
{
public:
    struct Config
    {
        std::uint32_t width{1920}, height{1080}, device{0};
        std::size_t maxBuffers{8};
        void* context{nullptr};
    };
    struct Snapshot { std::size_t allocated{0}, inUse{0}; std::uint64_t dropped{0}; };
    explicit AscendFramePool(Config config);
    ~AscendFramePool();
    media::RawFrame tryCopy(const hi_video_frame_info& borrowed, std::uint64_t sourceEpoch, std::uint64_t sequence);
    Snapshot snapshot() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace encode
