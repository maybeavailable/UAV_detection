// Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/bandwidth_usage.h"
#include<cstdint>

namespace webrtc
{
// Optional external predictor. TrendlineEstimator does not own this object;
// its lifetime must cover the estimator's lifetime.
class NetworkStatePredictor
{
public:
    virtual ~NetworkStatePredictor() = default;
    virtual BandwidthUsage Update(std::int64_t send_time_ms, std::int64_t arrival_time_ms,
                                  BandwidthUsage network_state) = 0;
};
} // namespace webrtc
