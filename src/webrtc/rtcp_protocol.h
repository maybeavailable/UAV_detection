#pragma once

#include<cstddef>
#include<cstdint>
#include<optional>
#include<string>
#include<variant>
#include<vector>

namespace webrtc
{
using RtcpBytes = std::vector<std::uint8_t>;
struct RtcpReceptionReport
{
    std::uint32_t ssrc{0};
    std::uint8_t fractionLost{0};
    std::int32_t cumulativeLost{0}; // signed 24 bits
    std::uint32_t highestSequence{0}, jitter{0}, lsr{0}, dlsr{0};
};
struct RtcpReport
{
    std::uint32_t senderSsrc{0};
    bool senderReport{false};
    std::uint64_t ntp{0};
    std::uint32_t rtpTimestamp{0}, packetCount{0}, octetCount{0};
    std::vector<RtcpReceptionReport> reports;
};
struct RtcpNack
{
    std::uint32_t senderSsrc{0}, mediaSsrc{0};
    std::vector<std::uint16_t> sequences;
};
struct RtcpKeyFrameRequest
{
    enum class Kind { Pli, Fir };
    std::uint32_t senderSsrc{0}, mediaSsrc{0};
    Kind kind{Kind::Pli};
    std::uint8_t sequence{0}; // FIR command sequence
};
struct RtcpTwcc
{
    std::uint32_t senderSsrc{0}, mediaSsrc{0}, referenceTime{0}; // 24-bit, 64 ms units
    std::uint16_t baseSequence{0};
    std::uint8_t feedbackCount{0};
    // nullopt = not received; deltas relative to previous received packet (first: referenceTime).
    // Unit 250 us; small unsigned 8-bit or signed 16-bit on the wire.
    std::vector<std::optional<std::int32_t>> deltas;
};
using RtcpMessage = std::variant<RtcpReport, RtcpNack, RtcpKeyFrameRequest, RtcpTwcc>;

// Compound / reduced-size RTCP. Unknown packet types/FMT are skipped; malformed input
// returns false and leaves out unchanged. Allocation bounds include <=8192 TWCC statuses.
bool parseRtcp(const std::uint8_t* data, std::size_t size, std::vector<RtcpMessage>& out);
RtcpBytes buildRtcpReport(const RtcpReport& report);
RtcpBytes buildRtcpSdes(std::uint32_t ssrc, const std::string& cname);
RtcpBytes buildRtcpNack(const RtcpNack& nack);
RtcpBytes buildRtcpKeyFrameRequest(const RtcpKeyFrameRequest& request);
RtcpBytes buildRtcpTwcc(const RtcpTwcc& feedback);

// RFC 8285 one-byte / two-byte extensions. Preserve other extensions, CSRC, payload and padding.
std::optional<std::uint16_t> rtpTransportSequence(const RtcpBytes& packet, std::uint8_t extensionId);
bool setRtpTransportSequence(RtcpBytes& packet, std::uint8_t extensionId, std::uint16_t sequence);
} // namespace webrtc
