#pragma once
#include"encode/video_encoder.h"
#include<chrono>
#include<functional>
#include<memory>
#include<string>

namespace encode
{
class EncoderSession
{
public:
    struct Config
    {
        media::Output output{media::Output::Rtsp};
        EncoderConfig encoder;
        std::size_t maxPendingFrames{2}, maxAccessUnitBytes{8 * 1024 * 1024};
        std::chrono::milliseconds keyFrameInterval{300}; // Coalesce requests; initial frame is requested immediately.
    };
    struct Snapshot
    {
        bool running{false}, failed{false}, paused{false};
        std::uint64_t targetBitrateBps{0}, encodedFrames{0}, encodedBytes{0}, droppedInputs{0}, keyFrameRequests{0};
        std::size_t pendingFrames{0};
        std::string error;
    };
    using FrameHandler = std::function<void(media::EncodedFrame)>;
    EncoderSession(Config config, std::unique_ptr<VideoEncoder> encoder, FrameHandler output);
    ~EncoderSession();
    EncoderSession(const EncoderSession&) = delete;
    EncoderSession& operator=(const EncoderSession&) = delete;
    void start(); // Waits for backend startup; throws on failure, rolls back acquired hardware.
    void stop(); // Waits for worker/input leases; must not be called from the output callback.
    bool submit(media::RawFrame frame); // Non-blocking; keeps a bounded queue of the latest raw frames.
    void setBitrate(std::uint64_t bitsPerSecond); // Zero pauses before encoding; positive values clamp to config.
    void requestKeyFrame();
    Snapshot snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace encode
