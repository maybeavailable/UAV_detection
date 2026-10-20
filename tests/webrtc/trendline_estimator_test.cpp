#include"webrtc/congestion_control/trendline_estimator.h"
#include"webrtc/congestion_control/inter_arrival_delta.h"
#include"webrtc/transport_adapter.h"

#include<algorithm>
#include<cmath>
#include<iostream>
#include<limits>
#include<memory>
#include<stdexcept>
#include<vector>

using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
using Usage = BandwidthUsage;
void close(double a, double b) { CHECK(std::abs(a - b) <= 1e-10 * std::max(1.0, std::abs(b))); }
void upstreamScenarios()
{
    for (const auto packet_size : {std::size_t(0), std::size_t(100), std::size_t(1200), std::numeric_limits<std::size_t>::max()})
    {
        for (const int recv_ms : {20, 22, 17})
        {
            TrendlineEstimator e;
            CHECK(e.State() == Usage::kBwNormal);
            for (int i = 1; i < 25; ++i)
                e.Update(recv_ms, 20, 123456789 + i * 20, 987654321 + i * recv_ms, packet_size, true);
            const auto expected = recv_ms == 20 ? Usage::kBwNormal : (recv_ms > 20 ? Usage::kBwOverusing : Usage::kBwUnderusing);
            CHECK(e.State() == expected && e.Snapshot().num_of_deltas == 24);
        }
    }
}
void filteringAndThresholds()
{
    TrendlineEstimator e;
    CHECK(!e.Snapshot().modified_trend && e.Snapshot().threshold == 12.5);
    e.Update(25, 20, 1000, 2000, 1000, true);
    auto stats = e.Snapshot();
    close(stats.accumulated_delay_ms, 5); close(stats.smoothed_delay_ms, 0.5);
    CHECK(stats.window_samples == 1 && !stats.modified_trend && stats.trend == 0);
    e.Update(25, 20, 1020, 2025, 1000, true);
    stats = e.Snapshot(); close(stats.accumulated_delay_ms, 10); close(stats.smoothed_delay_ms, 1.45);
    CHECK(stats.modified_trend == 0 && stats.threshold == 12.5);
    e.Update(25, 20, 1040, 2050, 1000, true);
    stats = e.Snapshot(); close(stats.smoothed_delay_ms, 2.805); CHECK(stats.threshold == 6);
    for (int i = 3; i < 19; ++i) e.Update(25, 20, 1000 + i * 20, 2000 + i * 25, 1000, true);
    CHECK(e.Snapshot().window_samples == 19 && e.Snapshot().trend == 0);
    e.Update(25, 20, 1380, 2475, 1000, true); CHECK(e.Snapshot().trend > 0);

    TrendlineEstimator normal;
    for (int i = 0; i < 2001; ++i) normal.Update(20, 20, i * 20, 1000 + i * 20, 0, true);
    stats = normal.Snapshot(); CHECK(stats.num_of_deltas == 1000 && stats.window_samples == 20 && stats.threshold == 6);

    TrendlineEstimator clock;
    clock.Update(20, 20, 0, 100000, 1000, true); clock.Update(20, 20, 20, 100020, 1000, true);
    clock.Update(20, 20, 40, 0, 1000, true); CHECK(clock.Snapshot().threshold == 600);
    // Keep upstream's signed adaptive time delta on backwards receive timestamps.
    for (int i = 1; i <= 10; ++i) clock.Update(20, 20, 40 + i * 20, i * 20, 1000, true);
    CHECK(clock.Snapshot().threshold == 6);

    TrendlineEstimator spike;
    for (int i = 0; i < 80; ++i) spike.Update(20, 20, i * 20, 2000 + i * 20, 1200, true);
    const auto threshold = spike.Snapshot().threshold;
    spike.Update(1000000, 20, 1600, 1003580, 1200, true);
    stats = spike.Snapshot(); CHECK(*stats.modified_trend > threshold + 15 && stats.threshold == threshold);
}
void overuseTimerAndDegenerateFit()
{
    TrendlineEstimator::Config config; config.window_size = 10;
    TrendlineEstimator e(config);
    for (int i = 0; i < 10; ++i) e.Update(40, 5, 1000 + i * 5, 2000 + i * 40, 1200, true);
    CHECK(e.State() == Usage::kBwNormal && e.Snapshot().overuse_counter == 1);
    close(e.Snapshot().time_over_using_ms, 2.5);
    e.Update(40, 5, 1050, 2400, 1200, true);
    CHECK(e.State() == Usage::kBwNormal && e.Snapshot().overuse_counter == 2);
    close(e.Snapshot().time_over_using_ms, 7.5);
    e.Update(40, 5, 1055, 2440, 1200, true);
    CHECK(e.State() == Usage::kBwOverusing && e.Snapshot().overuse_counter == 0 && e.Snapshot().time_over_using_ms == 0);
    e.Update(40, 5, 1060, 2480, 1200, true); e.Update(40, 5, 1065, 2520, 1200, true);
    close(e.Snapshot().time_over_using_ms, 10); CHECK(e.Snapshot().overuse_counter == 2); // strictly > 10 ms

    TrendlineEstimator identical(config);
    for (int i = 0; i < 30; ++i) identical.Update(30, 20, i * 20, 3000, 1000, true);
    CHECK(identical.State() == Usage::kBwNormal && identical.Snapshot().trend == 0);
}
void capAndSettings()
{
    TrendlineEstimator::Config config; config.window_size = 10;
    TrendlineEstimator uncapped(config);
    config.enable_cap = true; config.beginning_packets = config.end_packets = 3;
    TrendlineEstimator capped(config);
    for (int i = 0; i < 18; ++i)
    {
        const auto recv = i == 5 ? 40 : (i == 6 ? 0 : 20);
        uncapped.Update(recv, 20, 1000 + i * 20, 2000 + i * 20, 1000, true);
        capped.Update(recv, 20, 1000 + i * 20, 2000 + i * 20, 1000, true);
        if (i == 9) { CHECK(uncapped.Snapshot().trend > 0.01 && capped.Snapshot().trend == 0); }
        if (i == 17) { CHECK(capped.Snapshot().trend < 0); close(capped.Snapshot().trend, uncapped.Snapshot().trend); }
    }
    for (const unsigned invalid : {0U, 9U, 201U, std::numeric_limits<unsigned>::max()})
    {
        config = {}; config.window_size = invalid;
        TrendlineEstimator fallback(config);
        for (int i = 0; i < 25; ++i) fallback.Update(22, 20, i * 20, i * 22, 1000, true);
        CHECK(fallback.Snapshot().window_samples == 20 && fallback.State() == Usage::kBwOverusing);
    }
    for (int invalid = 0; invalid < 3; ++invalid)
    {
        config = {}; config.enable_cap = true;
        if (invalid == 0) config.beginning_packets = 0;
        if (invalid == 1) config.end_packets = std::numeric_limits<unsigned>::max();
        if (invalid == 2) config.beginning_packets = config.end_packets = 11;
        TrendlineEstimator fallback(config), expected;
        for (int i = 0; i < 90; ++i)
        {
            const auto recv = i < 60 ? 22 : 17;
            fallback.Update(recv, 20, i * 20, i * 22, 1000, true);
            expected.Update(recv, 20, i * 20, i * 22, 1000, true);
            close(fallback.Snapshot().trend, expected.Snapshot().trend); CHECK(fallback.State() == expected.State());
        }
    }
    for (const double invalid : {-0.001, 0.026, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        config = {}; config.enable_cap = true; config.cap_uncertainty = invalid;
        TrendlineEstimator fallback(config); config.cap_uncertainty = 0; TrendlineEstimator expected(config);
        for (int i = 0; i < 40; ++i)
        {
            fallback.Update(22, 20, i * 20, i * 22, 1000, true); expected.Update(22, 20, i * 20, i * 22, 1000, true);
            close(fallback.Snapshot().trend, expected.Snapshot().trend); CHECK(fallback.State() == expected.State());
        }
    }
}
class RecordingPredictor : public NetworkStatePredictor
{
public:
    struct Input { std::int64_t send, arrival; Usage raw; };
    std::vector<Input> inputs; Usage predicted{Usage::kBwUnderusing};
    Usage Update(std::int64_t send, std::int64_t arrival, Usage raw) override { inputs.push_back({send, arrival, raw}); return predicted; }
};
void predictorAndValidation()
{
    RecordingPredictor predictor;
    auto owned = std::make_unique<TrendlineEstimator>(TrendlineEstimator::Config{}, &predictor);
    auto* e = owned.get(); std::unique_ptr<DelayIncreaseDetectorInterface> interface = std::move(owned);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    interface->Update(nan, nan, 1000, 2000, 1000, false);
    CHECK(interface->State() == Usage::kBwUnderusing && e->Snapshot().num_of_deltas == 0);
    CHECK(predictor.inputs.size() == 1 && predictor.inputs[0].send == 1000 && predictor.inputs[0].arrival == 2000 && predictor.inputs[0].raw == Usage::kBwNormal);
    e->UpdateTrendline(22, 20, 1020, 2022, 1000);
    CHECK(predictor.inputs.size() == 1 && interface->State() == Usage::kBwUnderusing);
    predictor.predicted = Usage::kBwNormal;
    for (int i = 0; i < 40; ++i) interface->Update(22, 20, 1040 + i * 20, 2044 + i * 22, 1000, true);
    CHECK(e->Snapshot().hypothesis == Usage::kBwOverusing && interface->State() == Usage::kBwNormal);
    CHECK(predictor.inputs.back().raw == Usage::kBwOverusing);
    const auto before = e->Snapshot(); const auto calls = predictor.inputs.size();
    for (int invalid = 0; invalid < 5; ++invalid)
    {
        double recv = 22, send = 20; std::int64_t sent = 2000, arrived = 4000;
        if (invalid == 0) recv = nan;
        if (invalid == 1) send = std::numeric_limits<double>::infinity();
        if (invalid == 2) { recv = std::numeric_limits<double>::max(); send = -recv; }
        if (invalid == 3) sent = -1;
        if (invalid == 4) arrived = -1;
        bool threw = false;
        try { interface->Update(recv, send, sent, arrived, 1000, true); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw && e->Snapshot().num_of_deltas == before.num_of_deltas && predictor.inputs.size() == calls);
        close(e->Snapshot().smoothed_delay_ms, before.smoothed_delay_ms);
    }
}
void upstreamReplay()
{
    struct Expected { double trend, threshold; Usage state; };
    const Expected points[6][3] = {
        {{0.099598916100130483,23.392588628627642,Usage::kBwOverusing}, {-0.15892529994780458,37.520547689480331,Usage::kBwUnderusing}, {0.17644707821072675,12.456565718478373,Usage::kBwOverusing}},
        {{0.10034040194939386,23.856028223725545,Usage::kBwOverusing}, {-0.15916938718806506,37.892355759893157,Usage::kBwUnderusing}, {0.21980643120681959,18.627406714359147,Usage::kBwOverusing}},
        {{0.098452531437577304,23.295511017428854,Usage::kBwOverusing}, {-0.15892529994780458,37.488954114550609,Usage::kBwUnderusing}, {0.17554621255689151,12.343107478687179,Usage::kBwOverusing}},
        {{0.099598916100130483,23.358120803399984,Usage::kBwOverusing}, {-0.15892529994780458,37.520547689480331,Usage::kBwUnderusing}, {0,6,Usage::kBwNormal}},
        {{0.095859295588031343,22.253912178172715,Usage::kBwOverusing}, {-0.15627415767467112,36.350005685626982,Usage::kBwUnderusing}, {0.01,6,Usage::kBwNormal}},
        {{0.10034040194939386,23.856028223725545,Usage::kBwOverusing}, {-0.15916938718806506,37.892355759893157,Usage::kBwUnderusing}, {0.21980643120681959,6,Usage::kBwOverusing}}
    };
    const std::uint64_t expected_hashes[] = {14134899492615407168ULL,15565914188173418804ULL,3141645764742393566ULL,
        2594510782661187448ULL,7539307131059645853ULL,5602143104839949681ULL};
    const int expected_counts[6][3] = {{313,89,198},{312,95,193},{328,85,187},{359,113,128},{360,108,132},{372,106,122}};
    // Golden values generated by the pinned upstream .h/.cc functions, replacing
    // only field-trial/parser/logging dependencies. Settings below are all valid.
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        TrendlineEstimator::Config config;
        if (scenario == 1) config.window_size = 10;
        if (scenario == 2) config.enable_sort = true;
        if (scenario == 3) config.enable_cap = true;
        if (scenario == 4) { config.enable_sort = config.enable_cap = true; config.window_size = 30; config.beginning_packets = config.end_packets = 10; config.cap_uncertainty = 0.01; }
        if (scenario == 5) { config.enable_cap = true; config.window_size = 10; config.beginning_packets = config.end_packets = 3; config.cap_uncertainty = 0.025; }
        TrendlineEstimator e(config); std::uint64_t hash = 14695981039346656037ULL;
        int counts[3] = {}; std::int64_t send_us = 1000000, arrival_us = 2000000; int point = 0;
        for (int i = 0; i < 600; ++i)
        {
            std::int64_t recv_us = i < 50 ? 20000 : (i < 180 ? 22250 : (i < 300 ? 17250 : 20000));
            if (i >= 400 && i < 500) { const std::int64_t jitter[] = {20000,40000,0,20000,21000,19000}; recv_us = jitter[i % 6]; }
            if (i % 149 == 148) recv_us += 500000;
            send_us += 20000; arrival_us += recv_us;
            auto arrived = (arrival_us + 500) / 1000;
            if ((scenario == 2 || scenario == 4) && i % 43 == 0) arrived -= 30;
            e.Update(double(recv_us) / 1000, 20, (send_us + 500) / 1000, arrived, i % 2 ? 100 : 1200, i % 37 != 0);
            const auto state = e.State(); hash = (hash ^ static_cast<std::uint64_t>(state)) * 1099511628211ULL;
            ++counts[static_cast<int>(state)];
            if (i == 100 || i == 250 || i == 450)
            {
                const auto& expected = points[scenario][point++]; const auto stats = e.Snapshot();
                close(stats.trend, expected.trend); close(stats.threshold, expected.threshold); CHECK(state == expected.state);
            }
        }
        CHECK(point == 3 && hash == expected_hashes[scenario]);
        for (int i = 0; i < 3; ++i) CHECK(counts[i] == expected_counts[scenario][i]);
        close(e.Snapshot().accumulated_delay_ms, 1467.75); close(e.Snapshot().smoothed_delay_ms, 1172.5044639770181);
        CHECK(e.Snapshot().num_of_deltas == 583 && e.Snapshot().window_samples == config.window_size);
    }
}
void transportAndInterArrivalPipeline()
{
    using Time = TransportTime;
    for (const int receive_units : {80, 88, 68})
    {
        TransportFeedbackAdapter adapter; InterArrivalDelta groups(5ms); TrendlineEstimator estimator;
        const Time t(100s); int calculated = 0;
        for (int base = 0; base < 100; base += 20)
        {
            for (int i = base; i < base + 20; ++i)
            {
                const auto sent = t + i * 20ms;
                CHECK(adapter.AddPacket({static_cast<std::uint16_t>(i), 100, 42, static_cast<std::uint16_t>(i), false, false}, {}, 28, sent));
                CHECK(adapter.ProcessSentPacket({i, sent, {true, true, 0}}));
            }
            RtcpTwcc report; report.baseSequence = static_cast<std::uint16_t>(base); report.referenceTime = 100;
            report.feedbackCount = static_cast<std::uint8_t>(base / 20); report.deltas.resize(20, receive_units);
            report.deltas[0] = base * receive_units;
            const auto feedback = adapter.ProcessTransportFeedback(report, t + 200ms + (base + 20) * 25ms); CHECK(feedback);
            for (const auto& packet : feedback->SortedByReceiveTime())
            {
                InterArrivalDelta::Delta send_delta{0}, recv_delta{0}; int size_delta = 0;
                const bool valid = groups.ComputeDeltas(*packet.sent_packet.send_time, *packet.receive_time,
                    feedback->feedback_time, packet.sent_packet.size, &send_delta, &recv_delta, &size_delta);
                calculated += valid;
                const auto ms = [](Time at) {
                    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count();
                    return us / 1000 + (us % 1000 >= 500);
                };
                estimator.Update(std::chrono::duration<double, std::milli>(recv_delta).count(),
                    std::chrono::duration<double, std::milli>(send_delta).count(), ms(*packet.sent_packet.send_time),
                    ms(*packet.receive_time), packet.sent_packet.size, valid);
            }
        }
        CHECK(calculated == 98 && estimator.Snapshot().num_of_deltas == 98);
        const auto expected = receive_units == 80 ? Usage::kBwNormal : (receive_units > 80 ? Usage::kBwOverusing : Usage::kBwUnderusing);
        CHECK(estimator.State() == expected && adapter.GetOutstandingData() == 0);
    }
}
} // namespace
int main()
{
    try
    {
        upstreamScenarios(); filteringAndThresholds(); overuseTimerAndDegenerateFit(); capAndSettings();
        predictorAndValidation(); upstreamReplay(); transportAndInterArrivalPipeline();
        std::cout << "PASS: trendline states, filtering, adaptive thresholds, cap, predictor and TWCC pipeline\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
