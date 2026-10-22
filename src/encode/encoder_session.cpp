#include"encode/encoder_session.h"
#include"log/log.h"
#include<algorithm>
#include<atomic>
#include<condition_variable>
#include<deque>
#include<future>
#include<mutex>
#include<optional>
#include<stdexcept>
#include<thread>

namespace encode
{
namespace
{
// Sessions are recreated when MediaService restarts; never reuse an epoch in this process.
std::atomic<std::uint64_t> nextStreamEpoch{0};
}
void EncoderConfig::validate() const
{
    if ((codec != media::VideoCodec::H264 && codec != media::VideoCodec::H265) ||
        !width || !height || width > 8192 || height > 8192 || (width & 1) || (height & 1) ||
        !fps || fps > 120 || !gop || gop > 10000 || minBitrateBps < 1000 || maxBitrateBps > 1000000000 ||
        minBitrateBps > startBitrateBps || startBitrateBps > maxBitrateBps || profile > (codec == media::VideoCodec::H264 ? 2u : 0u))
        throw std::invalid_argument("invalid video encoder configuration");
}
struct EncoderSession::Impl
{
    Config config; std::unique_ptr<VideoEncoder> encoder; FrameHandler output;
    mutable std::mutex mutex; std::mutex lifecycle; std::condition_variable wake;
    std::thread worker; std::deque<media::RawFrame> pending;
    Snapshot stats; bool stopping{false}, keyRequested{true}, rateChanged{false};
    std::optional<std::uint64_t> inputEpoch, lastPts, nextPts;
    std::uint64_t streamEpoch{0}, configVersion{0}; media::SharedBytes parameterSets;
    std::optional<std::uint64_t> encodedSourceEpoch;
    std::uint64_t gopSequence{0}, frameInGop{0};
    Impl(Config c, std::unique_ptr<VideoEncoder> e, FrameHandler handler) : config(c), encoder(std::move(e)), output(std::move(handler))
    {
        config.encoder.validate(); (void)media::outputName(config.output);
        if (!encoder || !output || !config.maxPendingFrames || config.maxPendingFrames > 16 ||
            config.maxAccessUnitBytes < 1024 || config.maxAccessUnitBytes > 64 * 1024 * 1024 ||
            config.keyFrameInterval.count() < 0 || config.keyFrameInterval > std::chrono::seconds(10)) throw std::invalid_argument("invalid encoder session");
        stats.targetBitrateBps = config.encoder.startBitrateBps;
    }
    void run(std::promise<void> ready)
    {
        bool announced = false;
        try
        {
            encoder->start(config.encoder);
            { std::lock_guard<std::mutex> lock(mutex); stats.running = true; }
            announced = true; ready.set_value();
            auto lastKey = std::chrono::steady_clock::time_point::min();
            for (;;)
            {
                media::RawFrame frame; std::uint64_t rate; bool setRate = false, key = false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait(lock, [&] { return stopping || rateChanged || !pending.empty(); });
                    if (stopping) break;
                    rate = stats.targetBitrateBps; setRate = rateChanged; rateChanged = false;
                    if (!rate) { pending.clear(); continue; }
                    if (!pending.empty()) { frame = std::move(pending.front()); pending.pop_front(); }
                    const auto now = std::chrono::steady_clock::now();
                    if (frame && keyRequested && (lastKey == std::chrono::steady_clock::time_point::min() || now - lastKey >= config.keyFrameInterval))
                    { key = true; keyRequested = false; lastKey = now; ++stats.keyFrameRequests; }
                }
                if (setRate) encoder->setBitrate(rate);
                if (!frame) continue;
                if (!encodedSourceEpoch || *encodedSourceEpoch != frame->sourceEpoch)
                {
                    encodedSourceEpoch = frame->sourceEpoch; parameterSets.reset(); configVersion = 0; gopSequence = frameInGop = 0;
                    if (!key)
                    { std::lock_guard<std::mutex> lock(mutex); ++stats.keyFrameRequests; keyRequested = false; lastKey = std::chrono::steady_clock::now(); }
                    key = true;
                }
                if (key) encoder->requestKeyFrame();
                auto encoded = encoder->encode(frame);
                if (!encoded.data || encoded.data->empty() || encoded.data->size() > config.maxAccessUnitBytes)
                    throw std::runtime_error("encoder returned invalid access unit");
                auto sets = media::updateParameterSets(config.encoder.codec, *encoded.data, parameterSets);
                if (sets != parameterSets) { parameterSets = std::move(sets); ++configVersion; }
                encoded.output = config.output; encoded.codec = config.encoder.codec;
                encoded.sourceEpoch = frame->sourceEpoch; encoded.sourceSequence = frame->sequence;
                encoded.ptsUs = encoded.dtsUs = frame->ptsUs;
                encoded.keyFrame = media::isRandomAccess(encoded.codec, *encoded.data);
                if (encoded.keyFrame) { ++gopSequence; frameInGop = 0; }
                encoded.gopSequence = gopSequence; encoded.frameInGop = frameInGop++;
                encoded.parameterSets = parameterSets; encoded.configVersion = configVersion;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    encoded.epoch = streamEpoch;
                    if (stopping) break;
                    if (!inputEpoch || frame->sourceEpoch != *inputEpoch) continue;
                    if (!stats.targetBitrateBps) continue;
                    ++stats.encodedFrames; stats.encodedBytes += encoded.data->size();
                }
                output(std::make_shared<const media::EncodedVideoFrame>(std::move(encoded)));
            }
        }
        catch (const std::exception& e)
        {
            { std::lock_guard<std::mutex> lock(mutex); stats.failed = true; stats.error = e.what(); }
            UAV_LOG_ERROR << "[Encoder][" << media::outputName(config.output) << "] " << e.what();
            if (!announced) ready.set_exception(std::current_exception());
        }
        catch (...)
        {
            { std::lock_guard<std::mutex> lock(mutex); stats.failed = true; stats.error = "encoder worker exception"; }
            if (!announced) ready.set_exception(std::current_exception());
        }
        encoder->stop(); // Finish hardware use before releasing input leases, including failed sends.
        { std::lock_guard<std::mutex> lock(mutex); pending.clear(); stats.running = false; }
    }
};
EncoderSession::EncoderSession(Config c, std::unique_ptr<VideoEncoder> encoder, FrameHandler output)
    : impl_(std::make_unique<Impl>(c, std::move(encoder), std::move(output))) {}
