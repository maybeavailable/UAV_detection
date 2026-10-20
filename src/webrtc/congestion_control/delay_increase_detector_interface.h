// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/bandwidth_usage.h"
#include<cstddef>
#include<cstdint>

namespace webrtc
{
class DelayIncreaseDetectorInterface
{
public:
    DelayIncreaseDetectorInterface() = default;
    virtual ~DelayIncreaseDetectorInterface() = default;
    DelayIncreaseDetectorInterface(const DelayIncreaseDetectorInterface&) = delete;
    DelayIncreaseDetectorInterface& operator=(const DelayIncreaseDetectorInterface&) = delete;
    virtual void Update(double recv_delta_ms, double send_delta_ms, std::int64_t send_time_ms,
                        std::int64_t arrival_time_ms, std::size_t packet_size, bool calculated_deltas) = 0;
    virtual BandwidthUsage State() const = 0;
};
} // namespace webrtc
