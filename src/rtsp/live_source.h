#pragma once
#include"rtsp/rtp_source.h"
#include"media/media_service.h"

// One subscription feeds a shared RTSP source; client count does not create encoders.
class LiveRtpSource final : public RtpSource
{
public:
    LiveRtpSource(std::string path, std::shared_ptr<media::MediaSubscription> subscription);
    std::string path() const override { return path_; }
    std::string makeSdp(const std::string& host, int port) const override;
    void requestKeyFrame() override;
    std::size_t pump(std::size_t maxFrames = 8); // One producer/executor, typically an application timer.
private:
    std::string path_;
    std::shared_ptr<media::MediaSubscription> subscription_;
    std::optional<std::uint64_t> epoch_;
    std::uint64_t ptsOriginUs_{0}, ptsBaseUs_{0}, lastPtsUs_{0};
};
