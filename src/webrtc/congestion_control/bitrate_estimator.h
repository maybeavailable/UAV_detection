// Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include<chrono>
#include<cstddef>
#include<cstdint>
#include<optional>

namespace webrtc
{
// Bayesian estimate of acknowledged throughput, as in libwebrtc goog_cc.
// Synchronous: the owning transport/controller must serialize access.
class BitrateEstimator
{
public:
    using Time = std::chrono::steady_clock::time_point;
    struct Config
    {
        // WebRTC-BweThroughputWindowConfig, expressed without FieldTrialsView.
        std::chrono::milliseconds initial_window{500}, window{150}; // each 150..1000 ms
        double uncertainty_scale{10.0}, uncertainty_scale_in_alr{10.0};
        double small_sample_uncertainty_scale{10.0};
        std::size_t small_sample_threshold{0}; // bytes, strict less-than comparison
        std::uint64_t uncertainty_symmetry_cap_bps{0}, estimate_floor_bps{0};
    };

    BitrateEstimator();
    explicit BitrateEstimator(Config config); // invalid configuration throws invalid_argument
    virtual ~BitrateEstimator();
    virtual void Update(Time at_time, std::size_t amount, bool in_alr);
    virtual std::optional<std::uint64_t> bitrate() const; // bit/s; nullopt until the first full window
    std::optional<std::uint64_t> PeekRate() const; // unfiltered current-window rate, bit/s
    virtual void ExpectFastRateChange();

private:
    float UpdateWindow(std::int64_t now_ms, std::size_t bytes, std::int64_t rate_window_ms,
                       bool* is_small_sample);
    Config config_;
    std::size_t sum_{0};
    std::int64_t current_window_ms_{0};
    std::optional<std::int64_t> prev_time_ms_;
    float bitrate_estimate_kbps_{-1.0f}, bitrate_estimate_var_{50.0f};
};
} // namespace webrtc
