// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#include"webrtc/congestion_control/interval_budget.h"

#include<algorithm>
#include<stdexcept>

namespace webrtc
{
IntervalBudget::IntervalBudget(std::uint64_t rate, bool accumulate, Rounding rounding)
    : canBuildUpUnderuse_(accumulate), rounding_(rounding)
{
    if (!setTargetRate(rate)) throw std::invalid_argument("invalid interval budget rate");
}
bool IntervalBudget::setTargetRate(std::uint64_t rate)
{
    if (rate > MaxRateBps) return false;
    rateBps_ = rate;
    limit_ = static_cast<std::int64_t>(rate * Window.count() / 8000);
    remaining_ = std::clamp(remaining_, -limit_, limit_);
    if (!rate) fractional_ = 0;
    return true;
}
void IntervalBudget::increaseBudget(std::chrono::microseconds elapsed)
{
    if (elapsed.count() <= 0 || !rateBps_) return;
    // From the lowest debt to the highest credit takes two windows. Capping
    // elapsed here only prevents overflow; Pacer separately caps late ticks.
    const auto us = static_cast<std::uint64_t>(std::min(elapsed, std::chrono::microseconds(2 * Window)).count());
    constexpr std::uint64_t byteUnits = 8000000;
    const auto earned = rateBps_ * us + fractional_;
    const auto bytes = static_cast<std::int64_t>(earned / byteUnits);
    fractional_ = rounding_ == Rounding::CarryFractionalBytes ? earned % byteUnits : 0;
    remaining_ = std::min(limit_, bytes + ((remaining_ < 0 || canBuildUpUnderuse_) ? remaining_ : 0));
}
void IntervalBudget::useBudget(std::size_t bytes)
{
    // Compare before converting to signed so even SIZE_MAX saturates safely.
    const auto available = static_cast<std::uint64_t>(remaining_ + limit_);
    if (bytes >= available) remaining_ = -limit_;
    else remaining_ -= static_cast<std::int64_t>(bytes);
}
void IntervalBudget::discardPositive()
{
    if (remaining_ >= 0) { remaining_ = 0; fractional_ = 0; }
}
void IntervalBudget::reset() { remaining_ = 0; fractional_ = 0; }
std::size_t IntervalBudget::bytesRemaining() const { return static_cast<std::size_t>(std::max<std::int64_t>(remaining_, 0)); }
double IntervalBudget::budgetRatio() const { return limit_ ? static_cast<double>(remaining_) / static_cast<double>(limit_) : 0; }
} // namespace webrtc
