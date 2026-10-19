// Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include<chrono>
#include<cstddef>
#include<optional>

namespace webrtc
{
// Inter-group send/arrival/size deltas for the delay-based bandwidth estimator.
// Synchronous: the owning transport/controller must serialize access.
class InterArrivalDelta
{
public:
    using Time = std::chrono::steady_clock::time_point;
    using Delta = std::chrono::microseconds;
    static constexpr int kReorderedResetThreshold = 3;
    static constexpr Delta kArrivalTimeOffsetThreshold{std::chrono::seconds(3)};

    explicit InterArrivalDelta(Delta send_time_group_length); // normally 5 ms; negative throws
    InterArrivalDelta() = delete;
    InterArrivalDelta(const InterArrivalDelta&) = delete;
    InterArrivalDelta& operator=(const InterArrivalDelta&) = delete;

    // Times are quantized to microseconds, like upstream Timestamp. system_time
    // is local feedback processing time; arrival_time is the receiver timeline.
    // Returns true once two groups are complete. Outputs are valid only on true.
    // Output pointers must be non-null; packet/group sizes must fit in int.
    bool ComputeDeltas(Time send_time, Time arrival_time, Time system_time, std::size_t packet_size,
                       Delta* send_time_delta, Delta* arrival_time_delta, int* packet_size_delta);

private:
    struct SendTimeGroup
    {
        bool IsFirstPacket() const { return !complete_time; }
        std::size_t size{0};
        Delta first_send_time{0}, send_time{0}, first_arrival{0}, last_system_time{0};
        std::optional<Delta> complete_time; // upstream MinusInfinity -> nullopt
    };
    bool NewTimestampGroup(Delta arrival_time, Delta send_time) const;
    bool BelongsToBurst(Delta arrival_time, Delta send_time) const;
    void Reset();

    const Delta send_time_group_length_;
    SendTimeGroup current_timestamp_group_, prev_timestamp_group_;
    int num_consecutive_reordered_packets_{0};
};
} // namespace webrtc