EncoderSession::~EncoderSession() { stop(); }
void EncoderSession::start()
{
    auto& s = *impl_; std::lock_guard<std::mutex> operation(s.lifecycle);
    if (s.worker.joinable()) throw std::logic_error("encoder session already started");
    { std::lock_guard<std::mutex> lock(s.mutex); s.stopping = false; s.stats.failed = false; s.stats.error.clear();
        s.streamEpoch = ++nextStreamEpoch; s.keyRequested = true; s.rateChanged = true; s.inputEpoch.reset(); s.lastPts.reset(); s.nextPts.reset();
        s.encodedSourceEpoch.reset(); s.parameterSets.reset(); s.configVersion = 0; }
    std::promise<void> ready; auto result = ready.get_future(); s.worker = std::thread([&s, ready = std::move(ready)]() mutable { s.run(std::move(ready)); });
    try { result.get(); } catch (...) { s.worker.join(); throw; }
    UAV_LOG_INFO << "[Encoder][" << media::outputName(s.config.output) << "] started " << s.config.encoder.width << 'x' << s.config.encoder.height;
}
void EncoderSession::stop()
{
    if (!impl_) return;
    auto& s = *impl_; std::lock_guard<std::mutex> operation(s.lifecycle);
    { std::lock_guard<std::mutex> lock(s.mutex); s.stopping = true; } s.wake.notify_all();
    if (s.worker.joinable()) s.worker.join();
}
bool EncoderSession::submit(media::RawFrame frame)
{
    auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex);
    if (!frame || !frame->buffer || frame->width != s.config.encoder.width || frame->height != s.config.encoder.height ||
        !s.stats.running || s.stopping || !s.stats.targetBitrateBps) return false;
    if (!s.inputEpoch || *s.inputEpoch != frame->sourceEpoch)
    {
        if (s.inputEpoch && frame->sourceEpoch < *s.inputEpoch) return false;
        s.inputEpoch = frame->sourceEpoch; s.lastPts.reset(); s.nextPts.reset(); s.pending.clear(); s.keyRequested = true; s.streamEpoch = ++nextStreamEpoch;
    }
    if (s.lastPts && frame->ptsUs <= *s.lastPts) return false;
    if (s.nextPts && frame->ptsUs < *s.nextPts) { ++s.stats.droppedInputs; return false; }
    const auto interval = 1000000u / s.config.encoder.fps;
    // Preserve the sampling phase (e.g. 50 fps capture -> 30 fps), but reset it after a source gap.
    s.nextPts = !s.nextPts || frame->ptsUs - *s.nextPts >= interval ? frame->ptsUs + interval : *s.nextPts + interval;
    s.lastPts = frame->ptsUs;
    if (s.pending.size() >= s.config.maxPendingFrames) { s.pending.pop_front(); ++s.stats.droppedInputs; }
    s.pending.push_back(std::move(frame)); s.wake.notify_one(); return true;
}
void EncoderSession::setBitrate(std::uint64_t rate)
{
    auto& s = *impl_; std::lock_guard<std::mutex> lock(s.mutex);
    if (rate) rate = std::clamp(rate, s.config.encoder.minBitrateBps, s.config.encoder.maxBitrateBps);
    if (rate == s.stats.targetBitrateBps) return;
    if (!s.stats.targetBitrateBps && rate) s.keyRequested = true;
    s.stats.targetBitrateBps = rate; s.stats.paused = !rate; s.rateChanged = true; s.wake.notify_one();
}
void EncoderSession::requestKeyFrame() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->keyRequested = true; }
EncoderSession::Snapshot EncoderSession::snapshot() const
{ std::lock_guard<std::mutex> lock(impl_->mutex); auto result = impl_->stats; result.pendingFrames = impl_->pending.size(); return result; }
} // namespace encode
