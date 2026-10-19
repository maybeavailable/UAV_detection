// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include<chrono>
#include<cstddef>
#include<cstdint>

namespace webrtc
{
// Byte budget with libwebrtc IntervalBudget semantics: a 500 ms signed window,
// whole-packet overdraft, and optional accumulation of unused positive budget.
// Rates stay in bit/s; by default fractional bytes survive processing intervals.
class IntervalBudget
{
public:
    static constexpr std::chrono::milliseconds Window{500};
    static constexpr std::uint64_t MaxRateBps{1000000000};

    enum class Rounding { CarryFractionalBytes, TruncateBytes };
    explicit IntervalBudget(std::uint64_t targetRateBps = 0, bool canBuildUpUnderuse = false,
                            Rounding rounding = Rounding::CarryFractionalBytes);
    bool setTargetRate(std::uint64_t rateBps); // invalid rates leave the budget unchanged
    void increaseBudget(std::chrono::microseconds elapsed);
    void useBudget(std::size_t bytes);
    void discardPositive(); // idle/pause must not save credit for a later burst
    void reset(); // retains target rate and accumulation policy

    std::uint64_t targetRateBps() const { return rateBps_; }
    std::size_t bytesRemaining() const;
    std::int64_t signedBytesRemaining() const { return remaining_; }
    std::int64_t maxBytesInBudget() const { return limit_; }
    double budgetRatio() const;
private:
    std::uint64_t rateBps_{0}, fractional_{0};
    std::int64_t remaining_{0}, limit_{0};
    bool canBuildUpUnderuse_{false};
    Rounding rounding_{Rounding::CarryFractionalBytes};
};
} // namespace webrtc
