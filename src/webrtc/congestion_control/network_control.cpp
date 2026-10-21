// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#include"webrtc/congestion_control/network_control.h"
#include"webrtc/congestion_control/rate_math.h"

namespace webrtc
{
PacerConfig PacerConfig::Create(TransportTime at, std::uint64_t send, std::uint64_t pad, std::chrono::microseconds window)
{
    if (window.count() < 0) throw std::invalid_argument("negative pacing time window");
    PacerConfig config;
    config.at_time = at; config.time_window = window;
    config.data_window = cc_detail::bytesForRate(send, config.rate_window());
    config.pad_window = cc_detail::bytesForRate(pad, config.rate_window());
    return config;
}
std::chrono::microseconds PacerConfig::rate_window() const
{ return time_window.count() == 0 ? std::chrono::seconds(1) : time_window; }
std::uint64_t PacerConfig::data_rate() const { return cc_detail::rateForBytes(data_window, rate_window()); }
std::uint64_t PacerConfig::pad_rate() const { return cc_detail::rateForBytes(pad_window, rate_window()); }
} // namespace webrtc
