#include"media/ascend_media_pipeline.h"
#include"encode/venc_encoder.h"
#include"rtsp/live_source.h"
#include"rtsp/rtspserver.h"
#include"log/log.h"
#include<boost/asio/steady_timer.hpp>
#include<atomic>
#include<algorithm>
#include<thread>

namespace media
{
struct AscendMediaPipeline::Impl
{
    struct Input { std::uint32_t width, height; encode::AscendFramePool pool; };
    Config config; MediaService media; std::vector<Input> inputs;
    boost::asio::io_context io; boost::asio::steady_timer timer{io};
    std::shared_ptr<LiveRtpSource> source; std::unique_ptr<RtspServer> rtsp; std::thread network;
    std::atomic<bool> running{false}; std::atomic<std::uint64_t> sequence{0}, errors{0}; std::uint16_t port{0};
    explicit Impl(Config c) : config(c), media(c.media, encode::vencFactory(c.firstVencChannel, c.context))
    {
        for (const auto& e : c.media.encoders)
            if (std::none_of(inputs.begin(), inputs.end(), [&](const auto& input) { return input.width == e.width && input.height == e.height; }))
                inputs.push_back({e.width, e.height, encode::AscendFramePool({e.width, e.height, c.device, c.maxRawBuffers, c.context})});
    }
    void poll()
    {
        if (!running.load()) return;
        if (source) source->pump();
        timer.expires_after(std::chrono::milliseconds(5));
        timer.async_wait([this](auto ec) { if (!ec) poll(); });
    }
};
AscendMediaPipeline::AscendMediaPipeline(Config c) : impl_(std::make_unique<Impl>(c)) {}
AscendMediaPipeline::~AscendMediaPipeline() { stop(); }
void AscendMediaPipeline::start()
{
    auto& s = *impl_; if (s.running.load()) return;
    try
    {
        s.media.start(); s.io.restart();
        if (s.config.rtspPort)
        {
            s.source = std::make_shared<LiveRtpSource>(s.config.rtspPath, s.media.subscribe(Output::Rtsp));
            s.rtsp = std::make_unique<RtspServer>(s.io, *s.config.rtspPort); s.rtsp->registerSource(s.source); s.rtsp->start(); s.port = s.rtsp->port();
        }
        s.running.store(true);
        if (s.rtsp)
        {
            s.poll(); s.network = std::thread([&s] {
                try { s.io.run(); }
                catch (const std::exception& e) { UAV_LOG_ERROR << "[MediaPipeline] network loop failed: " << e.what(); }
            });
            UAV_LOG_INFO << "[MediaPipeline] RTSP ready port=" << s.port << " path=" << s.config.rtspPath;
        }
    }
    catch (...) { stop(); throw; }
}
void AscendMediaPipeline::stop()
{
    auto& s = *impl_; s.running.store(false);
    if (s.network.joinable())
    {
        boost::asio::post(s.io, [&s] { s.timer.cancel(); if (s.rtsp) s.rtsp->stop(); });
        // stop() prevents new submissions; network callbacks use only owned source/subscription data.
        s.network.join();
    }
    s.timer.cancel(); if (s.rtsp) s.rtsp->stop();
    s.io.restart(); s.io.poll(); s.io.stop(); // Drain canceled callbacks before a possible restart.
    s.source.reset(); s.rtsp.reset(); s.media.stop(); s.port = 0;
}
bool AscendMediaPipeline::submitBorrowed(const hi_video_frame_info& frame, std::uint64_t epoch) noexcept
{
    auto& s = *impl_; if (!s.running.load()) return false;
    try
    {
        const auto input = std::find_if(s.inputs.begin(), s.inputs.end(), [&](const auto& value) { return value.width == frame.v_frame.width && value.height == frame.v_frame.height; });
        if (input == s.inputs.end()) return false;
        auto owned = input->pool.tryCopy(frame, epoch, s.sequence.fetch_add(1)); return owned && s.media.submit(std::move(owned));
    }
    catch (const std::exception& e)
    {
        const auto count = s.errors.fetch_add(1) + 1;
        UAV_LOG_IF(LogLevel::WARN, (count & (count - 1)) == 0) << "[MediaPipeline] raw frame rejected: " << e.what() << " count=" << count;
        return false;
    }
}
MediaService& AscendMediaPipeline::service() { return impl_->media; }
std::uint16_t AscendMediaPipeline::rtspPort() const { return impl_->port; }
encode::AscendFramePool::Snapshot AscendMediaPipeline::framePoolSnapshot() const
{
    encode::AscendFramePool::Snapshot result;
    for (const auto& input : impl_->inputs) { const auto s = input.pool.snapshot(); result.allocated += s.allocated; result.inUse += s.inUse; result.dropped += s.dropped; }
    return result;
}
} // namespace media
