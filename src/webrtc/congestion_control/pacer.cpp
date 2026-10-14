#include"webrtc/congestion_control/pacer.h"
#include"webrtc/congestion_control/interval_budget.h"

#include<algorithm>
#include<array>
#include<deque>
#include<map>
#include<set>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace
{
constexpr auto MaxProcessingInterval = std::chrono::milliseconds(30);
using Microseconds = std::chrono::microseconds;
}
bool Pacer::validRate(std::uint64_t rate) { return rate == 0 || (rate >= 1000 && rate <= IntervalBudget::MaxRateBps); }
bool Pacer::Config::validate(std::string* reason) const
{
    if (!Pacer::validRate(rateBps) || !Pacer::validRate(paddingRateBps) ||
        processInterval.count() < 1 || processInterval > MaxProcessingInterval ||
        queueTimeLimit.count() < 1 || queueTimeLimit.count() > 60000 ||
        maxQueueDelay.count() < 1 || maxQueueDelay.count() > 60000 || !maxBurstBytes || maxBurstBytes > 64 * 1024 ||
        !maxQueuePackets || maxQueuePackets > 32768 || !maxQueueBytes || maxQueueBytes > 64 * 1024 * 1024 ||
        !maxStreams || maxStreams > 4096 || transportOverheadBytes > 512)
    { if (reason) *reason = "invalid pacer rate, queue or process limits"; return false; }
    return true;
}
struct Pacer::Impl
{
    // Preserve original RTP FIFO even if priority changes. Retransmissions and
    // padding have separate queues; disabled padding must never block new media.
    enum class Kind { Original, Retransmission, Padding };
    using Key = std::pair<std::uint32_t, Kind>;
    struct Entry { Packet packet; Time queuedAt; Microseconds activeQueuedAt; };
    Config config; Snapshot stats;
    std::map<Key, std::deque<Entry>> streams;
    std::array<std::deque<Key>, 4> ready;
    std::set<std::uint64_t> ids;
    IntervalBudget mediaBudget, paddingBudget;
    Time lastBudgetAt, nextProcessAt, queueUpdatedAt;
    Microseconds activeTime{0}, totalQueueTime{0};
    std::size_t mediaQueuedBytes{0}, paddingQueuedBytes{0}, processBytes{0};
    bool firstProcess{true};

