#pragma once
#include"encode/encoder_session.h"
#include<array>
#include<memory>
#include<optional>

namespace media
{
struct NetworkBudget
{
    std::uint64_t generation{0}, bitrateBps{0}, overheadBps{0};
    double reduceRatio{0}; // Congestion-window frame reduction, 0..1. Zero bitrate pauses encoding.
};
class MediaSubscription
{
public:
    struct Snapshot
    {
        bool closed{false}, waitingForKeyFrame{true};
        std::size_t queuedFrames{0}, queuedBytes{0};
        std::uint64_t droppedFrames{0}, deliveredFrames{0};
    };
    ~MediaSubscription();
    MediaSubscription(const MediaSubscription&) = delete;
    MediaSubscription& operator=(const MediaSubscription&) = delete;
    EncodedFrame tryPop(); // Poll on the consumer's own executor; never runs network code on an encoder worker.
    bool updateBudget(NetworkBudget budget); // WebRTC/MoQ only; rejects feedback from an older generation.
    void requestKeyFrame();
    void resync(); // Discard queued dependencies after a partial network send, then wait for IDR.
    void close(); // Also called by destructor. Releases the WebRTC admission slot synchronously.
    Snapshot snapshot() const;
private:
    struct Impl;
    explicit MediaSubscription(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class MediaService;
};
class MediaService
{
public:
    struct Config
    {
        std::array<encode::EncoderConfig, 3> encoders; // Rtsp, WebRtc, Moq; one backend per output.
        std::size_t maxWebRtcSubscribers{1}; // Zero permits multiple shared subscribers; default is one.
        std::size_t maxSubscriberFrames{16}, maxSubscriberBytes{8 * 1024 * 1024};
        std::chrono::milliseconds maxSubscriberDelay{500};
        static Config forVideo(std::uint32_t width, std::uint32_t height, std::uint32_t fps = 30);
    };
    struct Snapshot
    {
        bool running{false};
        std::array<std::size_t, 3> subscribers{};
        std::array<encode::EncoderSession::Snapshot, 3> encoders;
    };
    MediaService(Config config, encode::EncoderFactory factory);
    ~MediaService();
    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;
    void start(); // Creates exactly three encoder instances, rolls all back if any fails.
    void stop(); // Closes subscriptions, then waits for all three hardware workers.
    std::size_t submit(RawFrame frame); // Fan out a shared owning raw frame to outputs with subscribers.
    std::shared_ptr<MediaSubscription> subscribe(Output output); // Throws resource_unavailable_try_again when WebRTC is occupied.
    Snapshot snapshot() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace media
