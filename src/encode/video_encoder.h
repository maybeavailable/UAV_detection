#pragma once
#include"media/video_frame.h"
#include<functional>
#include<memory>

namespace encode
{
struct EncoderConfig
{
    media::VideoCodec codec{media::VideoCodec::H264};
    std::uint32_t width{1920}, height{1080}, fps{30}, gop{30};
    std::uint64_t minBitrateBps{30000}, startBitrateBps{2000000}, maxBitrateBps{5000000};
    std::uint32_t profile{0}; // H264 baseline default; H265 main=0.
    void validate() const;
};
// All calls, including start/stop, are serialized on one EncoderSession worker.
// encode returns owned bytes. It retains input pixels until hardware is finished.
class VideoEncoder
{
public:
    virtual ~VideoEncoder() = default;
    virtual void start(const EncoderConfig& config) = 0;
    virtual media::EncodedVideoFrame encode(const media::RawFrame& frame) = 0;
    virtual void setBitrate(std::uint64_t bitsPerSecond) = 0;
    virtual void requestKeyFrame() = 0;
    virtual void stop() noexcept = 0;
};
using EncoderFactory = std::function<std::unique_ptr<VideoEncoder>(media::Output)>;
} // namespace encode
