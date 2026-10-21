// Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
// Adapted from libwebrtc 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
// See third_party/webrtc/LICENSE, PATENTS and NOTICE.
#include"webrtc/congestion_control/congestion_window_pushback_controller.h"
#include"webrtc/congestion_control/rate_math.h"
#include"log/log.h"
#include<algorithm>

namespace webrtc
{
CongestionWindowPushbackController::CongestionWindowPushbackController() : CongestionWindowPushbackController(Config{}) {}
CongestionWindowPushbackController::CongestionWindowPushbackController(Config config)
    : config_(config), current_data_window_(config.initial_data_window) {}
void CongestionWindowPushbackController::UpdateOutstandingData(std::size_t bytes) { outstanding_bytes_ = bytes; }
void CongestionWindowPushbackController::UpdatePacingQueue(std::size_t bytes) { pacing_bytes_ = bytes; }
void CongestionWindowPushbackController::SetDataWindow(std::size_t bytes) { current_data_window_ = bytes; }
std::uint64_t CongestionWindowPushbackController::UpdateTargetBitrate(std::uint64_t bitrate)
{
    if (!current_data_window_ || !*current_data_window_) return bitrate;
    const double total = static_cast<double>(outstanding_bytes_) + (config_.add_pacing ? static_cast<double>(pacing_bytes_) : 0);
    const double fill = total / static_cast<double>(*current_data_window_);
    if (fill > 1.5) encoding_rate_ratio_ *= 0.9;
    else if (fill > 1) encoding_rate_ratio_ *= 0.95;
    else if (fill < 0.1) encoding_rate_ratio_ = 1.0;
    else encoding_rate_ratio_ = std::min(encoding_rate_ratio_ * 1.05, 1.0);
    const auto adjusted = cc_detail::bounded(static_cast<double>(bitrate) * encoding_rate_ratio_); // truncate, not round
    const auto target = adjusted < config_.min_pushback_target_bitrate ? std::min(bitrate, config_.min_pushback_target_bitrate) : adjusted;
    UAV_LOG_IF(LogLevel::TRACE, LogManager::getInstance().shouldLog(LogLevel::TRACE)) << "[WebRTC][CwndPushback][" << this
        << "] window_bytes=" << *current_data_window_ << " fill_ratio=" << fill << " encoding_ratio=" << encoding_rate_ratio_
        << " input_bps=" << bitrate << " target_bps=" << target;
    return target;
}
} // namespace webrtc
