#pragma once
#include"media/media_service.h"
#include"webrtc/peerconnection.h"

namespace webrtc
{
// Application media sessions. HTTP/authentication stay in Gateway; codecs stay in MediaService.
class VideoService
{
public:
    struct Config
    {
        encode::EncoderConfig encoder = media::MediaService::Config::forVideo(1920, 1080).encoders[1];
        PeerConnection::Config peer; // ICE bindings/STUN and transport options; media capabilities are filled here.
        std::chrono::milliseconds negotiationTimeout{8000}, connectionTimeout{20000}, disconnectedTimeout{15000};
    };
    struct Reply { unsigned status{500}; std::string sessionId, sdp, error; };
    using Handler = std::function<void(Reply)>;
    using Authorization = std::function<bool()>;
    VideoService(media::MediaService& media, Config config);
    ~VideoService();
    VideoService(const VideoService&) = delete;
    VideoService& operator=(const VideoService&) = delete;
    void start();
    void stop(); // Control thread; waits for PeerConnection, sockets and sender callbacks to close.
    void offer(std::string sdp, std::string credential, Authorization authorized, Handler done);
    void close(std::string sessionId, std::string credential, Handler done);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace webrtc
