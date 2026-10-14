#include"webrtc/congestion_control/pacer.h"

#include<chrono>
#include<iostream>
#include<stdexcept>
#include<vector>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
Pacer::Config config()
{
    Pacer::Config c; c.rateBps = 8000; c.drainLargeQueues = false; c.maxQueueDelay = 10s; return c;
}
Pacer::Packet packet(std::uint64_t id, std::uint32_t ssrc = 1, Pacer::Priority priority = Pacer::Priority::Video,
    std::size_t bytes = 100, std::uint64_t generation = 0)
{ return {id, generation, ssrc, priority, bytes}; }
void expectPacket(Pacer& p, Pacer::Time t, std::uint64_t id)
{ const auto next = p.pop(t); CHECK(next && next->id == id); }
void ratesAndIdle()
{
    const auto t = Pacer::Time(10s); Pacer p(config(), t);
    CHECK(p.enqueue(packet(1), t) == Pacer::Admission::Accepted && p.enqueue(packet(2), t) == Pacer::Admission::Accepted);
    expectPacket(p, t, 1); CHECK(p.snapshot(t).mediaBudgetBytes == -95);
    // Budget is refilled only once per 5 ms process interval, not on every pop.
    CHECK(!p.pop(t + 1ms) && !p.pop(t + 4ms)); CHECK(p.nextWakeup(t + 4ms) == t + 5ms);
    for (unsigned ms = 5; ms <= 95; ms += 5) CHECK(!p.pop(t + std::chrono::milliseconds(ms)));
    expectPacket(p, t + 100ms, 2); CHECK(!p.nextWakeup(t + 100ms));
    CHECK(p.snapshot(t + 100ms).expectedQueueTime == 0us);
    for (unsigned i = 0; i < 3; ++i) CHECK(p.enqueue(packet(3 + i), t + 100s) == Pacer::Admission::Accepted);
    CHECK(!p.pop(t + 100s)); expectPacket(p, t + 100s + 5ms, 3); CHECK(!p.pop(t + 100s + 5ms));
    CHECK(p.setRate(16000, t + 100s + 5ms));
    CHECK(p.snapshot(t + 100s + 5ms).mediaBudgetBytes == -95); // byte debt is retained
    for (unsigned ms = 10; ms <= 50; ms += 5) CHECK(!p.pop(t + 100s + std::chrono::milliseconds(ms)));
    expectPacket(p, t + 100s + 55ms, 4);
    CHECK(!p.setRate(999, t + 100s + 55ms) && !p.setRate(1000000001, t + 100s + 55ms));
    CHECK(p.snapshot(t + 100s + 55ms).rateBps == 16000);
    CHECK(p.setRates(0, 8000, t + 100s + 55ms) && !p.pop(t + 101s));
    CHECK(!p.snapshot(t + 101s).expectedQueueTime && p.snapshot(t + 101s).paddingRateBps == 0);
    CHECK(p.setRate(8000, t + 102s)); CHECK(!p.pop(t + 102s));
    expectPacket(p, t + 102s + 5ms, 5); CHECK(p.snapshot(t + 102s + 5ms).paddingRateBps == 8000);
}
void prioritiesAndFairness()
{
    const auto t = Pacer::Time(10s); auto c = config(); c.rateBps = c.paddingRateBps = 8000000; Pacer p(c, t);
    using Priority = Pacer::Priority;
    CHECK(p.enqueue(packet(1, 10), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(2, 11), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(3, 10), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(4, 20, Priority::Audio), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(5, 10, Priority::Retransmission), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(6, 30, Priority::Padding), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(7, 10, Priority::Audio), t) == Pacer::Admission::Accepted);
    std::vector<std::uint64_t> ids;
    while (const auto next = p.pop(t)) ids.push_back(next->id);
    CHECK(ids == std::vector<std::uint64_t>({4, 5, 1, 2, 3, 7, 6}));
}
void burstAndOverhead()
{
    const auto t = Pacer::Time(20s); auto c = config(); c.rateBps = 800000;
    Pacer p(c, t);
    for (unsigned i = 0; i < 20; ++i) CHECK(p.enqueue(packet(i + 1, 1, Pacer::Priority::Video, 400), t) == Pacer::Admission::Accepted);
    unsigned first = 0; while (p.pop(t)) ++first; CHECK(first == 2); // 500-byte budget allows one whole-packet overdraft
    unsigned late = 0; while (p.pop(t + 1s)) ++late; CHECK(late == 7); // only 30 ms of a 1 s scheduler delay is credited
    CHECK(p.snapshot(t + 1s).mediaBudgetBytes == -100 && p.snapshot(t + 1s).queuedBytes == 4400);
    c.maxBurstBytes = 100; Pacer capped(c, t);
    for (unsigned i = 0; i < 10; ++i) CHECK(capped.enqueue(packet(i + 1), t) == Pacer::Admission::Accepted);
    expectPacket(capped, t, 1); CHECK(!capped.pop(t) && capped.nextWakeup(t) == t + 5ms);
    expectPacket(capped, t + 5ms, 2); CHECK(!capped.pop(t + 5ms));
    c = config(); c.transportOverheadBytes = 28; c.maxQueueBytes = 255; Pacer overhead(c, t);
    CHECK(overhead.enqueue(packet(1), t) == Pacer::Admission::Accepted);
    CHECK(overhead.enqueue(packet(2), t) == Pacer::Admission::QueueFull);
    CHECK(overhead.snapshot(t).queuedBytes == 128); expectPacket(overhead, t, 1);
    CHECK(overhead.enqueue(packet(3), t) == Pacer::Admission::Accepted);
    for (unsigned ms = 5; ms <= 120; ms += 5) CHECK(!overhead.pop(t + std::chrono::milliseconds(ms)));
    expectPacket(overhead, t + 125ms, 3);
}
void paddingAndAudio()
{
    const auto t = Pacer::Time(20s); auto c = config(); c.rateBps = 32000; c.paddingRateBps = 8000;
    Pacer p(c, t);
    CHECK(p.enqueue(packet(1), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(2, 1, Pacer::Priority::Padding, 20), t) == Pacer::Admission::Accepted);
    expectPacket(p, t, 1);
    CHECK(p.snapshot(t).mediaBudgetBytes == -80 && p.snapshot(t).paddingBudgetBytes == -95);
    for (unsigned ms = 5; ms <= 95; ms += 5) CHECK(!p.pop(t + std::chrono::milliseconds(ms)));
    expectPacket(p, t + 100ms, 2);
    CHECK(p.snapshot(t + 100ms).paddingBudgetBytes == -15);
    CHECK(p.setRates(8000, 64000, t + 105ms)); CHECK(p.snapshot(t + 105ms).paddingRateBps == 8000);
    CHECK(!p.setRates(8000, 999, t + 105ms) && p.snapshot(t + 105ms).paddingRateBps == 8000);

    c = config(); Pacer noPadding(c, t);
    CHECK(noPadding.enqueue(packet(3, 1, Pacer::Priority::Padding), t) == Pacer::Admission::Accepted);
    CHECK(noPadding.enqueue(packet(4, 1), t) == Pacer::Admission::Accepted);
    expectPacket(noPadding, t, 4); CHECK(!noPadding.pop(t));
    CHECK(noPadding.nextWakeup(t) == t + 10s && !noPadding.snapshot(t).expectedQueueTime);

    c.paceAudio = false; c.accountForAudio = true; Pacer audio(c, t);
    CHECK(audio.enqueue(packet(1), t) == Pacer::Admission::Accepted); expectPacket(audio, t, 1);
    CHECK(audio.enqueue(packet(2, 20, Pacer::Priority::Audio), t + 1ms) == Pacer::Admission::Accepted);
    expectPacket(audio, t + 1ms, 2); CHECK(audio.snapshot(t + 1ms).mediaBudgetBytes == -194);
    c.accountForAudio = false; Pacer unaccounted(c, t);
    CHECK(unaccounted.enqueue(packet(1, 20, Pacer::Priority::Audio), t) == Pacer::Admission::Accepted);
    CHECK(unaccounted.enqueue(packet(2), t) == Pacer::Admission::Accepted);
    expectPacket(unaccounted, t, 1); CHECK(unaccounted.snapshot(t).mediaBudgetBytes == 5);
    expectPacket(unaccounted, t, 2); CHECK(unaccounted.snapshot(t).mediaBudgetBytes == -95);
}
void drainQueues()
{
    const auto t = Pacer::Time(20s); auto c = config(); c.drainLargeQueues = true; c.queueTimeLimit = 100ms;
    Pacer p(c, t);
    for (unsigned i = 0; i < 10; ++i) CHECK(p.enqueue(packet(i + 1), t) == Pacer::Admission::Accepted);
    expectPacket(p, t, 1); CHECK(p.snapshot(t).rateBps == 8000 && p.snapshot(t).adjustedRateBps == 80000);
    CHECK(p.snapshot(t + 5ms).averageQueueTime == 5ms);
    p.setPaused(true, t + 5ms);
    CHECK(p.snapshot(t + 1s).averageQueueTime == 5ms && p.snapshot(t + 1s).oldestWait == 1s);
    p.setPaused(false, t + 1s); CHECK(!p.pop(t + 1s));
    expectPacket(p, t + 1s + 5ms, 2); CHECK(p.snapshot(t + 1s + 5ms).averageQueueTime == 10ms);
    p.pop(t + 1s + 10ms);
    CHECK(p.snapshot(t + 1s + 10ms).adjustedRateBps > 8000);
    CHECK(p.clear(t + 1s + 10ms).size() == 8);
    CHECK(p.snapshot(t + 1s + 10ms).adjustedRateBps == 8000 && p.snapshot(t + 1s + 10ms).averageQueueTime == 0us);
}
void limitsPauseAndExpiry()
{
    const auto t = Pacer::Time(20s); auto c = config(); c.maxQueuePackets = 2; c.maxStreams = 1; c.maxQueueDelay = 100ms;
    Pacer p(c, t); p.setPaused(true, t);
    CHECK(p.enqueue(packet(1), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(1), t) == Pacer::Admission::InvalidPacket);
    CHECK(p.enqueue(packet(2, 2), t) == Pacer::Admission::QueueFull);
    CHECK(p.enqueue(packet(2), t) == Pacer::Admission::Accepted);
    CHECK(p.enqueue(packet(3), t) == Pacer::Admission::QueueFull);
    CHECK(!p.pop(t) && p.nextWakeup(t) == t + 100ms);
    CHECK(p.expire(t + 99ms).empty()); CHECK(p.expire(t + 100ms).size() == 2);
    CHECK(p.snapshot(t + 100ms).expiredPackets == 2 && !p.nextWakeup(t + 100ms));
    CHECK(p.enqueue(packet(3), t + 101ms) == Pacer::Admission::Accepted);
    p.setPaused(false, t + 105ms); CHECK(!p.pop(t + 105ms)); expectPacket(p, t + 110ms, 3);
    CHECK(p.setRate(0, t + 115ms)); CHECK(p.enqueue(packet(4), t + 115ms) == Pacer::Admission::Accepted);
    CHECK(!p.pop(t + 200ms) && p.nextWakeup(t + 200ms) == t + 215ms);
    CHECK(p.expire(t + 215ms).size() == 1);
}
void generationsAndConfiguration()
{
    const auto t = Pacer::Time(30s); Pacer p(config(), t);
    CHECK(p.enqueue(packet(1), t) == Pacer::Admission::Accepted);
    CHECK(p.clear(t).size() == 1 && p.snapshot(t).cancelledPackets == 1);
    CHECK(p.enqueue(packet(2), t) == Pacer::Admission::Accepted); p.setPaused(true, t);
    CHECK(p.setRates(64000, 8000, t)); CHECK(p.reset(7, t).size() == 1);
    CHECK(p.snapshot(t).generation == 7 && p.snapshot(t).queuedPackets == 0 && p.snapshot(t).enqueuedPackets == 0);
    CHECK(p.snapshot(t).rateBps == 64000 && p.snapshot(t).paddingRateBps == 8000 && p.snapshot(t).paused);
    CHECK(p.enqueue(packet(3), t) == Pacer::Admission::GenerationMismatch);
    CHECK(p.enqueue(packet(3, 1, Pacer::Priority::Video, 100, 7), t) == Pacer::Admission::Accepted);
    p.setPaused(false, t); CHECK(!p.pop(t)); expectPacket(p, t + 5ms, 3);
    CHECK(p.enqueue(packet(0), t) == Pacer::Admission::InvalidPacket);
    CHECK(p.enqueue(packet(4, 1, Pacer::Priority::Video, 65508, 7), t) == Pacer::Admission::InvalidPacket);
    auto c = config(); c.rateBps = 1; bool threw = false;
    try { Pacer invalid(c, t); } catch (const std::invalid_argument&) { threw = true; } CHECK(threw);
    c = config(); c.processInterval = 0ms; CHECK(!c.validate());
    c = config(); c.processInterval = 31ms; CHECK(!c.validate());
    c = config(); c.queueTimeLimit = 0ms; CHECK(!c.validate());
}
}
int main()
{
    try
    {
        ratesAndIdle(); prioritiesAndFairness(); burstAndOverhead(); paddingAndAudio(); drainQueues();
        limitsPauseAndExpiry(); generationsAndConfiguration();
        std::cout << "PASS: periodic media/padding budgets, priority/FIFO/fairness, drain, limits, pause and generation\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
