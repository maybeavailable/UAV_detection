// Copyright (c) 2024 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

namespace webrtc
{
enum class BandwidthUsage
{
    kBwNormal = 0,
    kBwUnderusing = 1,
    kBwOverusing = 2,
    kLast
};
} // namespace webrtc
