// Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
// C++17 adaptation; see third_party/webrtc/LICENSE, PATENTS and NOTICE.
#pragma once

#include"webrtc/transport_adapter.h"
#include<map>

namespace webrtc
{
// Received probe packets -> per-cluster send/receive rate -> probe estimate.
// Synchronous: serialize access and rebuild on route/ICE generation changes.
class ProbeBitrateEstimator
{
public:
    ProbeBitrateEstimator();
    ~ProbeBitrateEstimator();
    ProbeBitrateEstimator(const ProbeBitrateEstimator&) = delete;
    ProbeBitrateEstimator& operator=(const ProbeBitrateEstimator&) = delete;

    // Rates use bit/s. Ignores non-probes, lost packets and absent send history.
    // A cluster may yield multiple estimates as more feedback arrives.
    std::optional<std::uint64_t> HandleProbeAndEstimateBitrate(const PacketResult& packet_feedback);
    // Fetches the last successful result, not the maximum. Clears only this
    // cached value; aggregated clusters remain available for later feedback.
    std::optional<std::uint64_t> FetchAndResetLastEstimatedBitrate();

    struct Stats
    {
        std::size_t clusters{0};
        std::optional<std::uint64_t> last_estimated_bitrate;
    };
    Stats Snapshot() const;

private:
    using Timestamp = std::chrono::microseconds;
    struct AggregatedCluster
    {
        std::uint64_t num_probes{0};
        Timestamp first_send{0}, last_send{0}, first_receive{0}, last_receive{0};
        std::uint64_t size_last_send{0}, size_first_receive{0}, size_total{0};
    };
    void EraseOldClusters(Timestamp timestamp);
    std::map<int, AggregatedCluster> clusters_;
    std::optional<std::uint64_t> estimated_data_rate_;
};
} // namespace webrtc
