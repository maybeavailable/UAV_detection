#include"media/media_service.h"
#include"log/log.h"
#include<algorithm>
#include<cmath>
#include<deque>
#include<map>
#include<mutex>
#include<stdexcept>
#include<system_error>

namespace media
{
namespace
{
std::size_t index(Output output) { (void)outputName(output); return static_cast<std::size_t>(output); }
using Clock = std::chrono::steady_clock;
}
struct MediaSubscription::Impl
{
    mutable std::mutex mutex;
    struct Entry { EncodedFrame frame; Clock::time_point at; };
    std::deque<Entry> queue;
    Snapshot stats;
    std::size_t frameLimit, byteLimit; std::chrono::milliseconds delayLimit;
    std::function<void()> detach, request;
    std::function<bool(NetworkBudget)> budget;
    std::optional<std::uint64_t> epoch;
    std::optional<std::uint64_t> configVersion;
    Impl(std::size_t frames, std::size_t bytes, std::chrono::milliseconds delay) : frameLimit(frames), byteLimit(bytes), delayLimit(delay) {}
    void clear()
    { stats.droppedFrames += queue.size(); queue.clear(); stats.queuedBytes = 0; stats.waitingForKeyFrame = true; }
    bool push(const EncodedFrame& frame)
    {
        std::lock_guard<std::mutex> lock(mutex); if (stats.closed) return false;
        if (!epoch || *epoch != frame->epoch || !configVersion || *configVersion != frame->configVersion)
        { epoch = frame->epoch; configVersion = frame->configVersion; clear(); }
        const auto now = Clock::now(); const auto bytes = frame->data->size();
        const bool expired = !queue.empty() && now - queue.front().at >= delayLimit;
        if (expired || queue.size() >= frameLimit || bytes > byteLimit - stats.queuedBytes) clear();
        if (bytes > byteLimit) { ++stats.droppedFrames; return true; }
        if (stats.waitingForKeyFrame && (!frame->keyFrame || !completeParameterSets(frame->codec, frame->parameterSets)))
        { ++stats.droppedFrames; return true; }
        stats.waitingForKeyFrame = false;
        queue.push_back({frame, now}); stats.queuedBytes += bytes; return false;
    }
};
struct MediaService::Impl : std::enable_shared_from_this<Impl>
{
    struct Subscriber
    {
        Output output; std::weak_ptr<MediaSubscription::Impl> state;
        std::optional<NetworkBudget> budget;
    };
    Config config; encode::EncoderFactory factory;
    mutable std::mutex mutex; std::mutex lifecycle;
    bool running{false}; std::uint64_t nextId{0};
    std::map<std::uint64_t, Subscriber> subscribers;
    std::array<std::shared_ptr<encode::EncoderSession>, 3> sessions;
    Impl(Config c, encode::EncoderFactory f) : config(c), factory(std::move(f))
    {
        for (const auto& encoder : config.encoders) encoder.validate();
        if (config.encoders[index(Output::Rtsp)].codec != VideoCodec::H264 || config.encoders[index(Output::WebRtc)].codec != VideoCodec::H264)
            throw std::invalid_argument("RTSP/WebRTC media adapters currently require H264");
        if (!factory || !config.maxSubscriberFrames || config.maxSubscriberFrames > 4096 || config.maxSubscriberBytes < 1024 ||
            config.maxSubscriberBytes > 64 * 1024 * 1024 || config.maxSubscriberDelay.count() < 1 || config.maxSubscriberDelay > std::chrono::seconds(30))
            throw std::invalid_argument("invalid media service configuration");
    }
    void applyBudget(Output output) // Called with service mutex; setters only enqueue worker commands.
    {
        if (!sessions[index(output)] || output == Output::Rtsp) return;
        const auto& c = config.encoders[index(output)]; std::uint64_t rate = c.maxBitrateBps; bool found = false;
        for (const auto& s : subscribers) if (s.second.output == output && !s.second.state.expired())
        {
            const auto& b = s.second.budget;
            std::uint64_t value = c.startBitrateBps;
            if (b)
            {
                value = static_cast<std::uint64_t>(static_cast<long double>(b->bitrateBps) * (1.0 - b->reduceRatio));
                value = value > b->overheadBps ? value - b->overheadBps : 0;
            }
            rate = std::min(rate, value); found = true;
        }
        if (found && rate < c.minBitrateBps) rate = 0;
        sessions[index(output)]->setBitrate(found ? rate : c.startBitrateBps);
    }
    void remove(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex); const auto it = subscribers.find(id); if (it == subscribers.end()) return;
        const auto output = it->second.output; subscribers.erase(it); applyBudget(output);
    }
    bool budget(std::uint64_t id, NetworkBudget b)
    {
        if (b.bitrateBps > 1000000000 || !std::isfinite(b.reduceRatio) || b.reduceRatio < 0 || b.reduceRatio > 1) return false;
        std::lock_guard<std::mutex> lock(mutex); const auto it = subscribers.find(id);
        if (!running || it == subscribers.end() || it->second.output == Output::Rtsp ||
            (it->second.budget && b.generation < it->second.budget->generation)) return false;
        it->second.budget = b; applyBudget(it->second.output); return true;
    }
    void key(Output output)
    {
        std::shared_ptr<encode::EncoderSession> session;
        { std::lock_guard<std::mutex> lock(mutex); if (running) session = sessions[index(output)]; }
        if (session) session->requestKeyFrame();
    }
    void publish(const EncodedFrame& frame)
    {
        std::vector<std::shared_ptr<MediaSubscription::Impl>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex); if (!running) return;
            for (const auto& s : subscribers) if (s.second.output == frame->output)
                if (auto target = s.second.state.lock()) targets.push_back(std::move(target));
        }
        bool keyNeeded = false;
        for (const auto& target : targets) keyNeeded = target->push(frame) || keyNeeded;
        if (keyNeeded) key(frame->output);
    }
};
MediaService::Config MediaService::Config::forVideo(std::uint32_t width, std::uint32_t height, std::uint32_t fps)
{
    Config c; for (auto& e : c.encoders) { e.width = width; e.height = height; e.fps = fps; e.gop = fps; }
    c.encoders[index(Output::WebRtc)].startBitrateBps = 300000;
    c.encoders[index(Output::Moq)].startBitrateBps = 300000;
    return c;
}
MediaService::MediaService(Config c, encode::EncoderFactory factory) : impl_(std::make_shared<Impl>(c, std::move(factory))) {}
MediaService::~MediaService() { stop(); }
void MediaService::start()
{
    const auto s = impl_; std::lock_guard<std::mutex> operation(s->lifecycle);
    { std::lock_guard<std::mutex> lock(s->mutex); if (s->running) return; }
    std::array<std::shared_ptr<encode::EncoderSession>, 3> sessions;
    try
    {
        for (std::size_t i = 0; i < sessions.size(); ++i)
        {
            const auto output = static_cast<Output>(i); encode::EncoderSession::Config c; c.output = output; c.encoder = s->config.encoders[i];
            sessions[i] = std::make_shared<encode::EncoderSession>(c, s->factory(output), [weak = std::weak_ptr<Impl>(s)](auto frame) {
                if (auto state = weak.lock()) state->publish(frame);
            });
            sessions[i]->start();
        }
    }
    catch (...) { for (auto& session : sessions) if (session) session->stop(); throw; }
    { std::lock_guard<std::mutex> lock(s->mutex); s->sessions = std::move(sessions); s->running = true; }
    UAV_LOG_INFO << "[MediaService] started three independent encoders; WebRTC limit=" << s->config.maxWebRtcSubscribers << " RTSP/MoQ shared";
}
void MediaService::stop()
{
    if (!impl_) return;
    const auto s = impl_; std::lock_guard<std::mutex> operation(s->lifecycle);
    std::array<std::shared_ptr<encode::EncoderSession>, 3> sessions;
    {
        std::lock_guard<std::mutex> lock(s->mutex); s->running = false; sessions = std::move(s->sessions);
        for (const auto& sub : s->subscribers) if (auto state = sub.second.state.lock())
        { std::lock_guard<std::mutex> queueLock(state->mutex); state->stats.closed = true; state->clear(); }
        s->subscribers.clear();
    }
    for (const auto& session : sessions) if (session) session->stop();
}
std::size_t MediaService::submit(RawFrame frame)
{
    const auto s = impl_; std::array<std::shared_ptr<encode::EncoderSession>, 3> targets;
    {
        std::lock_guard<std::mutex> lock(s->mutex); if (!s->running) return 0;
        for (const auto& sub : s->subscribers) if (!sub.second.state.expired()) targets[index(sub.second.output)] = s->sessions[index(sub.second.output)];
    }
    std::size_t accepted = 0; for (const auto& session : targets) if (session && session->submit(frame)) ++accepted;
    return accepted;
}
std::shared_ptr<MediaSubscription> MediaService::subscribe(Output output)
{
    const auto i = index(output); const auto s = impl_; std::lock_guard<std::mutex> lock(s->mutex);
    if (!s->running) throw std::logic_error("media service is not running");
    if (output == Output::WebRtc && s->config.maxWebRtcSubscribers)
    {
        std::size_t count = 0; for (const auto& sub : s->subscribers) if (sub.second.output == output && !sub.second.state.expired()) ++count;
        if (count >= s->config.maxWebRtcSubscribers) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "WebRTC video slot occupied");
    }
    auto state = std::make_shared<MediaSubscription::Impl>(s->config.maxSubscriberFrames, s->config.maxSubscriberBytes, s->config.maxSubscriberDelay);
    const auto id = ++s->nextId; const auto weak = std::weak_ptr<Impl>(s);
    state->detach = [weak, id] { if (auto service = weak.lock()) service->remove(id); };
    state->budget = [weak, id](auto b) { if (auto service = weak.lock()) return service->budget(id, b); return false; };
    state->request = [weak, output] { if (auto service = weak.lock()) service->key(output); };
    s->subscribers.emplace(id, Impl::Subscriber{output, state, {}}); s->applyBudget(output); s->sessions[i]->requestKeyFrame();
    return std::shared_ptr<MediaSubscription>(new MediaSubscription(std::move(state)));
}
MediaService::Snapshot MediaService::snapshot() const
{
    const auto s = impl_; std::lock_guard<std::mutex> lock(s->mutex); Snapshot result; result.running = s->running;
    for (const auto& sub : s->subscribers) if (!sub.second.state.expired()) ++result.subscribers[index(sub.second.output)];
    for (std::size_t i = 0; i < s->sessions.size(); ++i) if (s->sessions[i]) result.encoders[i] = s->sessions[i]->snapshot();
    return result;
}
MediaSubscription::MediaSubscription(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
MediaSubscription::~MediaSubscription() { close(); }
void MediaSubscription::close()
{
    bool detach = false;
    { std::lock_guard<std::mutex> lock(impl_->mutex); if (!impl_->stats.closed) { impl_->stats.closed = true; impl_->clear(); detach = true; } }
    if (detach) impl_->detach();
}
EncodedFrame MediaSubscription::tryPop()
{
    EncodedFrame frame; bool request = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stats.closed || impl_->queue.empty()) return {};
        if (Clock::now() - impl_->queue.front().at >= impl_->delayLimit) { impl_->clear(); request = true; }
        else
        {
            frame = std::move(impl_->queue.front().frame); impl_->queue.pop_front(); impl_->stats.queuedBytes -= frame->data->size(); ++impl_->stats.deliveredFrames;
        }
    }
    if (request) impl_->request();
    return frame;
}
bool MediaSubscription::updateBudget(NetworkBudget budget)
{ { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->stats.closed) return false; } return impl_->budget(budget); }
void MediaSubscription::requestKeyFrame()
{ { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->stats.closed) return; } impl_->request(); }
void MediaSubscription::resync()
{ { std::lock_guard<std::mutex> lock(impl_->mutex); if (impl_->stats.closed) return; impl_->clear(); } impl_->request(); }
MediaSubscription::Snapshot MediaSubscription::snapshot() const
{ std::lock_guard<std::mutex> lock(impl_->mutex); auto result = impl_->stats; result.queuedFrames = impl_->queue.size(); return result; }
} // namespace media
