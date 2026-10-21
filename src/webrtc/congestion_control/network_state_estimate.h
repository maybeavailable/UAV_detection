// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include<cstdint>
#include<chrono>
#include<optional>

namespace webrtc
{
// The capacity bounds consumed by AIMD/probing from upstream NetworkStateEstimate.
// Rates are bit/s. nullopt means no finite bound; zero is a finite bound.
struct NetworkStateEstimate
{
    std::optional<std::uint64_t> link_capacity_lower;
    std::optional<std::uint64_t> link_capacity_upper;
    std::optional<std::chrono::steady_clock::time_point> update_time{};
};
} // namespace webrtc
