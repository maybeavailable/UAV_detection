#pragma once

#include<chrono>
#include<cstddef>
#include<cstdint>
#include<memory>
#include<optional>
#include<string>
#include<vector>

namespace webrtc
{
// Scheduling engine; DtlsTransport drives it with its strand and Asio steady_timer.
// No socket, encryption or payload ownership here. The owner retains packets by id.
class Pacer
{
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Priority { Audio, Retransmission, Video, Padding };
    enum class Admission { Accepted, InvalidPacket, GenerationMismatch, QueueFull };
    struct Config
    {
        bool enabled{true};
        std::uint64_t rateBps{3000000}, paddingRateBps{0}; // bit/s; padding is capped to media rate
        std::chrono::milliseconds processInterval{5}, maxQueueDelay{500};
        bool drainLargeQueues{true}, paceAudio{true}, accountForAudio{true};
        std::chrono::milliseconds queueTimeLimit{2000}; // soft drain target; expiry remains a hard wall-clock limit
        std::size_t maxBurstBytes{64 * 1024}; // per-process release cap; one whole packet may cross it
        std::size_t maxQueuePackets{256}, maxQueueBytes{2 * 1024 * 1024}, maxStreams{256};
        std::size_t transportOverheadBytes{0}; // extra UDP/IP/relay bytes beyond full SRTP wireSize
        bool validate(std::string* reason = nullptr) const;
    };
    struct Packet
    {
        std::uint64_t id{0}, generation{0};
        std::uint32_t ssrc{0};
        Priority priority{Priority::Video};
        std::size_t wireSize{0}; // complete UDP payload, including RTP extensions and SRTP tag
    };
    struct Snapshot
    {
        std::uint64_t generation{0}, rateBps{0}, adjustedRateBps{0}, paddingRateBps{0};
        std::int64_t mediaBudgetBytes{0}, paddingBudgetBytes{0}; // negative means outstanding byte debt
        bool paused{false};
        std::size_t queuedPackets{0}, queuedBytes{0}, streams{0}; // queuedBytes includes transport overhead
        std::chrono::microseconds oldestWait{0};
        std::chrono::microseconds averageQueueTime{0}; // excludes explicit pause and rate=0
        std::optional<std::chrono::microseconds> expectedQueueTime;
        std::uint64_t enqueuedPackets{0}, releasedPackets{0}, releasedBytes{0};
        std::uint64_t rejectedPackets{0}, expiredPackets{0}, cancelledPackets{0};
    };

    Pacer();
    explicit Pacer(Config config, Time now = Clock::now()); // invalid config throws invalid_argument
    ~Pacer();
    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;
    static bool validRate(std::uint64_t rateBps); // 0 or 1000..1,000,000,000
    bool setRate(std::uint64_t rateBps, Time now);
    bool setRates(std::uint64_t rateBps, std::uint64_t paddingRateBps, Time now);
    void setPaused(bool paused, Time now);
    Admission enqueue(Packet packet, Time now);
    // Caller expires queued requests before release, completing them with timed_out.
    std::vector<Packet> expire(Time now);
    std::optional<Packet> pop(Time now);
    std::optional<Time> nextWakeup(Time now); // includes expiry deadlines while paused/rate=0
    Snapshot snapshot(Time now) const;
    std::vector<Packet> clear(Time now); // cancellation; retains counters/settings
    std::vector<Packet> reset(std::uint64_t generation, Time now); // clears history and counters, retains rate/pause
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace webrtc
