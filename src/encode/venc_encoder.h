#pragma once
#include"encode/video_encoder.h"
#include<chrono>

namespace encode
{
struct VencOptions
{
    int channel{0};
    void* context{nullptr}; // Existing aclrtContext, owned by the application's media runtime.
    std::chrono::milliseconds timeout{1000};
};
class VencEncoder final : public VideoEncoder
{
public:
    explicit VencEncoder(VencOptions options);
    ~VencEncoder() override;
    void start(const EncoderConfig& config) override;
    media::EncodedVideoFrame encode(const media::RawFrame& frame) override;
    void setBitrate(std::uint64_t bitsPerSecond) override;
    void requestKeyFrame() override;
    void stop() noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
EncoderFactory vencFactory(int firstChannel, void* context);
} // namespace encode
