#include"webrtc/congestion_control/bitrate_estimator.h"

#include<array>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Time = BitrateEstimator::Time;
void windowsAndTime()
{
    BitrateEstimator e;
    CHECK(!e.bitrate() && !e.PeekRate());
    for (int i = 0; i < 5; ++i) e.Update(Time(i * 100ms), 1000, false);
    CHECK(!e.bitrate() && e.PeekRate() == 100000);
    e.Update(Time(500ms), 9000, false);
    CHECK(e.bitrate() == 80000 && !e.PeekRate()); // boundary bytes go to the next sample
    e.Update(Time(505ms), 0, false);
    CHECK(e.PeekRate() == 14400000 && e.bitrate() == 80000);
    e.Update(Time(650ms), 0, false);
    CHECK(*e.bitrate() > 80000 && *e.bitrate() < 480000 && !e.PeekRate());

    BitrateEstimator rounded;
    rounded.Update(Time(0us), 1000, false);
    rounded.Update(Time(499499us), 1000, false);
    CHECK(!rounded.bitrate());
    rounded.Update(Time(499500us), 9999, false); // Timestamp::ms rounds half up
    CHECK(rounded.bitrate() == 32000);

    BitrateEstimator idle;
    idle.Update(Time(0ms), 1000, false);
    idle.Update(Time(501ms), 1000, false); // a gap > one window discards old bytes
    CHECK(!idle.bitrate() && idle.PeekRate() == 8000000);
    idle.Update(Time(1000ms), 1000, false);
    CHECK(idle.bitrate() == 16000);
    idle.Update(Time(900ms), 2000, false); // backwards time resets the window, retains the estimate
    CHECK(idle.bitrate() == 16000 && !idle.PeekRate());
    idle.Update(Time(1000ms), 0, false);
    CHECK(idle.PeekRate() == 160000);

    BitrateEstimator::Config config; config.initial_window = 350ms; config.window = 250ms;
    config.estimate_floor_bps = 30000;
    BitrateEstimator floor(config);
    floor.Update(Time(0ms), 1000, false); floor.Update(Time(350ms), 1, false);
    CHECK(floor.bitrate() == 22857); // upstream applies floor only to filtered updates
    floor.Update(Time(600ms), 0, false);
    CHECK(floor.bitrate() == 30000);
}
void initialize800k(BitrateEstimator& e)
{
    for (int i = 0; i < 50; ++i) e.Update(Time(i * 10ms), 1000, false);
    e.Update(Time(500ms), 250, false);
    CHECK(e.bitrate() == 800000);
}
void finish200k(BitrateEstimator& e, bool alr)
{
    for (int i = 51; i <= 65; ++i) e.Update(Time(i * 10ms), 250, alr);
}
void uncertaintyAndAlr()
{
    BitrateEstimator normal, fast;
    initialize800k(normal); initialize800k(fast); fast.ExpectFastRateChange();
    finish200k(normal, false); finish200k(fast, false);
    CHECK(*fast.bitrate() < *normal.bitrate() && *fast.bitrate() > 200000);

    BitrateEstimator::Config config; config.uncertainty_scale_in_alr = 100;
    BitrateEstimator alr(config), outside(config);
    initialize800k(alr); initialize800k(outside);
    finish200k(alr, true); finish200k(outside, false);
    CHECK(*alr.bitrate() > *outside.bitrate() && outside.bitrate() == normal.bitrate());
    BitrateEstimator defaultAlr; initialize800k(defaultAlr); finish200k(defaultAlr, true);
    CHECK(defaultAlr.bitrate() == normal.bitrate()); // default ALR scale is also 10, not a freeze

    config.uncertainty_scale_in_alr = 0; config.small_sample_uncertainty_scale = 100;
    config.small_sample_threshold = 3751;
    BitrateEstimator small(config); initialize800k(small); finish200k(small, true);
    config.small_sample_threshold = 3750;
    BitrateEstimator exactThreshold(config); initialize800k(exactThreshold); finish200k(exactThreshold, true);
    CHECK(*small.bitrate() > 700000 && exactThreshold.bitrate() == 200000);
}
// Golden values from the pinned upstream .cc functions, using their original
// float arithmetic and DataRate/Timestamp conversions (see docs/bitrate-estimator.md).
struct Expected { int index; std::int64_t rate, peek; };
void upstreamReplay()
{
    const std::array<std::array<Expected, 12>, 3> expected{{
        {{{50,800000,8000000}, {110,733333,2000000}, {125,654555,-1}, {199,301967,212765},
          {200,205155,-1}, {224,208185,2666666}, {249,217586,3000000}, {299,242465,2637362},
          {300,242465,2408000}, {325,169323,96000}, {350,96944,800000}, {399,80000,97560}}},
        {{{50,800000,847682}, {110,793434,2000000}, {125,793434,213333}, {199,770079,212765},
          {200,770079,360000}, {224,1071639,2571428}, {249,1269889,2571428}, {299,2076996,2553191},
          {300,2076996,2405333}, {325,1625181,85333}, {350,1608989,84768}, {399,1548018,85106}}},
        {{{50,800000,847682}, {110,793434,2000000}, {125,793434,213333}, {199,770079,212765},
          {200,770079,160000}, {224,958043,2571428}, {249,1065257,2571428}, {299,1622393,2553191},
          {300,1622393,-1}, {325,1591095,-1}, {350,1551132,800000}, {399,1503565,82987}}}
    }};
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        BitrateEstimator::Config config;
        if (scenario)
        {
            config.initial_window = 350ms; config.window = 250ms;
            config.uncertainty_scale = 7; config.uncertainty_scale_in_alr = 50;
            config.small_sample_uncertainty_scale = 30; config.small_sample_threshold = 5000;
            config.uncertainty_symmetry_cap_bps = 100000; config.estimate_floor_bps = 20000;
        }
        BitrateEstimator e(config);
        std::size_t next = 0;
        for (int i = 0; i < 400; ++i)
        {
            auto at = Time(1s + i * 10ms + (i % 4) * 250us);
            if (scenario == 2 && i >= 200) at += 2s;
            if (scenario == 2 && i >= 300) at -= 500ms;
            const auto bytes = i < 100 ? 1000 : (i < 200 ? 250 : (i < 300 ? 3000 : 100));
            if (i == 200) e.ExpectFastRateChange();
            e.Update(at, bytes, i >= 100 && i < 200);
            if (next < expected[scenario].size() && i == expected[scenario][next].index)
            {
                const auto& golden = expected[scenario][next++];
                const auto rate = e.bitrate(), peek = e.PeekRate();
                CHECK((rate ? static_cast<std::int64_t>(*rate) : -1) == golden.rate);
                CHECK((peek ? static_cast<std::int64_t>(*peek) : -1) == golden.peek);
            }
        }
        CHECK(next == expected[scenario].size());
    }
}
template<class F> void invalid(F makeConfig)
{
    BitrateEstimator::Config config; makeConfig(config);
    bool threw = false;
    try { BitrateEstimator e(config); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
void limitsAndValidation()
{
    invalid([](auto& c) { c.window = 149ms; });
    invalid([](auto& c) { c.initial_window = 1001ms; });
    invalid([](auto& c) { c.uncertainty_scale = -1; });
    invalid([](auto& c) { c.uncertainty_scale_in_alr = std::numeric_limits<double>::quiet_NaN(); });
    invalid([](auto& c) { c.small_sample_uncertainty_scale = std::numeric_limits<double>::infinity(); });
    BitrateEstimator zero;
    zero.Update(Time(0ms), 0, false); zero.Update(Time(500ms), 0, false);
    CHECK(zero.bitrate() == 0);
    zero.Update(Time(650ms), 1000, false); CHECK(zero.bitrate() == 0);
    zero.Update(Time(800ms), 1000, false); CHECK(*zero.bitrate() > 0);
    constexpr auto maxSize = std::numeric_limits<std::size_t>::max();
    constexpr auto maxRate = std::numeric_limits<std::uint64_t>::max();
    BitrateEstimator large;
    large.Update(Time(0ms), maxSize, false); large.Update(Time(100ms), maxSize, false);
    CHECK(large.PeekRate() == maxRate);
    large.Update(Time(500ms), maxSize, false); CHECK(large.bitrate() == maxRate);
    large.Update(Time(650ms), maxSize, false); CHECK(large.bitrate() == maxRate);
    BitrateEstimator::Config config; config.uncertainty_scale = std::numeric_limits<float>::max();
    BitrateEstimator wide(config); initialize800k(wide); finish200k(wide, false);
    CHECK(wide.bitrate() == 800000); // huge uncertainty cannot introduce NaNs
}
} // namespace
int main()
{
    try
    {
        windowsAndTime(); uncertaintyAndAlr(); upstreamReplay(); limitsAndValidation();
        std::cout << "PASS: bitrate windows, upstream replay, ALR uncertainty, fast change and limits\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
