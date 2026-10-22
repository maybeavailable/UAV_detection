#pragma once
#include"media/h264_packetizer.h"
#include"media/media_service.h"
#include"webrtc/peerconnection.h"

namespace webrtc
{
// Application-owned adapter. PeerConnection owns transport/DataChannel; MediaService owns encoders.
// Supply a Sender that forwards to PeerConnection::asyncSendRtp, preferably with a weak PeerConnection.
class VideoSender
{
public:
    using Sender = std::function<void(PeerConnection::Bytes, PeerConnection::SendHandler)>;
    struct Config { media::H264Packetizer::Config rtp; std::chrono::milliseconds pollInterval{5}; std::uint64_t overheadReserveBps{0}; };
    VideoSender(boost::asio::io_context& io, std::shared_ptr<media::MediaSubscription> subscription, Sender sender);
    VideoSender(boost::asio::io_context& io, std::shared_ptr<media::MediaSubscription> subscription, Sender sender, Config config);
    ~VideoSender();
    VideoSender(const VideoSender&) = delete;
    VideoSender& operator=(const VideoSender&) = delete;
    void bindCallbacks(PeerConnection::Callbacks& callbacks); // Chain existing callbacks before constructing PeerConnection.
    void close(); // Cancels polling and releases the single WebRTC video admission slot.
    std::uint32_t ssrc() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace webrtc
