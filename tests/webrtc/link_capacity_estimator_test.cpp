#include"webrtc/congestion_control/link_capacity_estimator.h"
#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"

#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
void expect(const LinkCapacityEstimator& e, std::uint64_t mean, std::uint64_t lower, std::uint64_t upper)
{
    CHECK(e.has_estimate() && e.estimate() == mean && e.LowerBound() == lower && e.UpperBound() == upper);
    CHECK(lower <= mean && mean <= upper);
}
void initialUpdatesAndReset()
{
    LinkCapacityEstimator e;
    CHECK(!e.has_estimate() && !e.estimate() && !e.UpperBound() && e.LowerBound() == 0);
    e.Reset(); CHECK(!e.has_estimate());
    e.OnOveruseDetected(500000); expect(e, 500000, 457573, 542426);
    e.OnOveruseDetected(600000); expect(e, 505000, 428918, 581081);
    e.Reset(); e.OnOveruseDetected(500000); expect(e, 500000, 426213, 573786);

    LinkCapacityEstimator probe;
    probe.OnOveruseDetected(500000); probe.OnProbeRate(1000000);
    expect(probe, 750000, 620096, 879903); // probe gain 0.5, normalized variance capped at 2.5
    auto probeAfterReset = probe;
    probe.Reset(); probe.Reset();
    CHECK(!probe.estimate() && !probe.UpperBound() && probe.LowerBound() == 0);
    probe.OnOveruseDetected(500000); expect(probe, 500000, 396619, 603380); // variance 2.5 -> 2.375, not reset to 0.4
    probeAfterReset.Reset(); probeAfterReset.OnProbeRate(500000);
    expect(probeAfterReset, 500000, 425000, 575000); // variance 2.5 -> 1.25
    for (int i = 0; i < 1000; ++i) probe.OnOveruseDetected(500000);
    expect(probe, 500000, 457573, 542426); // variance converges to the original float clamp floor
}
void quantizationAndZero()
{
    struct Point { std::uint64_t sample, mean, lower, upper; };
    const Point points[] = {{0,0,0,0}, {499,0,0,0}, {500,1000,0,2897}, {1499,1000,0,2897},
        {1500,2000,0,4683}, {4499,4000,205,7794}, {4500,5000,757,9242}, {1234567,1235000,1168321,1301678}};
    for (const auto& point : points)
    {
        LinkCapacityEstimator overuse, probe;
        overuse.OnOveruseDetected(point.sample); probe.OnProbeRate(point.sample);
        expect(overuse, point.mean, point.lower, point.upper); expect(probe, point.mean, point.lower, point.upper);
    }
    LinkCapacityEstimator zero; zero.OnProbeRate(0);
    CHECK(zero.has_estimate() && zero.estimate().has_value() && zero.UpperBound().has_value());
    expect(zero, 0, 0, 0); zero.Reset(); CHECK(!zero.estimate() && !zero.UpperBound());

    LinkCapacityEstimator slow, fast;
    slow.OnOveruseDetected(500000); fast.OnOveruseDetected(500000);
    slow.OnOveruseDetected(5000000); fast.OnProbeRate(5000000);
    CHECK(slow.estimate() == 725000 && fast.estimate() == 2750000);
    // Updating outside the previous bounds still uses the weighted mean;
    // deciding when to reset is the rate controller's responsibility.
}
void rateLimits()
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    LinkCapacityEstimator e; e.OnProbeRate(max);
    CHECK(e.estimate() == max && e.UpperBound() == max && e.LowerBound() > 0);
    for (const auto rate : {std::uint64_t(0), max, std::uint64_t(1), max / 2, max - 499})
    {
        e.OnOveruseDetected(rate);
        CHECK(e.has_estimate() && e.LowerBound() <= *e.estimate() && *e.estimate() <= *e.UpperBound());
        e.OnProbeRate(rate);
        CHECK(e.LowerBound() <= *e.estimate() && *e.estimate() <= *e.UpperBound());
    }
    e.Reset(); e.OnProbeRate(0); expect(e, 0, 0, 0);
}
void acknowledgedEstimatorHandoff()
{
    AcknowledgedBitrateEstimator acknowledged;
    LinkCapacityEstimator capacity;
    std::vector<PacketResult> packets;
    for (int i = 0; i <= 50; ++i)
    {
        PacketResult packet;
        packet.sent_packet.send_time = TransportTime(1s + i * 10ms);
        packet.receive_time = TransportTime(100s + i * 10ms);
        packet.sent_packet.size = 928; packet.sent_packet.prior_unacked_data = 100;
        packets.push_back(packet);
    }
    acknowledged.IncomingPacketFeedbackVector(packets);
    CHECK(acknowledged.bitrate() == 822400 && !capacity.has_estimate());
    // The controller calls this when its delay detector reports overuse.
    capacity.OnOveruseDetected(*acknowledged.bitrate()); CHECK(capacity.estimate() == 822000);
    capacity.OnProbeRate(1645000); CHECK(capacity.estimate() == 1233500);
    const auto low = capacity.LowerBound(); CHECK(low > 0);
    const auto newThroughput = low / 2;
    if (newThroughput < capacity.LowerBound()) capacity.Reset(); // lower-bound policy from upstream AIMD
    CHECK(!capacity.has_estimate()); capacity.OnOveruseDetected(newThroughput);
    CHECK(capacity.estimate() == (newThroughput / 1000 + (newThroughput % 1000 >= 500)) * 1000);
}
void upstreamReplay()
{
    // Golden public results from the pinned original .h/.cc functions, replacing
    // DataRate and SafeClamp with equivalent standalone implementations.
    const std::uint64_t expected_hashes[] = {10537926255437548013ULL, 7962967065610925250ULL, 13518683239949446052ULL};
    const std::uint64_t expected_final[3][3] = {{500000,457573,542426}, {798741,743517,853966}, {185330,120755,249905}};
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        LinkCapacityEstimator estimator; std::uint32_t random = 0x6c696e6bU + scenario;
        const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
        const auto record = [&] {
            mix(estimator.has_estimate());
            if (const auto mean = estimator.estimate()) mix(*mean);
            mix(estimator.UpperBound().value_or(std::numeric_limits<std::uint64_t>::max()));
            mix(estimator.LowerBound());
        };
        record();
        for (int i = 0; i < 1000; ++i)
        {
            if (i > 0 && i % 137 == 0) { estimator.Reset(); record(); }
            std::int64_t sample = 0;
            if (scenario == 0) sample = 500000 + std::int64_t((next() >> 16) % 999) - 499;
            else if (scenario == 1)
            {
                const std::int64_t base = i < 300 ? 500000 : (i < 600 ? 900000 : (i < 900 ? 150000 : 800000));
                sample = base + std::int64_t((next() >> 16) % 50001) - 25000;
                if (i % 113 == 112) sample = 0;
            }
            else
            {
                const std::int64_t edges[] = {0,499,500,1499,1500,4499,4500,10000,499999,500500,1234567,30000000};
                sample = edges[(next() >> 16) % 12];
            }
            if (i % 7 == 0) estimator.OnProbeRate(sample); else estimator.OnOveruseDetected(sample);
            record();
        }
        CHECK(hash == expected_hashes[scenario]);
        expect(estimator, expected_final[scenario][0], expected_final[scenario][1], expected_final[scenario][2]);
    }
}
} // namespace
int main()
{
    try
    {
        initialUpdatesAndReset(); quantizationAndZero(); rateLimits(); acknowledgedEstimatorHandoff(); upstreamReplay();
        std::cout << "PASS: link capacity gains, bounds, variance reset, quantization and upstream replay\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
