// Copyright 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include<cstdint>
#include<optional>

namespace webrtc
{
// Link-capacity mean and normalized variance, as in libwebrtc goog_cc.
// Synchronous: the owning transport/controller must serialize access.
class LinkCapacityEstimator
{
public:
    LinkCapacityEstimator();
    // All public rates use bit/s. nullopt upper bound means unbounded/unknown,
    // corresponding to upstream DataRate::Infinity(). Lower bound is then 0.
    std::optional<std::uint64_t> UpperBound() const;
    std::uint64_t LowerBound() const;
    void Reset(); // clears the mean, retains normalized variance like upstream
    void OnOveruseDetected(std::uint64_t acknowledged_rate_bps);
    void OnProbeRate(std::uint64_t probe_rate_bps);
    bool has_estimate() const;
    std::optional<std::uint64_t> estimate() const; // nullopt until a sample; zero is a valid estimate

private:
    void Update(std::uint64_t capacity_sample_bps, double alpha);
    double deviation_estimate_kbps() const; // only valid when the mean is present
    std::optional<double> estimate_kbps_;
    double deviation_kbps_{0.4};
};
} // namespace webrtc
