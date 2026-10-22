#include"media/ascend_media_device.h"
#include"rtsp/live_source.h"
#include"rtsp/rtspserver.h"
#include"log/log.h"
#include<boost/asio/steady_timer.hpp>
#include<atomic>
#include<set>
#include<thread>

namespace media
{
struct AscendMediaDevice::Impl
{
    Config config; std::array<std::unique_ptr<AscendMediaPipeline>, 3> pipelines;
    boost::asio::io_context io; boost::asio::steady_timer timer{io};
    std::unique_ptr<RtspServer> rtsp; std::vector<std::shared_ptr<LiveRtpSource>> sources;
    std::thread network; std::atomic<bool> running{false};
    explicit Impl(Config c) : config(std::move(c))
    {
        if (!config.context || config.firstVencChannel < 0 || config.firstVencChannel > 247) throw std::invalid_argument("nine VENC channel slots and ACL context required");
        std::set<std::string> ids, paths;
        for (std::size_t i = 0; i < pipelines.size(); ++i)
        {
            const auto& source = config.sources[i];
            if (source.id.empty() || source.path.empty() || source.path[0] != '/' || !ids.insert(source.id).second || !paths.insert(source.path).second)
                throw std::invalid_argument("unique media source IDs/paths required");
            if (!source.enabled) continue;
            AscendMediaPipeline::Config pipeline; pipeline.context = config.context; pipeline.device = config.device;
            pipeline.firstVencChannel = config.firstVencChannel + static_cast<int>(3 * i); pipeline.rtspPort.reset();
            pipeline.media = MediaService::Config::forVideo(source.width, source.height, source.fps);
            pipelines[i] = std::make_unique<AscendMediaPipeline>(pipeline);
        }
    }
    void poll()
    {
        if (!running) return;
        for (const auto& source : sources) source->pump();
        timer.expires_after(std::chrono::milliseconds(5));
        timer.async_wait([this](auto ec) { if (!ec) poll(); });
    }
};
AscendMediaDevice::AscendMediaDevice(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
AscendMediaDevice::~AscendMediaDevice() { stop(); }
void AscendMediaDevice::start()
{
    auto& s = *impl_; if (s.running) return;
    try
    {
        s.io.restart();
        for (auto& pipeline : s.pipelines) if (pipeline) pipeline->start();
        s.rtsp = std::make_unique<RtspServer>(s.io, s.config.rtspPort);
        for (std::size_t i = 0; i < s.pipelines.size(); ++i) if (s.pipelines[i])
        {
            auto source = std::make_shared<LiveRtpSource>(s.config.sources[i].path, s.pipelines[i]->service().subscribe(Output::Rtsp));
            s.rtsp->registerSource(source); s.sources.push_back(std::move(source));
        }
        s.rtsp->start(); s.running = true; s.poll();
        s.network = std::thread([&s] { s.io.run(); });
        UAV_LOG_INFO << "[MediaDevice] live sources=" << s.sources.size() << " RTSP port=" << s.rtsp->port();
    }
    catch (...) { stop(); throw; }
}
void AscendMediaDevice::stop()
{
    auto& s = *impl_; s.running = false;
    if (s.network.joinable())
    {
        boost::asio::post(s.io, [&s] { s.timer.cancel(); if (s.rtsp) s.rtsp->stop(); }); s.network.join();
    }
    s.timer.cancel(); if (s.rtsp) s.rtsp->stop();
    s.io.restart(); s.io.poll(); s.io.stop(); s.sources.clear(); s.rtsp.reset();
    for (auto& pipeline : s.pipelines) if (pipeline) pipeline->stop();
}
bool AscendMediaDevice::submitBorrowed(std::size_t index, const hi_video_frame_info& frame, std::uint64_t epoch) noexcept
{ return index < impl_->pipelines.size() && impl_->pipelines[index] && impl_->pipelines[index]->submitBorrowed(frame, epoch); }
const AscendMediaDevice::Source& AscendMediaDevice::source(std::size_t index) const { return impl_->config.sources.at(index); }
MediaService& AscendMediaDevice::service(std::size_t index)
{
    const auto& pipeline = impl_->pipelines.at(index); if (!pipeline) throw std::logic_error("media source disabled"); return pipeline->service();
}
std::uint16_t AscendMediaDevice::rtspPort() const { return impl_->rtsp ? impl_->rtsp->port() : 0; }
}
