// Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/congestion_control/bitrate_estimator.h"
#include"webrtc/transport_adapter.h"
#include<memory>

namespace webrtc
{
// Consumes received packets with send info, ordered by receive/send/sequence.
// Pass TransportPacketsFeedback::SortedByReceiveTime(), once per feedback.
// All methods must run on the owning transport/controller's serialized context.
class AcknowledgedBitrateEstimator
{
public:
    using Time = TransportTime;
    using Config = BitrateEstimator::Config;
    AcknowledgedBitrateEstimator();
    explicit AcknowledgedBitrateEstimator(Config config);
    explicit AcknowledgedBitrateEstimator(std::unique_ptr<BitrateEstimator> bitrate_estimator);
    ~AcknowledgedBitrateEstimator();

    // Invalid/missing timestamps, unsorted input or overflowing packet sizes
    // throw invalid_argument before any samples are consumed. Empty is valid.
    void IncomingPacketFeedbackVector(const std::vector<PacketResult>& packet_feedback_vector);
    std::optional<std::uint64_t> bitrate() const; // acknowledged throughput in bit/s
    std::optional<std::uint64_t> PeekRate() const;
    void SetAlr(bool in_alr);
    void SetAlrEndedTime(Time alr_ended_time); // local send-clock time, not remote receive time

private:
    std::optional<Time> alr_ended_time_;
    bool in_alr_{false};
    std::unique_ptr<BitrateEstimator> bitrate_estimator_;
};
} // namespace webrtc
