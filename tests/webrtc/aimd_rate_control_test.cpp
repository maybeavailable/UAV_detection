#include"webrtc/congestion_control/aimd_rate_control.h"
#include<cmath>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = AimdRateControl::Time;
using Usage = BandwidthUsage;
void initializationAndGrowth()
{
    AimdRateControl e;
    CHECK(!e.ValidEstimate() && e.LatestEstimate() == 30000000);
    e.Update({Usage::kBwNormal, 123000}, Time(1s));
    e.Update({Usage::kBwNormal, 123000}, Time(6s));
    CHECK(!e.ValidEstimate()); // Strictly more than five seconds.
    e.Update({Usage::kBwNormal, 123000}, Time(6s + 1us));
    CHECK(e.ValidEstimate() && e.LatestEstimate() == 124000);
    for (int i = 1; i <= 100; ++i) e.Update({Usage::kBwNormal, std::nullopt}, Time(6s + 1us + i * 100ms));
    CHECK(e.LatestEstimate() == 194500); // 1.5 * last throughput + 10 kbit/s.
    e.Update({Usage::kBwNormal, 10000}, Time(17s));
    CHECK(e.LatestEstimate() == 194500); // Throughput cap does not itself decrease BWE.
    e.Update({Usage::kBwUnderusing, 10000}, Time(18s));
    CHECK(e.LatestEstimate() == 194500);

    AimdRateControl initial_overuse;
    CHECK(initial_overuse.Update({Usage::kBwOverusing, 200000}, Time(0s)) == 165000);
    CHECK(initial_overuse.ValidEstimate());
    e.SetEstimate(60000000, Time(19s));
    CHECK(e.LatestEstimate() == 60000000); // 30 Mbit/s is not a maximum constraint.
}
void increaseRateAndReductionGates()
{
    AimdRateControl e;
    e.SetEstimate(30000, Time(1s)); CHECK(e.GetNearMaxIncreaseRateBpsPerSecond() == 4000);
    e.SetEstimate(90000, Time(1s)); CHECK(e.GetNearMaxIncreaseRateBpsPerSecond() == 5000);
    e.SetEstimate(60000, Time(1s)); e.SetRtt(100ms); CHECK(e.GetNearMaxIncreaseRateBpsPerSecond() == 5000);
    e.SetEstimate(264000, Time(1s)); e.SetRtt(200ms);
    CHECK(e.Update({Usage::kBwOverusing, 260000}, Time(1100ms)) == 216000);
    CHECK(e.GetNearMaxIncreaseRateBpsPerSecond() == 12000);
    e.Update({Usage::kBwNormal, 260000}, Time(1200ms));
    CHECK(e.LatestEstimate() == 216000); // First normal after hold: additive dt = 0.
    e.Update({Usage::kBwNormal, 260000}, Time(1300ms));
    CHECK(e.LatestEstimate() == 217200);
    CHECK(!e.TimeToReduceFurther(Time(1301ms), 200000));
    CHECK(e.TimeToReduceFurther(Time(1301ms), 10000));
    CHECK(!e.TimeToReduceFurther(Time(1499ms), 200000));
    CHECK(e.TimeToReduceFurther(Time(1500ms), 200000));
    CHECK(e.InitialTimeToReduceFurther(Time(1300ms))); // Actual upstream half-rate escape.
    e.SetRtt(0ms);
    CHECK(!e.TimeToReduceFurther(Time(1309ms), 200000));
    CHECK(e.TimeToReduceFurther(Time(1310ms), 200000));
    e.SetEstimate(5000, Time(2s)); CHECK(e.GetFeedbackInterval() == 1s);
    e.SetEstimate(20000, Time(2s)); CHECK(e.GetFeedbackInterval() == 640ms);
    e.SetEstimate(1000000, Time(2s)); CHECK(e.GetFeedbackInterval() == 200ms);
}
void alrAndNetworkBounds()
{
    AimdRateControl::Config config; config.send_side = true; config.no_bitrate_increase_in_alr = true;
    AimdRateControl e(config); e.SetStartBitrate(123000); e.SetInApplicationLimitedRegion(true);
    for (int i = 0; i < 100; ++i) e.Update({Usage::kBwNormal, 123000}, Time(i * 100ms));
    CHECK(e.LatestEstimate() == 123000);
    e.SetEstimate(246000, Time(10s)); CHECK(e.LatestEstimate() == 246000); // Probe may still raise it.
    e.SetInApplicationLimitedRegion(false);
    e.Update({Usage::kBwNormal, 246000}, Time(10100ms)); CHECK(e.LatestEstimate() > 246000);

    e.SetEstimate(300000, Time(11s));
    e.SetNetworkStateEstimate(NetworkStateEstimate{std::nullopt, 400000});
    e.SetEstimate(500000, Time(11s)); CHECK(e.LatestEstimate() == 400000);
    e.SetNetworkStateEstimate(NetworkStateEstimate{std::nullopt, 300000});
    e.SetEstimate(700000, Time(11s)); CHECK(e.LatestEstimate() == 400000);
    e.SetNetworkStateEstimate(NetworkStateEstimate{400000, std::nullopt});
    e.SetEstimate(100000, Time(11s)); CHECK(e.LatestEstimate() == 340000);
    e.SetNetworkStateEstimate(std::nullopt); e.SetEstimate(200000, Time(11s));
    e.SetNetworkStateEstimate(NetworkStateEstimate{400000, std::nullopt});
    e.SetEstimate(100000, Time(11s)); CHECK(e.LatestEstimate() == 200000);

    config.use_current_estimate_as_min_upper_bound = false;
    AimdRateControl strict(config); strict.SetStartBitrate(500000);
    strict.SetNetworkStateEstimate(NetworkStateEstimate{std::nullopt, 300000});
    strict.SetEstimate(700000, Time(0s)); CHECK(strict.LatestEstimate() == 300000);
    config.disable_estimate_bounded_increase = true;
    AimdRateControl disabled(config); disabled.SetStartBitrate(500000);
    disabled.SetNetworkStateEstimate(NetworkStateEstimate{std::nullopt, 300000});
    disabled.SetEstimate(700000, Time(0s)); CHECK(disabled.LatestEstimate() == 700000);
}
void boundariesAndValidation()
{
    const auto max = std::numeric_limits<std::uint64_t>::max();
    AimdRateControl e; e.SetStartBitrate(max);
    CHECK(e.Update({Usage::kBwNormal, max}, Time(0s)) == max);
    CHECK(e.Update({Usage::kBwNormal, max}, Time(1s)) == max);
    CHECK(std::isfinite(e.GetNearMaxIncreaseRateBpsPerSecond()));
    CHECK(e.Update({Usage::kBwOverusing, max}, Time(2s)) < max);
    e.SetMinBitrate(max); CHECK(e.LatestEstimate() == max);
    e.SetMinBitrate(0); e.SetStartBitrate(0);
    CHECK(e.GetFeedbackInterval() == 1s && e.GetNearMaxIncreaseRateBpsPerSecond() == 4000);
    CHECK(e.Update({Usage::kBwNormal, 0}, Time(3s)) == 0); // Retained capacity history: additive dt = 0.
    AimdRateControl fresh; fresh.SetMinBitrate(0); fresh.SetStartBitrate(0);
    CHECK(fresh.Update({Usage::kBwNormal, 0}, Time(0s)) == 1000);
    const auto before = e.LatestEstimate();
    bool caught = false;
    try { e.Update({Usage::kLast, 5000}, Time(4s)); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught && e.LatestEstimate() == before);
    caught = false;
    try { e.SetEstimate(6000, Time(2999ms)); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught && e.LatestEstimate() == before);
    caught = false;
    try { e.SetRtt(-1us); } catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught);
    for (const double beta : {0.0, 1.0, -0.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        AimdRateControl::Config config; config.backoff_factor = beta;
        AimdRateControl fallback(config); fallback.SetStartBitrate(300000);
        CHECK(fallback.Update({Usage::kBwOverusing, 200000}, Time(0s)) == 165000);
    }
}
void upstreamReplay()
{
    // Generated by the pinned, original AIMD/LinkCapacity + original WebRTC
    // integer unit classes. Only platform/field-trial/logging dependencies stubbed.
    constexpr std::uint64_t golden[] = {9112604010230426008ULL, 6199096055765529514ULL, 9423703464735626813ULL,
        13167167677625598906ULL, 18042194023098997975ULL, 624947658485485725ULL};
    for (int s = 0; s < 6; ++s)
    {
        AimdRateControl::Config config; config.send_side = true; config.no_bitrate_increase_in_alr = s == 2;
        config.disable_estimate_bounded_increase = s == 4; config.use_current_estimate_as_min_upper_bound = s != 3;
        config.backoff_factor = s == 5 ? .7 : .85;
        AimdRateControl e(config); if (s != 0) e.SetStartBitrate(600000);
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        std::uint32_t random = 0x61696d64U + s;
        for (int i = 0; i < 1000; ++i)
        {
            random = random * 1664525U + 1013904223U; const std::uint64_t ack = 10000 + (random >> 8) % 1000000;
            const Time now(1000000us + i * 100000us + (i % 3) * 250us);
            std::optional<NetworkStateEstimate> network;
            if (i % 13 != 0) network = NetworkStateEstimate{std::uint64_t(i % 7 ? 100000 : 700000), std::uint64_t(i % 11 ? 750000 : 200000)};
            e.SetNetworkStateEstimate(network); e.SetInApplicationLimitedRegion((i / 100) % 2);
            if (i % 97 == 0) e.SetRtt((i % 5) * 75ms);
            if (i % 61 == 0) e.SetMinBitrate(i % 2 ? 10000 : 5000);
            if (i % 47 == 46) e.SetEstimate(20000 + ack, now);
            const auto state = i % 200 < 80 ? Usage::kBwNormal : (i % 200 < 110 ? Usage::kBwOverusing :
                (i % 200 < 150 ? Usage::kBwUnderusing : Usage::kBwNormal));
            std::optional<std::uint64_t> rate; if (i % 9 != 0) rate = ack;
            mix(e.Update({state, rate}, now)); mix(e.ValidEstimate());
            mix(static_cast<std::uint64_t>(e.GetNearMaxIncreaseRateBpsPerSecond())); mix(e.GetFeedbackInterval().count());
            mix(e.TimeToReduceFurther(now, ack)); mix(e.InitialTimeToReduceFurther(now));
        }
        CHECK(hash == golden[s]);
    }
}
} // namespace
int main()
{
    try
    {
        initializationAndGrowth(); increaseRateAndReductionGates(); alrAndNetworkBounds(); boundariesAndValidation(); upstreamReplay();
        std::cout << "PASS: AIMD initialization, growth/backoff, RTT, ALR, network bounds and 6000 upstream updates\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
