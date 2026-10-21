#include"webrtc/congestion_control/gcc_transport_controller.h"
#include"webrtc/congestion_control/pacer.h"
#include<iostream>
#include<limits>
#include<stdexcept>
using namespace webrtc;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
const TransportTime epoch{1000s};
ProbeClusterConfig cluster(int id, TransportTime now = epoch)
{ ProbeClusterConfig c; c.id = id; c.at_time = now; c.target_data_rate = 1000000; c.target_duration = 15ms; c.min_probe_delta = 2ms; c.target_probe_count = 5; return c; }
void proberScheduling()
{
    BitrateProber p; p.CreateProbeCluster(cluster(1)); CHECK(!p.is_probing());
    p.OnIncomingPacket(199); CHECK(!p.is_probing()); p.OnIncomingPacket(200); CHECK(p.is_probing());
    CHECK(p.RecommendedMinProbeSize() == 250 && p.NextProbeTime(epoch) == TransportTime{});
    auto info = p.CurrentCluster(epoch); CHECK(info && info->probe_cluster_min_bytes == 1875 && info->probe_cluster_min_probes == 5);
    auto now = epoch;
    for (int i = 0; i < 5; ++i)
    {
        CHECK(p.CurrentCluster(now)->probe_cluster_bytes_sent == static_cast<unsigned>(i * 400));
        p.ProbeSent(now, 400);
        if (i < 4) { now = *p.NextProbeTime(now); CHECK(now == epoch + (i + 1) * 3200us); }
    }
    CHECK(!p.is_probing() && !p.NextProbeTime(now));
    p.SetAllowProbeWithoutMediaPacket(true); p.CreateProbeCluster(cluster(2)); CHECK(p.is_probing()); p.ProbeSent(epoch, 250);
    const auto next = *p.NextProbeTime(epoch); CHECK(p.CurrentCluster(next + 10ms)); CHECK(!p.CurrentCluster(next + 10ms + 1us));
    p.Reset(); for (int i = 0; i < 8; ++i) p.CreateProbeCluster(cluster(i)); CHECK(p.pending_clusters() == 6);
    p.Reset(); CHECK(!p.is_probing() && p.pending_clusters() == 0);
}
void congestionAndCancellation()
{
    Pacer::Config cfg; cfg.rateBps = 1000; Pacer p(cfg, epoch);
    CHECK(p.enqueue({1, 0, 8, Pacer::Priority::Video, 100}, epoch) == Pacer::Admission::Accepted);
    CHECK(p.enqueue({2, 0, 8, Pacer::Priority::Video, 100}, epoch) == Pacer::Admission::Accepted);
    p.setCongested(true); CHECK(!p.pop(epoch + 5ms, true)); CHECK(p.nextWakeup(epoch) == epoch + cfg.maxQueueDelay);
    p.setCongested(false); CHECK(p.remove(1, epoch)->id == 1); CHECK(p.pop(epoch, true)->id == 2);
    p.accountProbeBytes(200); CHECK(p.snapshot(epoch).releasedPackets == 2 && p.snapshot(epoch).mediaBudgetBytes < 0);
}
void processingTimeAndRr()
{
    GccTransportConfig cfg; cfg.make_algorithm_config = [] { GoogCcConfig c; c.bandwidth.loss.enabled = false; return c; };
    GccTransportController c(cfg); auto first = c.Start(7, true, epoch); CHECK(first.target_rate && c.snapshot().pending_probe_clusters == 2);
    SentPacket sent; sent.send_time = epoch + 1ms; sent.size = 1200; sent.sequence_number = 0;
    (void)c.OnSentPacket(sent, epoch + 1ms); (void)c.Process(epoch + 25ms, 0, 0);
    TransportPacketsFeedback report; report.generation = 7; report.feedback_time = epoch + 10ms;
    PacketResult p; p.sent_packet = sent; p.receive_time = epoch + 5ms; report.packet_feedbacks = {p};
    (void)c.OnFeedback(report, epoch + 30ms); CHECK(c.snapshot().feedback_packets == 1 && report.feedback_time == epoch + 10ms);
    report.generation = 6; (void)c.OnFeedback(report, epoch + 35ms); CHECK(c.snapshot().feedback_packets == 1);
    RtcpReportEvent rr; rr.generation = 7; rr.reporterSsrc = 2; rr.report.ssrc = 9; rr.report.highestSequence = 100;
    (void)c.OnReport(rr, epoch + 40ms); rr.report.highestSequence = 200; rr.report.cumulativeLost = 20;
    (void)c.OnReport(rr, epoch + 50ms); (void)c.Process(epoch + 60ms, 0, 0);
    CHECK(c.snapshot().target->network_estimate.loss_rate_ratio > 0.19f);
    (void)c.OnReport(rr, epoch + 70ms); (void)c.Process(epoch + 80ms, 0, 0);
    rr.report.highestSequence = 190; (void)c.OnReport(rr, epoch + 90ms);
    rr.report.highestSequence = 300; rr.report.cumulativeLost = 30; (void)c.OnReport(rr, epoch + 100ms);
    (void)c.Process(epoch + 110ms, 0, 0); CHECK(c.snapshot().target->network_estimate.loss_rate_ratio > 0.09f);
    c.Reset(8); CHECK(!c.active() && c.snapshot().generation == 8 && !c.snapshot().target);
    CHECK(!c.OnFeedback(report, epoch + 120ms).has_updates());
}
RtcpBytes rtp(std::uint8_t pt, std::uint16_t sequence)
{ return {0x80, pt, static_cast<std::uint8_t>(sequence >> 8), static_cast<std::uint8_t>(sequence), 0, 0, 0, 1, 0, 0, 0, 88, 1, 2, 3, 4}; }
void adapterMetadataAndRemb()
{
    std::vector<SentPacket> sent; std::vector<TransportPacketsFeedback> feedback; std::vector<RtcpRembEvent> remb;
    RtcpSession::Config cfg; RtcpMediaParameters media; media.payloadType = 96; media.clockRate = 90000;
    media.transportCc = true; media.sendTwccExtension = 5;
    auto untracked = media; untracked.payloadType = 97; untracked.transportCc = false; cfg.parameters.media = {media, untracked};
    RtcpSession::Callbacks cb; cb.onSentPacket = [&](auto p) { sent.push_back(p); };
    cb.onTransportFeedback = [&](auto p) { feedback.push_back(p); }; cb.onRemb = [&](auto p) { remb.push_back(p); };
    RtcpSession session(cfg, cb); session.reset(7);
    auto plain = rtp(97, 0); CHECK(!session.sendingRtp(plain, 32, epoch, epoch, 20)); CHECK(sent.size() == 1 && sent[0].size == 52);
    plain = rtp(96, 1); CHECK(session.prepareRtp(plain)); PacedPacketInfo pacing{11, 5, 2000, 1000000};
    const auto id = session.sendingRtp(plain, 40, epoch, epoch + 1ms, 20, pacing);
    CHECK(id && sent.size() == 2 && sent.back().pacing_info.probe_cluster_id == 11 && sent.back().prior_unacked_data == 52);
    session.sentRtp(plain, {}, epoch + 1ms, false, id);
    CHECK(session.receivedRtcp(buildRtcpRemb({1, 500000, {88}}), epoch + 2ms) && remb.size() == 1 && remb[0].generation == 7);
    CHECK(session.receivedRtcp(buildRtcpRemb({1, 500000, {99}}), epoch + 3ms) && remb.size() == 1);
    session.retransmittedRtp(*id, epoch + 4ms, 40, 20); CHECK(sent.size() == 3 && session.snapshot().transport.registered_packets == 1);
    RtcpTwcc f; f.baseSequence = 0; f.deltas = {0};
    CHECK(session.receivedRtcp(buildRtcpTwcc(f), epoch + 10ms)); CHECK(feedback.size() == 1);
    CHECK(feedback[0].packet_feedbacks[0].sent_packet.pacing_info.probe_cluster_id == 11 && session.outstandingData() == 0);
    std::vector<RtcpMessage> parsed;
    auto wire = buildRtcpRemb({1, 900000, {88}}); CHECK(parseRtcp(wire.data(), wire.size(), parsed));
    CHECK(std::get<RtcpRemb>(parsed[0]).bitrateBps == 900000);
    wire[17] = 0xff; wire[18] = wire[19] = 0xff; CHECK(parseRtcp(wire.data(), wire.size(), parsed));
    CHECK(std::get<RtcpRemb>(parsed[0]).bitrateBps == std::numeric_limits<std::uint64_t>::max());
    wire[16] = 2; CHECK(!parseRtcp(wire.data(), wire.size(), parsed));
}
}
int main()
{
    try { proberScheduling(); congestionAndCancellation(); processingTimeAndRr(); adapterMetadataAndRemb(); std::cout << "PASS: probe scheduling, congestion gating, generation, RR/RTT/REMB and shared adapter metadata\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
