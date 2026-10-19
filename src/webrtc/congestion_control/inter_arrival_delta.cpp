/*
 * Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/inter_arrival_delta.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/inter_arrival_delta.h"
#include"log/log.h"

#include<algorithm>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
constexpr auto kBurstDeltaThreshold = std::chrono::milliseconds(5);
constexpr auto kMaxBurstDuration = std::chrono::milliseconds(100);
} // namespace

InterArrivalDelta::InterArrivalDelta(Delta send_time_group_length)
    : send_time_group_length_(send_time_group_length)
{
    if (send_time_group_length < Delta::zero())
        throw std::invalid_argument("negative inter-arrival send time group length");
}

bool InterArrivalDelta::ComputeDeltas(Time send, Time arrival, Time system, std::size_t packet_size,
                                    Delta* send_time_delta, Delta* arrival_time_delta, int* packet_size_delta)
{
    constexpr auto max_size = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (!send_time_delta || !arrival_time_delta || !packet_size_delta || packet_size > max_size)
        throw std::invalid_argument("invalid inter-arrival output pointer or packet size");

    // Subtract epoch microseconds instead of time_points: even the difference
    // between steady_clock's minimum and maximum nanosecond times stays safe.
    const auto send_time = std::chrono::duration_cast<Delta>(send.time_since_epoch());
    const auto arrival_time = std::chrono::duration_cast<Delta>(arrival.time_since_epoch());
    const auto system_time = std::chrono::duration_cast<Delta>(system.time_since_epoch());
    bool calculated_deltas = false;
    auto& current = current_timestamp_group_;
    const auto& previous = prev_timestamp_group_;
    if (current.IsFirstPacket())
    {
        current.send_time = send_time;
        current.first_send_time = send_time;
        current.first_arrival = arrival_time;
    }
    else if (current.first_send_time > send_time)
    {
        // Ignore older packets without modifying either group.
        return false;
    }
    else if (NewTimestampGroup(arrival_time, send_time))
    {
        if (previous.complete_time)
        {
            *send_time_delta = current.send_time - previous.send_time;
            *arrival_time_delta = *current.complete_time - *previous.complete_time;
            const auto system_time_delta = current.last_system_time - previous.last_system_time;
            if (*arrival_time_delta - system_time_delta >= kArrivalTimeOffsetThreshold)
            {
                UAV_LOG_WARN << "[WebRTC][InterArrivalDelta] arrival clock offset changed diff_us="
                    << (*arrival_time_delta - system_time_delta).count() << ", resetting";
                Reset();
                return false;
            }
            if (*arrival_time_delta < Delta::zero())
            {
                if (++num_consecutive_reordered_packets_ >= kReorderedResetThreshold)
                {
                    UAV_LOG_WARN << "[WebRTC][InterArrivalDelta] reordered groups arrival_delta_us="
                        << arrival_time_delta->count() << " send_delta_us=" << send_time_delta->count()
                        << ", resetting";
                    Reset();
                }
                // Upstream retains the two groups and drops this packet until
                // recovery or the reset threshold. Do not advance the groups.
                return false;
            }
            num_consecutive_reordered_packets_ = 0;
            *packet_size_delta = static_cast<int>(current.size) - static_cast<int>(previous.size);
            calculated_deltas = true;
        }
        prev_timestamp_group_ = current;
        current.first_send_time = send_time;
        current.send_time = send_time;
        current.first_arrival = arrival_time;
        current.size = 0;
    }
    else
    {
        if (packet_size > max_size - current.size)
            throw std::overflow_error("inter-arrival group size exceeds int range");
        current.send_time = std::max(current.send_time, send_time);
    }
    current.size += packet_size;
    current.complete_time = arrival_time;
    current.last_system_time = system_time;
    return calculated_deltas;
}

bool InterArrivalDelta::NewTimestampGroup(Delta arrival_time, Delta send_time) const
{
    if (current_timestamp_group_.IsFirstPacket() || BelongsToBurst(arrival_time, send_time)) return false;
    return send_time - current_timestamp_group_.first_send_time > send_time_group_length_;
}
bool InterArrivalDelta::BelongsToBurst(Delta arrival_time, Delta send_time) const
{
    const auto& current = current_timestamp_group_;
    const auto arrival_delta = arrival_time - *current.complete_time;
    const auto send_delta = send_time - current.send_time;
    if (send_delta == Delta::zero()) return true;
    const auto propagation_delta = arrival_delta - send_delta;
    return propagation_delta < Delta::zero() && arrival_delta <= kBurstDeltaThreshold &&
           arrival_time - current.first_arrival < kMaxBurstDuration;
}
void InterArrivalDelta::Reset()
{
    num_consecutive_reordered_packets_ = 0;
    current_timestamp_group_ = SendTimeGroup{};
    prev_timestamp_group_ = SendTimeGroup{};
}
} // namespace webrtc
