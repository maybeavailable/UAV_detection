#pragma once
#include"media/video_frame.h"
#include<optional>

namespace media
{
// Per-WebRTC-sender RTP state. RTSP retains its existing RtpSource packetizer.
class H264Packetizer
{
public:
    struct Config { std::uint8_t payloadType{96}; std::uint32_t ssrc{0}; std::size_t maxPayloadBytes{1000}; };
    explicit H264Packetizer(Config config);
    std::vector<Bytes> packetize(const EncodedVideoFrame& frame);
    std::uint32_t ssrc() const { return config_.ssrc; }
private:
    Config config_; std::uint16_t sequence_{0}; std::uint32_t baseTimestamp_{0}, lastTimestamp_{0};
    std::optional<std::uint64_t> epoch_, basePts_;
};
} // namespace media
