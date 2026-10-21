// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include<chrono>
#include<cstdint>

namespace webrtc
{
// Probe request, consumed by the owner's probing/pacing engine. Rates are bit/s.
struct ProbeClusterConfig
{
    std::chrono::steady_clock::time_point at_time{};
    std::uint64_t target_data_rate{0};
    std::chrono::microseconds target_duration{0};
    std::chrono::microseconds min_probe_delta{std::chrono::milliseconds(2)};
    std::int32_t target_probe_count{0}, id{0};
};
} // namespace webrtc
