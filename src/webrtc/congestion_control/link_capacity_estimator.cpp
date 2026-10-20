/*
 * Copyright 2018 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/link_capacity_estimator.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/link_capacity_estimator.h"
#include"log/log.h"

#include<algorithm>
#include<cmath>
#include<limits>

namespace webrtc
{
namespace
{
std::uint64_t rateFromKbps(double kbps)
{
    // DataRate::KilobitsPerSec(double) truncates the double product to bps.
    // Saturate values outside the project's uint64_t rate representation.
    const auto bps = kbps * 1000;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    return static_cast<long double>(bps) >= static_cast<long double>(max) ? max : static_cast<std::uint64_t>(bps);
}
} // namespace

LinkCapacityEstimator::LinkCapacityEstimator() = default;
std::optional<std::uint64_t> LinkCapacityEstimator::UpperBound() const
{
    if (!estimate_kbps_) return std::nullopt;
    return rateFromKbps(*estimate_kbps_ + 3 * deviation_estimate_kbps());
}
std::uint64_t LinkCapacityEstimator::LowerBound() const
{
    if (!estimate_kbps_) return 0;
    return rateFromKbps(std::max(0.0, *estimate_kbps_ - 3 * deviation_estimate_kbps()));
}
void LinkCapacityEstimator::Reset()
{
    if (estimate_kbps_) UAV_LOG_DEBUG << "[WebRTC][LinkCapacityEstimator] reset estimate_bps=" << *estimate();
    estimate_kbps_.reset();
}
void LinkCapacityEstimator::OnOveruseDetected(std::uint64_t acknowledged_rate_bps) { Update(acknowledged_rate_bps, 0.05); }
void LinkCapacityEstimator::OnProbeRate(std::uint64_t probe_rate_bps)
{
    Update(probe_rate_bps, 0.5);
    UAV_LOG_DEBUG << "[WebRTC][LinkCapacityEstimator] probe_bps=" << probe_rate_bps << " estimate_bps=" << *estimate();
}
void LinkCapacityEstimator::Update(std::uint64_t capacity_sample_bps, double alpha)
{
    // Upstream calls kbps() with the default integer type, which rounds to
    // nearest kbit/s before conversion to double. Avoid bps + 500 overflowing.
    const double sample_kbps = static_cast<double>(capacity_sample_bps / 1000 + (capacity_sample_bps % 1000 >= 500));
    const bool initialized = estimate_kbps_.has_value();
    if (!initialized) estimate_kbps_ = sample_kbps;
    else estimate_kbps_ = (1 - alpha) * *estimate_kbps_ + alpha * sample_kbps;

    const auto norm = std::max(*estimate_kbps_, 1.0);
    const auto error_kbps = *estimate_kbps_ - sample_kbps; // residual against the updated mean
    deviation_kbps_ = (1 - alpha) * deviation_kbps_ + alpha * error_kbps * error_kbps / norm;
    // The original clamp's float lower bound is intentionally retained.
    deviation_kbps_ = std::clamp(deviation_kbps_, static_cast<double>(0.4f), static_cast<double>(2.5f));
    if (!initialized) UAV_LOG_DEBUG << "[WebRTC][LinkCapacityEstimator] initialized estimate_bps=" << *estimate();
}
bool LinkCapacityEstimator::has_estimate() const { return estimate_kbps_.has_value(); }
std::optional<std::uint64_t> LinkCapacityEstimator::estimate() const
{
    if (!estimate_kbps_) return std::nullopt;
    return rateFromKbps(*estimate_kbps_);
}
double LinkCapacityEstimator::deviation_estimate_kbps() const { return std::sqrt(deviation_kbps_ * *estimate_kbps_); }
} // namespace webrtc
