/*
 * Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 * Adapted from modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator.cc,
 * revision 602d5fc23caaca7728ad5f3f0e68b92274d1a38a.
 * See third_party/webrtc/LICENSE, PATENTS and NOTICE.
 */
#include"webrtc/congestion_control/acknowledged_bitrate_estimator.h"
#include"log/log.h"

#include<algorithm>
#include<limits>
#include<stdexcept>
#include<tuple>
#include<utility>

namespace webrtc
{
AcknowledgedBitrateEstimator::AcknowledgedBitrateEstimator() : AcknowledgedBitrateEstimator(Config{}) {}
AcknowledgedBitrateEstimator::AcknowledgedBitrateEstimator(Config config)
    : AcknowledgedBitrateEstimator(std::make_unique<BitrateEstimator>(config)) {}
AcknowledgedBitrateEstimator::AcknowledgedBitrateEstimator(std::unique_ptr<BitrateEstimator> estimator)
    : bitrate_estimator_(std::move(estimator))
{
    if (!bitrate_estimator_) throw std::invalid_argument("null bitrate estimator");
}
AcknowledgedBitrateEstimator::~AcknowledgedBitrateEstimator() = default;

void AcknowledgedBitrateEstimator::IncomingPacketFeedbackVector(const std::vector<PacketResult>& packets)
{
    // Upstream DCHECKs the ordering. Validate the entire C++17 input in release
    // builds too, so optional timestamps cannot be dereferenced accidentally.
    const auto valid = [](const PacketResult& p) {
        return p.receive_time && p.sent_packet.send_time && p.sent_packet.prior_unacked_data <=
            std::numeric_limits<std::size_t>::max() - p.sent_packet.size;
    };
    const auto order = [](const PacketResult& a, const PacketResult& b) {
        return std::tie(a.receive_time, a.sent_packet.send_time, a.sent_packet.sequence_number) <
               std::tie(b.receive_time, b.sent_packet.send_time, b.sent_packet.sequence_number);
    };
    if (!std::all_of(packets.begin(), packets.end(), valid) || !std::is_sorted(packets.begin(), packets.end(), order))
        throw std::invalid_argument("acknowledged feedback must contain sorted received packets with send info");
    for (const auto& packet : packets)
    {
        if (alr_ended_time_ && *packet.sent_packet.send_time > *alr_ended_time_)
        {
            bitrate_estimator_->ExpectFastRateChange();
            alr_ended_time_.reset();
        }
        bitrate_estimator_->Update(*packet.receive_time,
            packet.sent_packet.size + packet.sent_packet.prior_unacked_data, in_alr_);
    }
}
std::optional<std::uint64_t> AcknowledgedBitrateEstimator::bitrate() const { return bitrate_estimator_->bitrate(); }
std::optional<std::uint64_t> AcknowledgedBitrateEstimator::PeekRate() const { return bitrate_estimator_->PeekRate(); }
void AcknowledgedBitrateEstimator::SetAlr(bool in_alr)
{
    if (in_alr_ != in_alr) UAV_LOG_DEBUG << "[WebRTC][AcknowledgedBitrateEstimator] in_alr=" << in_alr;
    in_alr_ = in_alr;
}
void AcknowledgedBitrateEstimator::SetAlrEndedTime(Time at) { alr_ended_time_ = at; }
} // namespace webrtc