    Impl(Config c, Time now) : config(std::move(c)),
        mediaBudget(config.rateBps), paddingBudget(std::min(config.rateBps, config.paddingRateBps)),
        lastBudgetAt(now), nextProcessAt(now), queueUpdatedAt(now)
    {
        if (!config.validate()) throw std::invalid_argument("invalid pacer configuration");
        stats.rateBps = stats.adjustedRateBps = config.rateBps;
        stats.paddingRateBps = paddingBudget.targetRateBps();
    }
    std::size_t cost(const Packet& p) const { return p.wireSize + config.transportOverheadBytes; }
    bool suspended() const { return stats.paused || !stats.rateBps; }
    void updateQueueTime(Time now)
    {
        if (now <= queueUpdatedAt) return;
        const auto elapsed = std::chrono::duration_cast<Microseconds>(now - queueUpdatedAt);
        if (!suspended()) { activeTime += elapsed; totalQueueTime += elapsed * stats.queuedPackets; }
        queueUpdatedAt = now;
    }
    Microseconds averageQueueTime(Time now) const
    {
        if (!stats.queuedPackets) return Microseconds(0);
        const auto elapsed = !suspended() && now > queueUpdatedAt ?
            std::chrono::duration_cast<Microseconds>(now - queueUpdatedAt) : Microseconds(0);
        return totalQueueTime / stats.queuedPackets + elapsed;
    }
    void discardCredit() { mediaBudget.discardPositive(); paddingBudget.discardPositive(); }
    void settle(Time now, bool idle)
    {
        if (now > lastBudgetAt)
        {
            const auto elapsed = std::chrono::duration_cast<Microseconds>(now - lastBudgetAt);
            const auto cap = idle ? Microseconds(IntervalBudget::Window) : Microseconds(MaxProcessingInterval);
            mediaBudget.increaseBudget(std::min(elapsed, cap));
            paddingBudget.increaseBudget(std::min(elapsed, cap));
            lastBudgetAt = now;
        }
        discardCredit();
    }
    void adjustMediaRate(Time now)
    {
        auto rate = stats.rateBps;
        if (rate && config.drainLargeQueues && mediaQueuedBytes)
        {
            const auto left = std::max(Microseconds(1000), Microseconds(config.queueTimeLimit) - averageQueueTime(now));
            const auto numerator = std::uint64_t(mediaQueuedBytes) * 8000000;
            const auto required = (numerator + left.count() - 1) / left.count();
            rate = std::max(rate, std::min(required, IntervalBudget::MaxRateBps));
        }
        stats.adjustedRateBps = rate;
        mediaBudget.setTargetRate(rate);
    }
    void process(Time now)
    {
        updateQueueTime(now);
        if (suspended() || now < nextProcessAt) return;
        adjustMediaRate(now);
        const auto elapsed = firstProcess ? Microseconds(config.processInterval) :
            std::chrono::duration_cast<Microseconds>(std::max(now, lastBudgetAt) - lastBudgetAt);
        // Refill once per process interval, never once per popped packet.
        mediaBudget.increaseBudget(std::min(elapsed, Microseconds(MaxProcessingInterval)));
        paddingBudget.increaseBudget(std::min(elapsed, Microseconds(MaxProcessingInterval)));
        lastBudgetAt = now; nextProcessAt = now + config.processInterval;
        processBytes = 0; firstProcess = false;
    }
    void addReady(const Key& key, Priority priority) { ready[static_cast<unsigned>(priority)].push_back(key); }
    void removeReady(const Key& key, Priority priority)
    {
        auto& list = ready[static_cast<unsigned>(priority)];
        list.erase(std::remove(list.begin(), list.end(), key), list.end());
    }
    void removed(const Entry& entry)
    {
        const auto& p = entry.packet;
        stats.queuedBytes -= cost(p); --stats.queuedPackets; ids.erase(p.id);
        (p.priority == Priority::Padding ? paddingQueuedBytes : mediaQueuedBytes) -= cost(p);
        totalQueueTime -= activeTime - entry.activeQueuedAt;
    }
    void emptied()
    {
        if (!streams.empty()) return;
        discardCredit(); totalQueueTime = Microseconds(0);
        // Drain adjustment must not carry into the next frame's idle repayment.
        stats.adjustedRateBps = stats.rateBps; mediaBudget.setTargetRate(stats.rateBps);
    }
    bool eligible(Priority priority) const
    {
        if (suspended()) return false;
        if (!config.enabled) return true;
        if (processBytes >= config.maxBurstBytes) return false;
        if (priority == Priority::Audio && !config.paceAudio) return true;
        if (!mediaBudget.bytesRemaining()) return false;
        // Actual media consumes both budgets. Padding fills only unused capacity.
        return priority != Priority::Padding || (!mediaQueuedBytes && paddingBudget.bytesRemaining());
    }
    std::vector<Packet> expire(Time now)
    {
        updateQueueTime(now);
        if (suspended()) settle(now, true);
        std::vector<Packet> expired;
        for (auto it = streams.begin(); it != streams.end();)
        {
            auto& queue = it->second;
            if (queue.front().queuedAt + config.maxQueueDelay > now) { ++it; continue; }
            removeReady(it->first, queue.front().packet.priority);
            while (!queue.empty() && queue.front().queuedAt + config.maxQueueDelay <= now)
            {
                const auto entry = queue.front(); queue.pop_front(); removed(entry);
                expired.push_back(entry.packet); ++stats.expiredPackets;
            }
            if (queue.empty()) it = streams.erase(it);
            else { addReady(it->first, queue.front().packet.priority); ++it; }
        }
        emptied(); return expired;
    }
};
Pacer::Pacer() : Pacer(Config{}) {}
Pacer::Pacer(Config config, Time now) : impl_(std::make_unique<Impl>(std::move(config), now)) {}
Pacer::~Pacer() = default;
bool Pacer::setRate(std::uint64_t rate, Time now) { return setRates(rate, impl_->config.paddingRateBps, now); }
bool Pacer::setRates(std::uint64_t rate, std::uint64_t paddingRate, Time now)
{
    if (!validRate(rate) || !validRate(paddingRate)) return false;
    auto& s = *impl_; s.updateQueueTime(now);
    const bool wasSuspended = s.suspended();
    s.settle(now, wasSuspended || s.streams.empty());
    s.stats.rateBps = s.config.rateBps = rate; s.config.paddingRateBps = paddingRate;
    s.stats.paddingRateBps = std::min(rate, paddingRate);
    s.paddingBudget.setTargetRate(s.stats.paddingRateBps); s.adjustMediaRate(now);
    // Rate=0 resets its budgets through setTargetRate(0). Resume gets its next
    // periodic refill, rather than a burst accumulated while suspended.
    if (wasSuspended || s.suspended()) s.nextProcessAt = now + s.config.processInterval;
    return true;
}
void Pacer::setPaused(bool paused, Time now)
{
    auto& s = *impl_; s.updateQueueTime(now); s.settle(now, s.suspended() || s.streams.empty());
    if (s.stats.paused != paused) s.nextProcessAt = now + s.config.processInterval;
    s.stats.paused = paused;
}
Pacer::Admission Pacer::enqueue(Packet packet, Time now)
{
    auto& s = *impl_; s.updateQueueTime(now);
    auto reject = [&](Admission result) { ++s.stats.rejectedPackets; return result; };
    if (!packet.id || !packet.wireSize || packet.wireSize > 65507 || static_cast<unsigned>(packet.priority) > 3 || s.ids.count(packet.id))
        return reject(Admission::InvalidPacket);
    if (packet.generation != s.stats.generation) return reject(Admission::GenerationMismatch);
    const auto kind = packet.priority == Priority::Retransmission ? Impl::Kind::Retransmission :
        packet.priority == Priority::Padding ? Impl::Kind::Padding : Impl::Kind::Original;
    const Impl::Key key{packet.ssrc, kind};
    if (s.stats.queuedPackets >= s.config.maxQueuePackets || s.cost(packet) > s.config.maxQueueBytes - s.stats.queuedBytes ||
        (!s.streams.count(key) && s.streams.size() >= s.config.maxStreams)) return reject(Admission::QueueFull);
    if (s.streams.empty())
    {
        s.settle(now, true);
        if (!s.firstProcess) s.nextProcessAt = std::max(s.nextProcessAt, now + s.config.processInterval);
    }
    auto& queue = s.streams[key];
    if (queue.empty()) s.addReady(key, packet.priority);
    queue.push_back({packet, std::max(now, s.queueUpdatedAt), s.activeTime}); s.ids.insert(packet.id);
    ++s.stats.queuedPackets; s.stats.queuedBytes += s.cost(packet); ++s.stats.enqueuedPackets;
    (packet.priority == Priority::Padding ? s.paddingQueuedBytes : s.mediaQueuedBytes) += s.cost(packet);
    return Admission::Accepted;
}
std::vector<Pacer::Packet> Pacer::expire(Time now) { return impl_->expire(now); }
std::optional<Pacer::Packet> Pacer::pop(Time now)
{
    auto& s = *impl_;
    if (s.streams.empty()) return std::nullopt;
    s.process(now);
    for (auto& ready : s.ready)
    {
        if (ready.empty()) continue;
        const auto key = ready.front(); auto it = s.streams.find(key); auto& queue = it->second;
        // Owner first collects expired requests so it can complete their callbacks.
        if (queue.front().queuedAt + s.config.maxQueueDelay <= now || !s.eligible(queue.front().packet.priority))
            return std::nullopt;
        ready.pop_front(); const auto entry = queue.front(); queue.pop_front();
        if (queue.empty()) s.streams.erase(it); else s.addReady(key, queue.front().packet.priority);
        s.removed(entry); const auto& packet = entry.packet;
        const auto bytes = s.cost(packet);
        if (packet.priority != Priority::Audio || s.config.accountForAudio)
        { s.mediaBudget.useBudget(bytes); s.paddingBudget.useBudget(bytes); }
        s.processBytes += bytes;
        ++s.stats.releasedPackets; s.stats.releasedBytes += bytes;
        s.emptied(); return packet;
    }
    return std::nullopt;
}
std::optional<Pacer::Time> Pacer::nextWakeup(Time now)
{
    auto& s = *impl_; if (s.streams.empty()) return std::nullopt;
    auto next = Time::max();
    for (const auto& stream : s.streams) next = std::min(next, stream.second.front().queuedAt + s.config.maxQueueDelay);
    if (!s.suspended())
    {
        const Impl::Entry* head = nullptr;
        for (const auto& ready : s.ready) if (!ready.empty()) { head = &s.streams.at(ready.front()).front(); break; }
        // Process a due tick even when there is currently no positive budget.
        // Zero padding rate needs expiry only, unless media is also queued.
        if (head && (head->packet.priority != Priority::Padding || s.stats.paddingRateBps))
            next = std::min(next, s.eligible(head->packet.priority) ? now : std::max(now, s.nextProcessAt));
    }
    return std::max(now, next);
}
Pacer::Snapshot Pacer::snapshot(Time now) const
{
    const auto& s = *impl_; auto result = s.stats; result.streams = s.streams.size();
    result.mediaBudgetBytes = s.mediaBudget.signedBytesRemaining(); result.paddingBudgetBytes = s.paddingBudget.signedBytesRemaining();
    result.averageQueueTime = s.averageQueueTime(now);
    auto first = now;
    for (const auto& stream : s.streams) first = std::min(first, stream.second.front().queuedAt);
    result.oldestWait = std::chrono::duration_cast<Microseconds>(now - first);
    if (!s.suspended() && (!s.paddingQueuedBytes || result.paddingRateBps))
    {
        auto duration = std::uint64_t(s.mediaQueuedBytes) * 8000000 / result.adjustedRateBps;
        if (s.paddingQueuedBytes) duration += std::uint64_t(s.paddingQueuedBytes) * 8000000 / result.paddingRateBps;
        result.expectedQueueTime = Microseconds(duration);
    }
    return result;
}
std::vector<Pacer::Packet> Pacer::clear(Time now)
{
    auto& s = *impl_; std::vector<Packet> cancelled; cancelled.reserve(s.stats.queuedPackets);
    for (const auto& stream : s.streams) for (const auto& e : stream.second) cancelled.push_back(e.packet);
    s.stats.cancelledPackets += cancelled.size(); s.stats.queuedBytes = s.stats.queuedPackets = 0;
    s.streams.clear(); s.ids.clear(); for (auto& list : s.ready) list.clear();
    s.mediaBudget.reset(); s.paddingBudget.reset(); s.mediaBudget.setTargetRate(s.stats.rateBps);
    s.stats.adjustedRateBps = s.stats.rateBps;
    s.lastBudgetAt = s.nextProcessAt = s.queueUpdatedAt = now; s.firstProcess = true;
    s.activeTime = s.totalQueueTime = Microseconds(0);
    s.mediaQueuedBytes = s.paddingQueuedBytes = s.processBytes = 0;
    return cancelled;
}
std::vector<Pacer::Packet> Pacer::reset(std::uint64_t generation, Time now)
{
    auto cancelled = clear(now); auto& s = *impl_;
    const auto rate = s.stats.rateBps, paddingRate = s.stats.paddingRateBps; const auto paused = s.stats.paused;
    s.stats = {}; s.stats.generation = generation; s.stats.rateBps = s.stats.adjustedRateBps = rate;
    s.stats.paddingRateBps = paddingRate; s.stats.paused = paused; return cancelled;
}
} // namespace webrtc
