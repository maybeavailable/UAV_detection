// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once
#include<cstddef>
#include<cstdint>
#include<optional>
namespace webrtc
{
class CongestionWindowPushbackController
{
public:
    struct Config
    {
        bool add_pacing{false};
        std::uint64_t min_pushback_target_bitrate{30000};
        std::optional<std::size_t> initial_data_window;
    };
    CongestionWindowPushbackController();
    explicit CongestionWindowPushbackController(Config config);
    void UpdateOutstandingData(std::size_t bytes);
    void UpdatePacingQueue(std::size_t bytes);
    void SetDataWindow(std::size_t bytes);
    std::uint64_t UpdateTargetBitrate(std::uint64_t bitrate);
private:
    Config config_;
    std::optional<std::size_t> current_data_window_;
    std::size_t outstanding_bytes_{0}, pacing_bytes_{0};
    double encoding_rate_ratio_{1.0};
};
} // namespace webrtc
