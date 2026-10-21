#pragma once

#include"webrtc/rtcp_protocol.h"
#include"webrtc/transport_adapter.h"
#include<chrono>
#include<functional>
#include<memory>

namespace webrtc
{
struct RtcpMediaParameters
{
    std::uint8_t payloadType{0};
    std::uint32_t clockRate{0};
    bool send{true}, receive{true}, nack{false}, pli{false}, fir{false}, transportCc{false};
    std::optional<std::uint8_t> sendTwccExtension, receiveTwccExtension;
    bool audio{false}; // negotiated media kind; used by the transport's pacer
};
struct RtcpParameters
{
    std::vector<RtcpMediaParameters> media;
    bool reducedSize{false}; // SDP rtcp-rsize; otherwise feedback includes RR + SDES
};
bool validateRtcpParameters(const RtcpParameters& parameters);
struct RtcpReportEvent
{
    std::uint64_t generation{0};
    std::uint32_t reporterSsrc{0};
    RtcpReceptionReport report;
    std::optional<double> rttMs;
};
struct RtcpNackEvent
{
    std::uint64_t generation{0};
    RtcpNack feedback;
    std::vector<std::uint16_t> cacheMisses;
};
struct RtcpKeyFrameEvent
{
    std::uint64_t generation{0};
    RtcpKeyFrameRequest feedback; // encoder should produce a key frame for mediaSsrc
};
struct TwccPacketResult
{
    std::uint64_t transportSequence{0};
    std::uint32_t ssrc{0};
    std::uint16_t rtpSequence{0};
    std::size_t wireSize{0};
    std::chrono::steady_clock::time_point sentAt;
    // Receiver's clock, unwrapped across reference-time rollover; NOT local wall clock.
    std::optional<std::int64_t> remoteArrivalUs;
    bool ambiguousReceiveTime{false};
};
struct RtcpRembEvent { std::uint64_t generation{0}; RtcpRemb feedback; };
struct TwccFeedbackEvent
{
    std::uint64_t generation{0};
    std::uint8_t feedbackCount{0};
    std::vector<TwccPacketResult> packets; // only known history entries with changed status
    std::size_t received{0}, notReceived{0}, receivedBytes{0};
    std::chrono::steady_clock::time_point feedbackAt{};
    std::vector<TransportPacketDelta> packetDeltas;
};
struct RtcpStats
{
    std::uint64_t generation{0};
    std::uint64_t sentSenderReports{0}, sentReceiverReports{0}, receivedReports{0};
    std::uint64_t sentNacks{0}, receivedNacks{0}, retransmissions{0}, cacheMisses{0};
    std::uint64_t sentPlis{0}, receivedKeyFrameRequests{0};
    std::uint64_t sentTwcc{0}, receivedTwcc{0}, malformedPackets{0}, droppedOutputs{0};
    std::size_t sendStreams{0}, receiveStreams{0}, cachedPackets{0}, cachedBytes{0}, twccHistory{0};
    std::optional<double> lastRttMs;
    TransportAdapterStats transport;
};

// Synchronous RTCP engine, owned and serialized by DtlsTransport's strand.
// Only authenticated RTP/RTCP may enter. reset clears all history and timers for a new generation.
class RtcpSession
{
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Config
    {
        bool enabled{true};
        std::uint32_t senderSsrc{0}; // zero chooses a random receiver SSRC
        std::string cname; // empty chooses a per-session CNAME
        std::chrono::milliseconds reportInterval{1000}, tickInterval{10}, twccInterval{50};
        std::chrono::milliseconds nackDelay{20}, nackRetry{100}, historyAge{2000}, pliInterval{300};
        unsigned maxNackRetries{5};
        std::size_t maxStreams{64}, maxMissing{512}, maxCachedPackets{2048}, maxCachedBytes{4 * 1024 * 1024};
        std::size_t maxTwccHistory{8192}, maxOutputPackets{64}, maxDatagramSize{1100}; // plaintext RTCP size
        RtcpParameters parameters; // direct DtlsTransport users configure their PTs here
    };
    struct Callbacks
    {
        std::function<void(RtcpReportEvent)> onReport;
        std::function<void(RtcpNackEvent)> onNack;
        std::function<void(RtcpKeyFrameEvent)> onKeyFrameRequest;
        std::function<void(TwccFeedbackEvent)> onTwccFeedback;
        std::function<void(TransportPacketsFeedback)> onTransportFeedback;
        std::function<void(SentPacket)> onSentPacket{};
        std::function<void(RtcpRembEvent)> onRemb{};
    };
    struct Output { RtcpBytes bytes; bool retransmission{false}; std::optional<std::int64_t> transportSequence; }; // retransmission is already SRTP ciphertext
    explicit RtcpSession(Config config);
    RtcpSession(Config config, Callbacks callbacks);
    ~RtcpSession();
    RtcpSession(const RtcpSession&) = delete;
    RtcpSession& operator=(const RtcpSession&) = delete;
    bool configure(RtcpParameters parameters); // rejects conflicting bundled PT/extension mappings
    bool prepareRtp(RtcpBytes& plaintext); // adds negotiated association-wide TWCC sequence
    bool reserveRtpExtensions(RtcpBytes& plaintext); // reserves space before queuing; doesn't consume a TWCC sequence
    // Register submission before the Sender can synchronously return feedback.
    std::optional<std::int64_t> sendingRtp(const RtcpBytes& plaintext, std::size_t wireSize, Time createdAt, Time sentAt,
        std::size_t overheadBytes = 0, const PacedPacketInfo& pacing = {});
    void retransmittedRtp(std::int64_t transportSequence, Time sentAt, std::size_t wireSize = 0, std::size_t overheadBytes = 0);
    std::size_t outstandingData() const;
    void failedRtp(std::int64_t transportSequence);
    // Success-only SR statistics/cache. Direct users can also register the send here;
    // Transport callers pass recordTransport=false and the full id returned by sendingRtp().
    void sentRtp(const RtcpBytes& plaintext, RtcpBytes ciphertext, Time sentAt, bool recordTransport = true,
        std::optional<std::int64_t> transportSequence = {});
    void receivedRtp(const RtcpBytes& plaintext, Time arrival);
    bool receivedRtcp(const RtcpBytes& plaintext, Time arrival);
    bool requestPli(std::uint32_t mediaSsrc, Time now); // false if unknown/unnegotiated/rate limited
    void poll(Time now, std::uint64_t ntpNow);
    std::vector<Output> takeOutput();
    RtcpStats snapshot() const;
    void invalidate(); // clear live media/history, retain final diagnostic counters
    void reset(std::uint64_t generation);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace webrtc
