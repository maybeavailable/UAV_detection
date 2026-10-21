#pragma once
#include"webrtc/congestion_control/goog_cc_network_control.h"
#include"webrtc/congestion_control/bitrate_prober.h"
#include"webrtc/rtcp_session.h"
#include<functional>
#include<map>

namespace webrtc
{
struct GccTransportConfig
{
    bool enabled{true}; // Activates with managed SRTP + RTCP + Pacer + negotiated send media.
    std::uint64_t min_bitrate{30000}, start_bitrate{300000};
    std::optional<std::uint64_t> max_bitrate{5000000};
    StreamsConfig streams;
    BitrateProber::Config prober;
    // Invoked for each new controller; keep DtlsTransport::Config copyable.
    std::function<GoogCcConfig()> make_algorithm_config;
    bool generate_padding{true};
    std::uint32_t padding_ssrc{0}; // Zero chooses a separate random SSRC; RTP sequence space is transport-owned.
};
struct CongestionControlEvent { std::uint64_t generation{0}; TargetTransferRate rate; };
struct GccTransportSnapshot
{
    std::uint64_t generation{0}, sent_packets{0}, feedback_packets{0}, process_ticks{0}, requested_probes{0};
    std::uint64_t padding_packets{0}, padding_bytes{0}, probe_packets{0}, probe_bytes{0};
    bool active{false}, probing{false}, network_available{true};
    std::size_t pending_probe_clusters{0};
    std::optional<TargetTransferRate> target;
    std::optional<PacerConfig> pacing;
    std::optional<std::size_t> congestion_window;
};
// Transport-side wiring policy. Synchronous, serialized by the DTLS strand.
// Event receive times are normalized to local processing time; the adapter's
// original packet send/receive timestamps stay intact for delay estimation.
class GccTransportController
{
public:
    using Time = TransportTime;
    explicit GccTransportController(GccTransportConfig config);
    NetworkControlUpdate Start(std::uint64_t generation, bool probing_supported, Time now);
    void Stop();
    void Reset(std::uint64_t generation);
    bool active() const { return bool(controller_); }
    NetworkControlUpdate OnSentPacket(SentPacket packet, Time now);
    NetworkControlUpdate OnFeedback(TransportPacketsFeedback feedback, Time now);
    NetworkControlUpdate OnReport(const RtcpReportEvent& report, Time now);
    NetworkControlUpdate OnRemoteBitrate(std::uint64_t rate, Time now);
    NetworkControlUpdate OnNetworkAvailability(bool available, Time now);
    NetworkControlUpdate Process(Time now, std::size_t pacer_queue, std::size_t outstanding_data);
    NetworkControlUpdate SetConstraints(TargetRateConstraints constraints, Time now);
    NetworkControlUpdate SetStreams(StreamsConfig streams, Time now);
    NetworkControlUpdate SetNetworkEstimate(NetworkStateEstimate estimate, Time now);
    void OnPacketQueued(std::size_t bytes) { prober_.OnIncomingPacket(bytes); }
    BitrateProber& prober() { return prober_; }
    GccTransportSnapshot snapshot() const;
private:
    NetworkControlUpdate record(NetworkControlUpdate update);
    Time observe(Time now);
    struct ReportState { std::uint32_t highest; std::int32_t lost; };
    GccTransportConfig config_;
    std::unique_ptr<GoogCcNetworkController> controller_;
    BitrateProber prober_;
    bool probing_supported_{false};
    std::optional<Time> last_time_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, ReportState> reports_;
    GccTransportSnapshot stats_;
};
} // namespace webrtc
