// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/delay_increase_detector_interface.h"
#include"webrtc/congestion_control/network_state_predictor.h"
#include<deque>
#include<optional>

namespace webrtc
{
struct TrendlineEstimatorSettings
{
    static constexpr unsigned kDefaultTrendlineWindowSize = 20;
    // WebRTC-Bwe-TrendlineEstimatorSettings, without FieldTrialsView parsing.
    bool enable_sort{false}, enable_cap{false};
    unsigned beginning_packets{7}, end_packets{7};
    double cap_uncertainty{0.0};
    unsigned window_size{kDefaultTrendlineWindowSize}; // 10..200; invalid values use 20
};

// Delay trend and overuse/underuse detection, as in libwebrtc goog_cc.
// Synchronous: the owning transport/controller must serialize access.
class TrendlineEstimator : public DelayIncreaseDetectorInterface
{
public:
    using Config = TrendlineEstimatorSettings;
    TrendlineEstimator();
    explicit TrendlineEstimator(Config config, NetworkStatePredictor* network_state_predictor = nullptr);
    ~TrendlineEstimator() override;
    TrendlineEstimator(const TrendlineEstimator&) = delete;
    TrendlineEstimator& operator=(const TrendlineEstimator&) = delete;

    // Deltas come from InterArrivalDelta groups, not consecutive individual
    // packets. Timestamps are nonnegative integer milliseconds; deltas retain
    // fractional milliseconds. False skips fitting but still calls the predictor.
    void Update(double recv_delta_ms, double send_delta_ms, std::int64_t send_time_ms,
                std::int64_t arrival_time_ms, std::size_t packet_size, bool calculated_deltas) override;
    // Raw detector only: this entry does not update an injected predictor.
    void UpdateTrendline(double recv_delta_ms, double send_delta_ms, std::int64_t send_time_ms,
                         std::int64_t arrival_time_ms, std::size_t packet_size);
    BandwidthUsage State() const override;

    struct PacketTiming
    {
        double arrival_time_ms, smoothed_delay_ms, raw_delay_ms;
    };
    struct Stats
    {
        std::size_t window_samples{0};
        int num_of_deltas{0};
        double accumulated_delay_ms{0}, smoothed_delay_ms{0}, trend{0};
        std::optional<double> modified_trend;
        double threshold{12.5}, time_over_using_ms{-1};
        int overuse_counter{0};
        BandwidthUsage hypothesis{BandwidthUsage::kBwNormal}, state{BandwidthUsage::kBwNormal};
    };
    Stats Snapshot() const; // diagnostics; this class produces a usage state, not a bitrate

private:
    void Detect(double trend, double ts_delta, std::int64_t now_ms);
    void UpdateThreshold(double modified_trend, std::int64_t now_ms);

    const Config settings_;
    int num_of_deltas_{0};
    std::int64_t first_arrival_time_ms_{-1};
    double accumulated_delay_{0}, smoothed_delay_{0};
    std::deque<PacketTiming> delay_hist_;
    double threshold_{12.5};
    std::optional<double> prev_modified_trend_;
    std::int64_t last_update_ms_{-1};
    double prev_trend_{0}, time_over_using_{-1};
    int overuse_counter_{0};
    BandwidthUsage hypothesis_{BandwidthUsage::kBwNormal}, hypothesis_predicted_{BandwidthUsage::kBwNormal};
    NetworkStatePredictor* const network_state_predictor_;
};
} // namespace webrtc
