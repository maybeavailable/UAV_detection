#include"webrtc/peerconnection.h"
#include"log/log.h"

#include<boost/asio/post.hpp>
#include<boost/asio/strand.hpp>
#include<openssl/rand.h>

#include<algorithm>
#include<deque>
#include<map>
#include<mutex>
#include<set>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace net = boost::asio;
namespace
{
using ErrorCode = PeerConnection::ErrorCode;
using Completion = PeerConnection::Completion;
void complete(const Completion& done, ErrorCode ec = {}) { if (done) done(ec); }
const char* stateName(PeerSignalingState state)
{
    switch (state) { case PeerSignalingState::Stable: return "Stable"; case PeerSignalingState::HaveLocalOffer: return "HaveLocalOffer"; case PeerSignalingState::HaveRemoteOffer: return "HaveRemoteOffer"; case PeerSignalingState::Closed: return "Closed"; }
    return "Unknown";
}
const char* stateName(PeerConnectionState state)
{
    switch (state)
    {
    case PeerConnectionState::New: return "New"; case PeerConnectionState::Connecting: return "Connecting"; case PeerConnectionState::Connected: return "Connected";
    case PeerConnectionState::Disconnected: return "Disconnected"; case PeerConnectionState::Failed: return "Failed"; case PeerConnectionState::Closed: return "Closed";
    }
    return "Unknown";
}
std::string lower(std::string value)
{
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool sameCredentials(const IceCredentials& a, const IceCredentials& b)
{ return a.ufrag == b.ufrag && a.password == b.password; }
bool sends(SdpDirection d) { return d == SdpDirection::SendRecv || d == SdpDirection::SendOnly; }
bool receives(SdpDirection d) { return d == SdpDirection::SendRecv || d == SdpDirection::RecvOnly; }
bool token(const std::string& value)
{
    return !value.empty() && value.size() <= 256 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; });
}
std::vector<std::pair<std::string, std::string>> fingerprintIdentity(const std::vector<SdpFingerprint>& values)
{
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& f : values) result.emplace_back(lower(f.algorithm), lower(f.value));
    std::sort(result.begin(), result.end()); return result;
}
const SdpMedia* transportTag(const PeerSessionDescription& description)
{
    for (const auto& m : description.session.media)
        if (!m.isRejected()) return &description.session.transportMedia(m, description.type);
    return nullptr;
}
IceCredentials credentials(const PeerSessionDescription& description, const SdpMedia& media)
{
    const auto a = description.session.effectiveAttributes(description.session.transportMedia(media, description.type));
    return {a.iceUfrag.value_or(""), a.icePwd.value_or("")};
}
SdpCodec canonicalCodec(const SdpMedia& media, unsigned pt)
{
    if (const auto* codec = media.codec(pt); codec && !codec->encoding.empty()) return *codec;
    SdpCodec value; value.payloadType = pt;
    if (media.kind == "audio")
    {
        if (pt == 0) { value.encoding = "PCMU"; value.clockRate = 8000; }
        if (pt == 8) { value.encoding = "PCMA"; value.clockRate = 8000; }
        if (pt == 9) { value.encoding = "G722"; value.clockRate = 8000; }
    }
    if (const auto* codec = media.codec(pt)) value.parameters = codec->parameters;
    return value;
}
bool codecIdentity(const SdpCodec& a, const SdpCodec& b)
{
    return !a.encoding.empty() && lower(a.encoding) == lower(b.encoding) && a.clockRate == b.clockRate && a.channels == b.channels;
}
bool codecCompatible(const SdpCodec& a, const SdpCodec& b)
{
    if (!codecIdentity(a, b)) return false;
    const auto name = lower(a.encoding);
    if (name == "h264")
        return a.parameter("packetization-mode").value_or("0") == b.parameter("packetization-mode").value_or("0") &&
               lower(a.parameter("profile-level-id").value_or("42e01f")) == lower(b.parameter("profile-level-id").value_or("42e01f"));
    if (name == "vp9") return a.parameter("profile-id").value_or("0") == b.parameter("profile-id").value_or("0");
    if (name == "rtx") return true; // apt 在主编码协商后按 PT 单独检查
    if (name == "opus" || name == "vp8" || name == "pcmu" || name == "pcma" || name == "g722") return true;
    // 未实现特定 codec 的参数协商时要求相同参数，避免宣称不具备的编码能力。
    return a.parameters.value_or("") == b.parameters.value_or("");
}
std::optional<unsigned> unsignedParameter(const SdpCodec& codec, std::string_view key)
{
    const auto value = codec.parameter(key);
    if (!value || value->empty() || value->size() > 3) return std::nullopt;
    unsigned n = 0;
    for (char c : *value) { if (c < '0' || c > '9') return std::nullopt; n = n * 10 + c - '0'; }
    return n <= 127 ? std::optional<unsigned>(n) : std::nullopt;
}
std::vector<SdpCodec> negotiateCodecs(const SdpMedia& offered, const PeerMedia& capability)
{
    std::vector<SdpCodec> result;
    std::map<unsigned, unsigned> localToRemote;
    for (const auto& format : offered.formats)
    {
        const auto remote = canonicalCodec(offered, static_cast<unsigned>(std::stoul(format)));
        if (lower(remote.encoding) == "rtx") continue;
        for (const auto& local : capability.codecs)
            if (codecCompatible(local, remote))
            {
                auto selected = remote;
                // answer 描述本地接收参数；H264/VP9 的兼容性已在上面保守检查。
                selected.parameters = local.parameters;
                result.push_back(std::move(selected)); localToRemote[local.payloadType] = remote.payloadType; break;
            }
    }
    for (const auto& format : offered.formats)
    {
        auto remote = canonicalCodec(offered, static_cast<unsigned>(std::stoul(format)));
        if (lower(remote.encoding) != "rtx") continue;
        const auto remoteApt = unsignedParameter(remote, "apt");
        if (!remoteApt) continue;
        for (const auto& local : capability.codecs)
        {
            const auto localApt = unsignedParameter(local, "apt");
            if (codecIdentity(local, remote) && localApt && localToRemote.count(*localApt) && localToRemote.at(*localApt) == *remoteApt)
            { remote.parameters = "apt=" + std::to_string(*remoteApt); result.push_back(remote); break; }
        }
    }
    return result;
}
RtcpParameters negotiatedRtcpParameters(const PeerSessionDescription& local, const PeerSessionDescription& remote)
{
    RtcpParameters result; result.reducedSize = true;
    const auto& answer = local.type == SdpDescriptionType::Answer ? local : remote;
    for (std::size_t i = 0; i < answer.session.media.size(); ++i)
    {
        const auto& a = answer.session.media[i]; if (a.isRejected() || !a.isRtp()) continue;
        result.reducedSize = result.reducedSize && a.rtcpRsize;
        const auto ld = local.session.effectiveDirection(local.session.media[i]);
        const auto rd = remote.session.effectiveDirection(remote.session.media[i]);
        const auto direction = answerDirection(rd, sends(ld), receives(ld));
        for (const auto& format : a.formats)
        {
            const auto codec = canonicalCodec(a, static_cast<unsigned>(std::stoul(format)));
            RtcpMediaParameters p; p.payloadType = codec.payloadType; p.clockRate = codec.clockRate;
            p.audio = a.kind == "audio";
            p.send = sends(direction); p.receive = receives(direction);
            for (const auto& feedback : a.feedback) if (!feedback.payloadType || *feedback.payloadType == codec.payloadType)
            {
                p.nack = p.nack || feedback.feedback == "nack"; p.pli = p.pli || feedback.feedback == "nack pli";
                p.fir = p.fir || feedback.feedback == "ccm fir"; p.transportCc = p.transportCc || feedback.feedback == "transport-cc";
            }
            for (const auto& ext : answer.session.effectiveAttributes(a).extmaps)
                if (ext.uri == "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01" && ext.id > 0 && ext.id <= 255)
                {
                    const auto d = ext.direction.value_or(SdpDirection::SendRecv);
                    if (local.type == SdpDescriptionType::Answer ? sends(d) : receives(d)) p.sendTwccExtension = static_cast<std::uint8_t>(ext.id);
                    if (local.type == SdpDescriptionType::Answer ? receives(d) : sends(d)) p.receiveTwccExtension = static_cast<std::uint8_t>(ext.id);
                }
            result.media.push_back(p);
        }
    }
    return result;
}
bool mediaValid(const PeerMedia& media)
{
    if (!token(media.mid) || !directionString(media.direction)) return false;
    if (media.kind == "application") return media.sctpPort != 0 && media.codecs.empty() && media.extmaps.empty() && media.feedback.empty();
    if ((media.kind != "audio" && media.kind != "video") || media.codecs.empty()) return false;
    SdpSession test; test.connection = SdpConnection{};
    SdpMedia section; section.kind = media.kind; section.mid = media.mid; section.rtcpMux = true;
    section.codecs = media.codecs; section.feedback = media.feedback; section.common.extmaps = media.extmaps;
    for (const auto& c : media.codecs)
    {
        if (c.encoding.empty() || !c.clockRate || !c.channels || (c.payloadType >= 64 && c.payloadType <= 95)) return false;
        section.formats.push_back(std::to_string(c.payloadType));
    }
    test.media.push_back(std::move(section)); return test.validate();
}
ErrorCode descriptionValid(const PeerSessionDescription& description)
{
    if (!description.session.validateWebRtc(description.type)) return net::error::invalid_argument;
    const SdpGroup* group = nullptr;
    std::size_t active = 0, applications = 0;
    std::map<unsigned, SdpCodec> payloads;
    for (const auto& m : description.session.media)
    {
        if (m.isRejected()) continue;
        ++active;
        if (m.isSctp()) ++applications;
        if (m.protocol != "UDP/TLS/RTP/SAVPF" && m.protocol != "UDP/DTLS/SCTP") return net::error::operation_not_supported;
        const auto* ownGroup = description.session.bundleGroup(*m.mid);
        if (active == 1) group = ownGroup;
        else if (!group || group != ownGroup) return net::error::operation_not_supported;
        if (m.isRtp()) for (const auto& format : m.formats)
        {
            const auto c = canonicalCodec(m, static_cast<unsigned>(std::stoul(format)));
            const auto found = payloads.find(c.payloadType);
            if (found != payloads.end() && (!codecIdentity(found->second, c) || found->second.parameters != c.parameters))
                return net::error::operation_not_supported;
            payloads[c.payloadType] = c;
        }
    }
    return applications > 1 ? ErrorCode(net::error::operation_not_supported) : ErrorCode{};
}
ErrorCode answerValid(const PeerSessionDescription& offer, const PeerSessionDescription& answer)
{
    if (answer.session.media.size() != offer.session.media.size()) return net::error::invalid_argument;
    for (std::size_t i = 0; i < offer.session.media.size(); ++i)
    {
        const auto& o = offer.session.media[i]; const auto& a = answer.session.media[i];
        if (o.mid != a.mid || o.kind != a.kind || o.protocol != a.protocol || (o.isRejected() && !a.isRejected())) return net::error::invalid_argument;
        if (a.isRejected()) continue;
        const auto od = offer.session.effectiveDirection(o), ad = answer.session.effectiveDirection(a);
        if ((sends(ad) && !receives(od)) || (receives(ad) && !sends(od))) return net::error::invalid_argument;
        if (const auto* ag = answer.session.bundleGroup(*a.mid))
        {
            const auto* og = offer.session.bundleGroup(*a.mid);
            if (!og) return net::error::invalid_argument;
            for (const auto& mid : ag->mids) if (std::find(og->mids.begin(), og->mids.end(), mid) == og->mids.end()) return net::error::invalid_argument;
        }
        else if (o.bundleOnly) return net::error::invalid_argument;
        if (a.isRtp())
        {
            for (const auto& feedback : a.feedback)
                if (std::none_of(o.feedback.begin(), o.feedback.end(), [&](const auto& original) {
                    return feedback.feedback == original.feedback && (!original.payloadType || original.payloadType == feedback.payloadType);
                })) return net::error::invalid_argument;
            const auto offeredExts = offer.session.effectiveAttributes(o).extmaps;
            for (const auto& ext : answer.session.effectiveAttributes(a).extmaps)
            {
                const auto original = std::find_if(offeredExts.begin(), offeredExts.end(), [&](const auto& e) { return e.id == ext.id && e.uri == ext.uri; });
                if (original == offeredExts.end()) return net::error::invalid_argument;
                const auto od = original->direction.value_or(SdpDirection::SendRecv), ad = ext.direction.value_or(SdpDirection::SendRecv);
                if ((sends(ad) && !receives(od)) || (receives(ad) && !sends(od))) return net::error::invalid_argument;
            }
        }
        if (a.isRtp()) for (const auto& f : a.formats)
        {
            if (std::find(o.formats.begin(), o.formats.end(), f) == o.formats.end()) return net::error::invalid_argument;
            const auto pt = static_cast<unsigned>(std::stoul(f));
            const auto oc = canonicalCodec(o, pt), ac = canonicalCodec(a, pt);
            if (!codecCompatible(oc, ac)) return net::error::invalid_argument;
            if (lower(ac.encoding) == "rtx" && (unsignedParameter(oc, "apt") != unsignedParameter(ac, "apt") ||
                !unsignedParameter(ac, "apt") || std::find(a.formats.begin(), a.formats.end(), std::to_string(*unsignedParameter(ac, "apt"))) == a.formats.end()))
                return net::error::invalid_argument;
        }
    }
    return {};
}
} // namespace

std::string PeerSessionDescription::toSdp() const { return session.toSdp(); }
bool PeerSessionDescription::parse(SdpDescriptionType type, std::string_view text, PeerSessionDescription& out, std::string* reason)
{
    PeerSessionDescription result; result.type = type;
    if (!SdpSession::parse(text, result.session, reason) || !result.session.validateWebRtc(type, reason)) return false;
    out = std::move(result); return true;
}
PeerMedia PeerMedia::audio(std::string mid, std::vector<SdpCodec> codecs, SdpDirection direction)
{ PeerMedia m; m.mid = std::move(mid); m.kind = "audio"; m.codecs = std::move(codecs); m.direction = direction; return m; }
PeerMedia PeerMedia::video(std::string mid, std::vector<SdpCodec> codecs, SdpDirection direction)
{ PeerMedia m; m.mid = std::move(mid); m.codecs = std::move(codecs); m.direction = direction; return m; }
PeerMedia PeerMedia::data(std::string mid, std::uint16_t port, std::uint64_t limit)
{ PeerMedia m; m.mid = std::move(mid); m.kind = "application"; m.sctpPort = port; m.maxMessageSize = limit; return m; }

struct PeerConnection::Impl : std::enable_shared_from_this<PeerConnection::Impl>
{
    using Action = std::function<void(Impl&, Completion)>;
    enum class SendMode { Dtls, Secure, Rtp, Rtcp };
    struct Command { Action action; Completion done; const char* name; };
    struct Candidate { PeerIceCandidate signal; std::optional<IceCandidate> parsed; };
    net::io_context& io;
    net::strand<net::io_context::executor_type> strand;
    Config config;
    Callbacks callbacks;
    std::unique_ptr<IceTransport> ice;
    std::unique_ptr<DtlsTransport> dtls;
    std::unique_ptr<DataChannelTransport> dataChannels;
    Snapshot current, published;
    mutable std::mutex mutex;
    std::uint64_t sessionId{0}, version{0}, tokenCounter{0};
    std::optional<IceCredentials> preparedCredentials;
    std::map<std::string, std::string> bindings; // 已协商 MID -> 本地能力 MID
    std::deque<Command> commands;
    Completion activeDone;
    bool busy{false}, closing{false}, closed{false}, transportClosed{false}, activating{false};
    bool activeTransport{false}, remoteEnd{false}, remoteEndRequested{false};
    std::size_t pendingSends{0}, candidateImports{0};
    std::vector<Completion> closeCallbacks;
    std::vector<Candidate> stagedCandidates;
    std::set<std::string> seenRemoteCandidates, failedRemoteCandidates, staleUfrags, emittedCandidates;
    std::string activeRemoteMid, activeRemoteUfrag, emittedUfrag;
    bool emittedEnd{false};
    std::vector<SdpFingerprint> activeRemoteFingerprints;
    std::optional<SdpSetupRole> activeLocalSetup;
    std::optional<DtlsSrtpKeyMaterial> srtpKeys; // 私有缓存：纯 SCTP 建链后可在重协商中增加 RTP
    bool keysDelivered{false};
    std::uint64_t loggedErrors{0};

    Impl(net::io_context& context, Config options, Callbacks handlers)
        : io(context), strand(net::make_strand(io)), config(std::move(options)), callbacks(std::move(handlers))
    {
        if (!config.maxPendingOperations || config.maxPendingOperations > 65536 || !config.maxPendingCandidates || config.maxPendingCandidates > 65536 ||
            config.ice.ice.components != std::vector<std::uint16_t>{1} || config.dtls.component != 1)
            throw std::invalid_argument("PeerConnection requires one muxed ICE component and bounded queues");
        if (config.media.size() > 64) throw std::invalid_argument("too many media sections");
        if (!config.dataChannel.validate()) throw std::invalid_argument("invalid DataChannel transport configuration");
        for (auto& m : config.media) assignMid(m);
        if (config.enableDataChannels) prepareDataCapability();
        if (config.ice.ice.credentials.ufrag.empty() && config.ice.ice.credentials.password.empty()) config.ice.ice.credentials = IceCredentials::generate();
        if (!config.ice.ice.credentials.validate()) throw std::invalid_argument("invalid local ICE credentials");
        if (!config.dtls.certificate) config.dtls.certificate = DtlsCertificate::generate();
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&sessionId), sizeof(sessionId)) != 1) throw std::runtime_error("SDP session ID generation failed");
        sessionId &= 0x7fffffffffffffffULL;
        current.localCredentials = config.ice.ice.credentials; current.localFingerprint = config.dtls.certificate->fingerprint();
        UAV_LOG_INFO << "[WebRTC][PeerConnection " << this << "] created media=" << config.media.size() << " data_channels=" << config.enableDataChannels
            << " transport_srtp=" << config.dtls.enableSrtp << " operation_limit=" << config.maxPendingOperations;
        publish();
    }
    void assignMid(PeerMedia& media)
    {
        auto exists = [&](const std::string& mid) {
            return std::any_of(config.media.begin(), config.media.end(), [&](const auto& m) { return &m != &media && m.mid == mid; }) || bindings.count(mid);
        };
        if (media.mid.empty()) for (std::size_t n = 0;; ++n) if (!exists(std::to_string(n))) { media.mid = std::to_string(n); break; }
        if (exists(media.mid) || !mediaValid(media)) throw std::invalid_argument("invalid or duplicate media capability");
        if (media.kind == "application" && std::any_of(config.media.begin(), config.media.end(), [&](const auto& m) { return &m != &media && m.kind == "application"; }))
            throw std::invalid_argument("only one SCTP association is supported");
    }
    void prepareDataCapability()
    {
        for (auto& m : config.media) if (m.kind == "application")
        {
            if (!m.maxMessageSize || m.maxMessageSize > config.dataChannel.maxMessageSize) m.maxMessageSize = config.dataChannel.maxMessageSize;
            return;
        }
        if (config.media.size() >= 64) throw std::invalid_argument("too many media sections for DataChannel");
        auto m = PeerMedia::data("", 5000, config.dataChannel.maxMessageSize); assignMid(m); config.media.push_back(std::move(m));
    }
    void ensureDataChannels()
    {
        if (dataChannels) return;
        const auto weak = weak_from_this(); DataChannelTransport::Callbacks handlers;
        handlers.onDataChannel = [weak](auto channel) {
            if (auto p = weak.lock()) net::post(p->strand, [p, channel = std::move(channel)] {
                if (!p->closing && p->callbacks.onDataChannel) p->callbacks.onDataChannel(channel);
            });
        };
        handlers.onError = [weak](auto ec, auto message) {
            if (auto p = weak.lock()) net::post(p->strand, [p, ec, message = std::move(message)] { p->error(ec, message); });
        };
        auto transport = std::make_unique<DataChannelTransport>(io, [weak](Bytes bytes, SendHandler done) {
            if (auto p = weak.lock())
            {
                net::post(p->strand, [p, bytes = std::move(bytes), done = std::move(done)]() mutable {
                    if (!p->closing && p->dtls && p->current.connectionState == PeerConnectionState::Connected)
                        p->dtls->asyncSend(std::move(bytes), std::move(done));
                    else if (done) done(net::error::not_connected, 0);
                });
            }
            else if (done) done(net::error::operation_aborted, 0);
        }, config.dataChannel, std::move(handlers));
        { std::lock_guard<std::mutex> lock(mutex); dataChannels = std::move(transport); }
    }
    void updateDataChannels()
    {
        if (!config.enableDataChannels || closing) return;
        ensureDataChannels();
        const auto generation = current.ice ? current.ice->generation : 0;
        const auto dataState = dataChannels->snapshot();
        if ((dataState.state == DataChannelTransportState::Closed || dataState.state == DataChannelTransportState::Failed) && generation > dataState.generation)
        {
            { std::lock_guard<std::mutex> lock(mutex); dataChannels.reset(); }
            ensureDataChannels();
        }
        if (current.connectionState == PeerConnectionState::Failed) { dataChannels->close(); return; }
        if (current.connectionState != PeerConnectionState::Connected || !current.dtls || !hasMedia(true)) { dataChannels->pause(generation); return; }
        if (!current.dtls->localSetup || !current.dtls->peerFingerprint) return;
        DataChannelTransport::Parameters parameters;
        parameters.generation = generation; parameters.localSetup = *current.dtls->localSetup;
        parameters.maxPacketSize = current.dtls->maxPlaintextSize;
        parameters.remoteIdentity = current.dtls->peerFingerprint->algorithm + ":" + current.dtls->peerFingerprint->value;
        for (const auto& media : current.negotiatedMedia) if (!media.rejected && media.kind == "application")
        {
            parameters.localPort = *media.localSctpPort; parameters.remotePort = *media.remoteSctpPort;
            parameters.remoteMaxMessageSize = media.remoteMaxMessageSize.value_or(65536);
            const auto* local = current.currentLocalDescription->session.findMedia(media.mid);
            parameters.localMaxMessageSize = static_cast<std::size_t>(local->maxMessageSize.value_or(65536));
            break;
        }
        const auto weak = weak_from_this();
        dataChannels->start(std::move(parameters), [weak](ErrorCode ec) { if (ec) if (auto p = weak.lock()) net::post(p->strand, [p, ec] { p->error(ec, "DataChannel SCTP transport could not start"); }); });
    }
    const PeerMedia* configuredCapability(const std::string& key) const
    {
        for (const auto& c : config.media) if (c.mid == key) return &c;
        return nullptr;
    }
    const PeerMedia* capability(const std::string& mid) const
    {
        return configuredCapability(bindings.count(mid) ? bindings.at(mid) : mid);
    }
    void publish() { std::lock_guard<std::mutex> lock(mutex); published = current; }
    void signal(PeerSignalingState state)
    {
        const auto previous = current.signalingState; current.signalingState = state; publish();
        if (previous != state) UAV_LOG_INFO << "[WebRTC][PeerConnection " << this << "] signaling " << stateName(previous) << " -> " << stateName(state);
        if (previous != state && callbacks.onSignalingState) callbacks.onSignalingState(state);
    }
    bool hasMedia(bool application) const
    {
        return std::any_of(current.negotiatedMedia.begin(), current.negotiatedMedia.end(), [application](const auto& m) {
            return !m.rejected && (application ? m.kind == "application" : (m.kind == "audio" || m.kind == "video"));
        });
    }
    void deliverSrtpKeys()
    {
        if (!closing && !keysDelivered && srtpKeys && hasMedia(false) && ice->snapshot().generation == srtpKeys->generation)
        {
            keysDelivered = true;
            if (callbacks.onSrtpKeys) callbacks.onSrtpKeys(*srtpKeys);
        }
    }
    void updateConnection()
    {
        auto next = PeerConnectionState::New;
        if (closing) next = PeerConnectionState::Closed;
        else if (activeTransport)
        {
            next = PeerConnectionState::Connecting;
            if (current.ice && current.dtls)
            {
                const auto is = current.ice->state; const auto ds = current.dtls->state;
                if (is == IceTransportState::Failed || is == IceTransportState::Closed || ds == DtlsState::Failed || ds == DtlsState::Closed)
                    next = PeerConnectionState::Failed;
                else if (is == IceTransportState::Disconnected) next = PeerConnectionState::Disconnected;
                else if (!activating && !current.ice->restarting && current.ice->generation == current.dtls->generation &&
                         (is == IceTransportState::Connected || is == IceTransportState::Completed) && ds == DtlsState::Connected)
                    next = hasMedia(false) && !current.dtls->srtpProfile ? PeerConnectionState::Failed : PeerConnectionState::Connected;
            }
        }
        const auto previous = current.connectionState; current.connectionState = next; publish();
        if (next != previous)
        {
            const auto level = next == PeerConnectionState::Failed ? LogLevel::ERROR : next == PeerConnectionState::Disconnected ? LogLevel::WARN : LogLevel::INFO;
            UAV_LOG_IF(level, true) << "[WebRTC][PeerConnection " << this << "][g=" << (current.ice ? current.ice->generation : 0)
                << "] connection " << stateName(previous) << " -> " << stateName(next);
        }
        if (next != previous && callbacks.onConnectionState) callbacks.onConnectionState(next);
        updateDataChannels();
    }
    void error(ErrorCode ec, std::string message)
    {
        if (!closing)
        {
            ++loggedErrors;
            UAV_LOG_IF(LogLevel::WARN, (loggedErrors & (loggedErrors - 1)) == 0)
                << "[WebRTC][PeerConnection " << this << "][g=" << (current.ice ? current.ice->generation : 0) << "] " << message
                << " error=" << ec.value() << ":" << ec.message() << " errors=" << loggedErrors;
            if (callbacks.onError) callbacks.onError(ec, std::move(message));
        }
    }
    template<class F> Completion resume(Completion done, F function)
    {
        const auto self = shared_from_this(); const auto ticket = tokenCounter;
        return [self, ticket, done = std::move(done), function = std::move(function)](ErrorCode ec) mutable {
            net::post(self->strand, [self, ticket, ec, done, function = std::move(function)]() mutable {
                if (self->closing || ticket != self->tokenCounter) return;
                function(*self, ec, std::move(done));
            });
        };
    }
    void enqueue(Action action, Completion done, const char* name)
    {
        if (closing) { complete(done, net::error::operation_aborted); return; }
        if (commands.size() + (busy ? 1 : 0) >= config.maxPendingOperations)
        {
            UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG))
                << "[WebRTC][PeerConnection " << this << "] operation queue full operation=" << name << " pending=" << commands.size();
            complete(done, net::error::no_buffer_space); return;
        }
        commands.push_back({std::move(action), std::move(done), name}); pump();
    }
    void pump()
    {
        if (busy || closing || commands.empty()) return;
        auto command = std::move(commands.front()); commands.pop_front(); busy = true;
        activeDone = std::move(command.done); const auto ticket = ++tokenCounter; const auto self = shared_from_this();
        UAV_LOG_IF(LogLevel::DEBUG, LogManager::getInstance().shouldLog(LogLevel::DEBUG))
            << "[WebRTC][PeerConnection " << this << "] operation=" << command.name << " ticket=" << ticket << " signaling=" << stateName(current.signalingState);
        Completion finish = [self, ticket, name = command.name](ErrorCode ec) {
            net::post(self->strand, [self, ticket, name, ec] {
                if (!self->busy || ticket != self->tokenCounter) return;
                auto done = std::move(self->activeDone); self->busy = false;
                if (ec && ec != net::error::operation_aborted)
                {
                    ++self->loggedErrors;
                    UAV_LOG_IF(LogLevel::WARN, (self->loggedErrors & (self->loggedErrors - 1)) == 0)
                        << "[WebRTC][PeerConnection " << self.get() << "] operation=" << name << " rejected ticket=" << ticket
                        << " error=" << ec.value() << ":" << ec.message() << " errors=" << self->loggedErrors;
                }
                self->publish(); complete(done, ec); self->pump();
            });
        };
        try { command.action(*this, finish); }
        catch (const std::invalid_argument&) { finish(net::error::invalid_argument); }
        catch (const std::exception&) { finish(net::error::fault); }
    }
    void ensureTransport(bool offerer)
    {
        if (ice) return;
        auto iceConfig = config.ice; iceConfig.ice.role = offerer ? StunIceRole::Controlling : StunIceRole::Controlled;
        iceConfig.ice.credentials = current.localCredentials;
        const auto weak = weak_from_this();
        IceTransport::Callbacks ih;
        ih.onState = [weak](auto) {
            if (auto self = weak.lock(); self && self->ice)
            {
                auto source = self->ice->snapshot();
                net::post(self->strand, [self, source = std::move(source)]() mutable {
                    if (self->closing || (self->current.ice && source.generation < self->current.ice->generation)) return;
                    self->current.ice = source; self->dtls->handleIceState(std::move(source)); self->updateConnection();
                });
            }
        };
        ih.onLocalCandidate = [weak](auto) { if (auto self = weak.lock()) self->gatheringEvent(); };
        ih.onGatheringState = [weak](auto state) {
            if (auto self = weak.lock()) self->gatheringEvent(state);
        };
        ih.onDtlsReady = [weak](auto ready) {
            if (auto self = weak.lock()) net::post(self->strand, [self, ready = std::move(ready)]() mutable {
                if (!self->closing && self->activeTransport && ready.generation == self->ice->snapshot().generation)
                    self->dtls->start(std::move(ready));
            });
        };
        ih.onPacket = [weak](auto packet) {
            if (auto self = weak.lock()) net::post(self->strand, [self, packet = std::move(packet)]() mutable {
                if (self->closing || packet.generation != self->ice->snapshot().generation) return;
                if (packet.type == IcePacketType::Dtls) self->dtls->handlePacket(std::move(packet));
                else if (self->current.connectionState == PeerConnectionState::Connected && self->hasMedia(false))
                {
                    if (self->config.dtls.enableSrtp) self->dtls->handlePacket(std::move(packet));
                    else if (self->callbacks.onSecurePacket) self->callbacks.onSecurePacket(std::move(packet));
                }
            });
        };
        ih.onError = [weak](auto ec, auto message) {
            if (auto self = weak.lock()) net::post(self->strand, [self, ec, message = std::move(message)] { self->error(ec, message); });
        };
        auto nextIce = std::make_unique<IceTransport>(io, std::move(iceConfig), std::move(ih));
        DtlsTransport::Callbacks dh;
        dh.onState = [weak](auto) {
            if (auto self = weak.lock(); self && self->dtls)
            {
                auto source = self->dtls->snapshot();
                net::post(self->strand, [self, source = std::move(source)]() mutable {
                    if (self->closing || (self->current.ice && source.generation < self->current.ice->generation)) return;
                    self->current.dtls = std::move(source); self->updateConnection();
                });
            }
        };
        dh.onData = [weak](auto bytes) {
            if (auto self = weak.lock())
            {
                const auto generation = self->dtls->snapshot().generation;
                net::post(self->strand, [self, generation, bytes = std::move(bytes)]() mutable {
                    if (!self->closing && self->current.connectionState == PeerConnectionState::Connected &&
                        self->ice->snapshot().generation == generation && self->hasMedia(true))
                    {
                        if (self->config.enableDataChannels) { self->ensureDataChannels(); self->dataChannels->handlePacket(std::move(bytes), generation); }
                        else if (self->callbacks.onDtlsData) self->callbacks.onDtlsData(std::move(bytes));
                    }
                });
            }
        };
        dh.onSrtpKeys = [weak](auto keys) {
            if (auto self = weak.lock()) net::post(self->strand, [self, keys = std::move(keys)]() mutable {
                if (!self->closing && self->ice->snapshot().generation == keys.generation)
                { self->srtpKeys = std::move(keys); self->deliverSrtpKeys(); }
            });
        };
        dh.onRtpPacket = dh.onRtcpPacket = [weak](auto packet) {
            if (auto self = weak.lock()) net::post(self->strand, [self, packet = std::move(packet)]() mutable {
                if (self->closing || packet.generation != self->ice->snapshot().generation ||
                    self->current.connectionState != PeerConnectionState::Connected || !self->hasMedia(false)) return;
                const auto& handler = packet.type == IcePacketType::Rtcp ? self->callbacks.onRtcpPacket : self->callbacks.onRtpPacket;
                if (handler) handler(std::move(packet));
            });
        };
        dh.onError = [weak](auto ec, auto message) {
            if (auto self = weak.lock()) net::post(self->strand, [self, ec, message = std::move(message)] { self->error(ec, message); });
        };
        const auto bridgeFeedback = [weak](auto member) {
            return [weak, member](auto event) {
                if (auto self = weak.lock()) net::post(self->strand, [self, member, event = std::move(event)]() mutable {
                    if (self->closing || event.generation != self->ice->snapshot().generation ||
                        self->current.connectionState != PeerConnectionState::Connected || !self->hasMedia(false)) return;
                    const auto& callback = self->callbacks.*member;
                    if (callback) callback(std::move(event));
                });
            };
        };
        dh.onReceiverReport = bridgeFeedback(&Callbacks::onReceiverReport);
        dh.onNack = bridgeFeedback(&Callbacks::onNack);
        dh.onKeyFrameRequest = bridgeFeedback(&Callbacks::onKeyFrameRequest);
        dh.onTwccFeedback = bridgeFeedback(&Callbacks::onTwccFeedback);
        dh.onTransportFeedback = bridgeFeedback(&Callbacks::onTransportFeedback);
        dh.onRemoteBitrateReport = bridgeFeedback(&Callbacks::onRemoteBitrateReport);
        dh.onTargetTransferRate = [weak](auto event) {
            if (auto self = weak.lock()) net::post(self->strand, [self, event = std::move(event)]() mutable {
                if (self->closing || event.generation != self->ice->snapshot().generation) return;
                if (self->callbacks.onTargetTransferRate) self->callbacks.onTargetTransferRate(std::move(event));
            });
        };
        auto nextDtls = std::make_unique<DtlsTransport>(io, [weak](Bytes bytes, SendHandler done) {
            if (auto self = weak.lock()) self->ice->asyncSend(std::move(bytes), 1, std::move(done));
            else if (done) done(net::error::operation_aborted, 0);
        }, config.dtls, std::move(dh));
        {
            std::lock_guard<std::mutex> lock(mutex);
            ice = std::move(nextIce); dtls = std::move(nextDtls);
        }
        current.ice = ice->snapshot(); current.dtls = dtls->snapshot(); publish();
    }
    void gatheringEvent(std::optional<IceGatheringState> state = std::nullopt)
    {
        auto source = ice->snapshot(); const auto self = shared_from_this();
        net::post(strand, [self, source = std::move(source), state]() mutable {
            if (self->closing || (self->current.ice && source.generation < self->current.ice->generation)) return;
            self->current.ice = std::move(source); self->refreshLocalCandidates();
            if (state && self->callbacks.onGatheringState) self->callbacks.onGatheringState(*state);
        });
    }
    void includeCandidates(PeerSessionDescription& description)
    {
        if (!current.ice) return;
        auto* tag = const_cast<SdpMedia*>(transportTag(description));
        if (!tag) return;
        tag->candidates = current.ice->ice.localCandidates;
        tag->endOfCandidates = current.ice->ice.gatheringState == IceGatheringState::Complete;
    }
    void refreshLocalCandidates()
    {
        if (current.currentLocalDescription) includeCandidates(*current.currentLocalDescription);
        if (current.pendingLocalDescription) includeCandidates(*current.pendingLocalDescription);
        auto* local = current.pendingLocalDescription ? &*current.pendingLocalDescription :
                      current.currentLocalDescription ? &*current.currentLocalDescription : nullptr;
        publish();
        if (!local) return;
        const auto* tag = transportTag(*local); if (!tag) return;
        const auto ufrag = credentials(*local, *tag).ufrag;
        const auto index = static_cast<std::size_t>(tag - local->session.media.data());
        if (emittedUfrag != ufrag) { emittedUfrag = ufrag; emittedCandidates.clear(); emittedEnd = false; }
        for (const auto& candidate : tag->candidates)
        {
            const auto text = candidate.toCandidateString();
            if (emittedCandidates.insert(text).second && callbacks.onIceCandidate)
                callbacks.onIceCandidate({text, tag->mid, index, ufrag, std::nullopt});
        }
        if (tag->endOfCandidates && !emittedEnd)
        {
            emittedEnd = true;
            if (callbacks.onIceCandidate) callbacks.onIceCandidate({"", tag->mid, index, ufrag, std::nullopt});
        }
    }
    PeerSessionDescription baseDescription(SdpDescriptionType type)
    {
        PeerSessionDescription d; d.type = type; d.session.origin.sessionId = sessionId;
        d.session.origin.sessionVersion = ++version; d.session.connection = SdpConnection{};
        return d;
    }
    void bundle(PeerSessionDescription& d, const IceCredentials& local, SdpSetupRole setup)
    {
        d.session.common.iceUfrag = local.ufrag; d.session.common.icePwd = local.password;
        d.session.common.fingerprints = {current.localFingerprint}; d.session.common.setup = setup;
        d.session.common.iceOptions = {"trickle"};
        SdpGroup group; group.semantics = "BUNDLE";
        bool rtp = false;
        for (auto& m : d.session.media) if (!m.isRejected()) { group.mids.push_back(*m.mid); rtp |= m.isRtp(); }
        if (!group.mids.empty())
        {
            auto* tag = d.session.findMedia(group.mids.front()); tag->rtcpMux = rtp;
            d.session.groups.push_back(std::move(group));
        }
        includeCandidates(d);
    }
    SdpMedia offerMedia(const PeerMedia& c, const std::string& mid)
    {
        SdpMedia m; m.kind = c.kind; m.mid = mid; m.common.direction = c.direction;
        if (c.kind == "application")
        {
            m.protocol = "UDP/DTLS/SCTP"; m.formats = {"webrtc-datachannel"}; m.sctpPort = c.sctpPort; m.maxMessageSize = c.maxMessageSize;
        }
        else
        {
            m.codecs = c.codecs; m.feedback = c.feedback; m.common.extmaps = c.extmaps; m.rtcpMux = true;
            for (const auto& codec : c.codecs) m.formats.push_back(std::to_string(codec.payloadType));
        }
        return m;
    }
    PeerSessionDescription offer(OfferOptions options)
    {
        if (options.iceRestart && !preparedCredentials) preparedCredentials = IceCredentials::generate();
        auto d = baseDescription(SdpDescriptionType::Offer);
        std::set<std::string> used;
        if (current.currentLocalDescription) for (const auto& old : current.currentLocalDescription->session.media)
        {
            const auto* cap = capability(*old.mid);
            if (cap && !old.isRejected()) { d.session.media.push_back(offerMedia(*cap, *old.mid)); used.insert(cap->mid); }
            else { auto rejected = old; rejected.port = 0; rejected.bundleOnly = false; rejected.candidates.clear(); rejected.endOfCandidates = false; d.session.media.push_back(std::move(rejected)); }
        }
        for (const auto& cap : config.media) if (!used.count(cap.mid))
        {
            // MID 不得与此前被拒绝的槽位重用。
            auto mid = cap.mid;
            if (d.session.findMedia(mid)) continue;
            d.session.media.push_back(offerMedia(cap, mid));
        }
        bundle(d, options.iceRestart ? *preparedCredentials : current.localCredentials, SdpSetupRole::ActPass);
        bool first = true;
        for (auto& m : d.session.media) if (!m.isRejected())
        {
            if (!first) { m.port = 0; m.bundleOnly = true; }
            first = false;
        }
        return d;
    }
    std::map<std::string, std::string> matchCapabilities(const PeerSessionDescription& offered) const
    {
        std::map<std::string, std::string> result; std::set<std::string> used;
        for (const auto& m : offered.session.media)
        {
            if (m.isRejected()) continue;
            const auto* preferred = capability(*m.mid);
            if (preferred && preferred->kind == m.kind && !used.count(preferred->mid)) { result[*m.mid] = preferred->mid; used.insert(preferred->mid); continue; }
            for (const auto& cap : config.media) if (cap.kind == m.kind && !used.count(cap.mid))
            { result[*m.mid] = cap.mid; used.insert(cap.mid); break; }
        }
        return result;
    }
    PeerSessionDescription answer()
    {
        const auto& offered = *current.pendingRemoteDescription;
        auto d = baseDescription(SdpDescriptionType::Answer);
        const auto matches = matchCapabilities(offered);
        for (const auto& o : offered.session.media)
        {
            SdpMedia m; m.kind = o.kind; m.protocol = o.protocol; m.mid = o.mid; m.formats = o.formats; m.port = 0;
            if (!o.isRejected() && matches.count(*o.mid))
            {
                const auto* c = configuredCapability(matches.at(*o.mid));
                m = offerMedia(*c, *o.mid); m.protocol = o.protocol;
                m.common.direction = answerDirection(offered.session.effectiveDirection(o), sends(c->direction), receives(c->direction));
                if (o.isRtp())
                {
                    m.codecs = negotiateCodecs(o, *c); m.formats.clear();
                    for (const auto& codec : m.codecs) m.formats.push_back(std::to_string(codec.payloadType));
                    if (m.formats.empty()) { m.port = 0; m.formats = o.formats; }
                    m.feedback.clear();
                    for (const auto& f : o.feedback)
                    {
                        if (f.payloadType && !m.codec(*f.payloadType)) continue;
                        for (const auto& local : c->feedback)
                        {
                            bool allowed = !local.payloadType;
                            if (local.payloadType)
                            {
                                const auto localCodec = std::find_if(c->codecs.begin(), c->codecs.end(), [&](const auto& codec) { return codec.payloadType == *local.payloadType; });
                                if (localCodec != c->codecs.end())
                                    allowed = std::any_of(m.codecs.begin(), m.codecs.end(), [&](const auto& codec) { return (!f.payloadType || codec.payloadType == *f.payloadType) && codecIdentity(codec, *localCodec); });
                            }
                            if (allowed && f.feedback == local.feedback) { m.feedback.push_back(f); break; }
                        }
                    }
                    m.common.extmaps.clear();
                    for (const auto& ext : offered.session.effectiveAttributes(o).extmaps)
                        for (const auto& local : c->extmaps) if (local.uri == ext.uri)
                        {
                            auto chosen = ext; chosen.direction = answerDirection(ext.direction.value_or(SdpDirection::SendRecv),
                                sends(local.direction.value_or(SdpDirection::SendRecv)), receives(local.direction.value_or(SdpDirection::SendRecv)));
                            m.common.extmaps.push_back(std::move(chosen)); break;
                        }
                }
            }
            d.session.media.push_back(std::move(m));
        }
        // 已接受的 tag 决定远端 offer 中实际采用的 ICE 凭据。
        const auto accepted = std::find_if(d.session.media.begin(), d.session.media.end(), [](const auto& m) { return !m.isRejected(); });
        bool restart = false;
        if (accepted != d.session.media.end() && !activeRemoteUfrag.empty())
        {
            const auto* remote = offered.session.findMedia(*accepted->mid);
            restart = credentials(offered, *remote).ufrag != activeRemoteUfrag;
        }
        if (restart && !preparedCredentials) preparedCredentials = IceCredentials::generate();
        const auto setup = !restart && activeLocalSetup ? *activeLocalSetup :
                           (config.ice.preferDtlsClient ? SdpSetupRole::Active : SdpSetupRole::Passive);
        bundle(d, preparedCredentials.value_or(current.localCredentials), setup);
        return d;
    }
    ErrorCode localValid(const PeerSessionDescription& d)
    {
        if (d.session.iceLite) return net::error::invalid_argument;
        std::optional<IceCredentials> identity;
        const auto matched = d.type == SdpDescriptionType::Answer ? matchCapabilities(*current.pendingRemoteDescription) : std::map<std::string, std::string>{};
        for (const auto& m : d.session.media)
        {
            if (m.isRejected()) continue;
            const auto attrs = d.session.effectiveAttributes(d.session.transportMedia(m, d.type));
            const auto local = credentials(d, m);
            if (identity && !sameCredentials(*identity, local)) return net::error::invalid_argument;
            identity = local;
            if (!sameCredentials(local, current.localCredentials) && (!preparedCredentials || !sameCredentials(local, *preparedCredentials))) return net::error::invalid_argument;
            for (const auto& f : attrs.fingerprints)
            {
                const auto actual = config.dtls.certificate->fingerprint(f.algorithm);
                if (lower(f.value) != lower(actual.value)) return net::error::invalid_argument;
            }
            const auto* cap = d.type == SdpDescriptionType::Offer ? capability(*m.mid) :
                              matched.count(*m.mid) ? configuredCapability(matched.at(*m.mid)) : nullptr;
            if (!cap || cap->kind != m.kind || (sends(d.session.effectiveDirection(m)) && !sends(cap->direction)) ||
                (receives(d.session.effectiveDirection(m)) && !receives(cap->direction))) return net::error::invalid_argument;
            if (m.isSctp())
            {
                if (m.sctpPort != cap->sctpPort || (cap->maxMessageSize && (!m.maxMessageSize || !*m.maxMessageSize || *m.maxMessageSize > cap->maxMessageSize)))
                    return net::error::invalid_argument;
            }
            else
            {
                for (const auto& ext : d.session.effectiveAttributes(m).extmaps)
                    if (std::none_of(cap->extmaps.begin(), cap->extmaps.end(), [&](const auto& own) { return own.uri == ext.uri; }))
                        return net::error::invalid_argument;
                for (const auto& feedback : m.feedback)
                    if (std::none_of(cap->feedback.begin(), cap->feedback.end(), [&](const auto& own) { return own.feedback == feedback.feedback; }))
                        return net::error::invalid_argument;
                const auto allowed = d.type == SdpDescriptionType::Offer ? cap->codecs :
                    negotiateCodecs(*current.pendingRemoteDescription->session.findMedia(*m.mid), *cap);
                for (const auto& format : m.formats)
                {
                    const auto selected = canonicalCodec(m, static_cast<unsigned>(std::stoul(format)));
                    const auto found = std::find_if(allowed.begin(), allowed.end(), [&](const auto& c) { return c.payloadType == selected.payloadType && codecCompatible(c, selected); });
                    if (found == allowed.end() || (lower(selected.encoding) == "rtx" && unsignedParameter(selected, "apt") != unsignedParameter(*found, "apt")))
                        return net::error::invalid_argument;
                }
            }
        }
        return {};
    }
    ErrorCode offerContinuity(const PeerSessionDescription& d, bool local) const
    {
        const auto& previous = local ? current.currentLocalDescription : current.currentRemoteDescription;
        if (!previous) return {};
        if (d.session.media.size() < previous->session.media.size()) return net::error::invalid_argument;
        for (std::size_t i = 0; i < previous->session.media.size(); ++i)
        {
            const auto& before = previous->session.media[i]; const auto& after = d.session.media[i];
            if (before.mid != after.mid || before.kind != after.kind || before.protocol != after.protocol || (before.isRejected() && !after.isRejected()))
                return net::error::invalid_argument;
        }
        return {};
    }
    void setDescription(PeerSessionDescription d, bool local, Completion done)
    {
        const bool offerType = d.type == SdpDescriptionType::Offer;
        if ((offerType && current.signalingState != PeerSignalingState::Stable) ||
            (!offerType && current.signalingState != (local ? PeerSignalingState::HaveRemoteOffer : PeerSignalingState::HaveLocalOffer)))
        { complete(done, net::error::already_started); return; }
        auto ec = descriptionValid(d);
        if (!ec && offerType) ec = offerContinuity(d, local);
        if (!ec && !offerType) ec = answerValid(local ? *current.pendingRemoteDescription : *current.pendingLocalDescription, d);
        if (!ec && local) ec = localValid(d);
        if (ec) { complete(done, ec); return; }
        if (!local)
        {
            for (const auto& m : d.session.media) for (const auto& c : m.candidates)
                if (c.component != 1 || c.transport != IceCandidateTransport::Udp) { complete(done, net::error::operation_not_supported); return; }
        }
        ensureTransport(local == offerType);
        if (offerType)
        {
            if (local) current.pendingLocalDescription = std::move(d);
            else { preparedCredentials.reset(); current.pendingRemoteDescription = std::move(d); }
            signal(local ? PeerSignalingState::HaveLocalOffer : PeerSignalingState::HaveRemoteOffer);
            if (!local) updateTrickle(*current.pendingRemoteDescription);
            refreshLocalCandidates();
            // gather 启动成功后可立即交换 SDP；STUN 收集完成另有 onGatheringState。
            ice->gather(resume(std::move(done), [](Impl& self, ErrorCode result, Completion finish) {
                self.current.ice = self.ice->snapshot();
                if (result)
                {
                    self.current.pendingLocalDescription.reset(); self.current.pendingRemoteDescription.reset();
                    if (self.current.currentRemoteDescription) self.updateTrickle(*self.current.currentRemoteDescription);
                    else self.current.canTrickleIceCandidates.reset();
                    self.signal(PeerSignalingState::Stable);
                }
                self.refreshLocalCandidates(); complete(finish, result);
            }));
            return;
        }
        auto localDescription = local ? std::move(d) : *current.pendingLocalDescription;
        auto remoteDescription = local ? *current.pendingRemoteDescription : std::move(d);
        activate(std::move(localDescription), std::move(remoteDescription), std::move(done));
    }
    void updateTrickle(const PeerSessionDescription& remote)
    {
        const auto* tag = transportTag(remote);
        current.canTrickleIceCandidates = tag && [&] {
            const auto attrs = remote.session.effectiveAttributes(*tag);
            return std::any_of(attrs.iceOptions.begin(), attrs.iceOptions.end(), [](const auto& option) { return lower(option) == "trickle"; });
        }();
        publish();
    }
    void commit(PeerSessionDescription local, PeerSessionDescription remote)
    {
        current.negotiatedMedia.clear();
        const auto& answerDescription = local.type == SdpDescriptionType::Answer ? local : remote;
        const auto matches = local.type == SdpDescriptionType::Answer ? matchCapabilities(remote) : std::map<std::string, std::string>{};
        auto rtcpParameters = negotiatedRtcpParameters(local, remote);
        for (std::size_t i = 0; i < local.session.media.size(); ++i)
        {
            const auto& l = local.session.media[i]; const auto& r = remote.session.media[i]; const auto& a = answerDescription.session.media[i];
            PeerNegotiatedMedia m; m.mid = *l.mid; m.kind = l.kind; m.rejected = a.isRejected();
            if (!m.rejected)
            {
                const auto ld = local.session.effectiveDirection(l), rd = remote.session.effectiveDirection(r);
                m.localDirection = answerDirection(rd, sends(ld), receives(ld));
                if (a.isRtp())
                {
                    for (const auto& format : a.formats) m.codecs.push_back(canonicalCodec(a, static_cast<unsigned>(std::stoul(format))));
                    m.feedback = a.feedback; m.extmaps = answerDescription.session.effectiveAttributes(a).extmaps;
                    if (local.type != SdpDescriptionType::Answer)
                        for (auto& ext : m.extmaps)
                        {
                            const auto d = ext.direction.value_or(SdpDirection::SendRecv);
                            ext.direction = d == SdpDirection::SendOnly ? SdpDirection::RecvOnly : d == SdpDirection::RecvOnly ? SdpDirection::SendOnly : d;
                        }
                }
                else { m.localSctpPort = l.sctpPort; m.remoteSctpPort = r.sctpPort; m.remoteMaxMessageSize = r.maxMessageSize.value_or(65536); }
                if (local.type == SdpDescriptionType::Answer && matches.count(m.mid)) bindings[m.mid] = matches.at(m.mid);
            }
            current.negotiatedMedia.push_back(std::move(m));
        }
        current.currentLocalDescription = std::move(local); current.currentRemoteDescription = std::move(remote);
        if (config.dtls.enableSrtp && (config.dtls.rtcp.enabled || config.dtls.pacer.enabled))
        {
            const auto weak = weak_from_this();
            dtls->configureRtcp(std::move(rtcpParameters), [weak](ErrorCode ec) {
                if (ec) if (auto self = weak.lock()) net::post(self->strand, [self, ec] {
                    self->error(ec, "unsupported bundled RTCP payload/extension mapping");
                });
            });
        }
        UAV_LOG_INFO << "[WebRTC][PeerConnection " << this << "][g=" << (current.ice ? current.ice->generation : 0)
            << "] SDP committed media=" << current.negotiatedMedia.size() << " rejected="
            << std::count_if(current.negotiatedMedia.begin(), current.negotiatedMedia.end(), [](const auto& m) { return m.rejected; });
        current.pendingLocalDescription.reset(); current.pendingRemoteDescription.reset(); preparedCredentials.reset();
        if (const auto* tag = transportTag(*current.currentLocalDescription)) current.localCredentials = credentials(*current.currentLocalDescription, *tag);
        updateTrickle(*current.currentRemoteDescription); activating = false;
        signal(PeerSignalingState::Stable); refreshLocalCandidates(); updateConnection(); deliverSrtpKeys();
    }
    void activate(PeerSessionDescription local, PeerSessionDescription remote, Completion done)
    {
        if (config.dtls.enableSrtp && (config.dtls.rtcp.enabled || config.dtls.pacer.enabled) && !validateRtcpParameters(negotiatedRtcpParameters(local, remote)))
        { complete(done, net::error::operation_not_supported); return; }
        const auto& answerDescription = local.type == SdpDescriptionType::Answer ? local : remote;
        const auto* answerTag = transportTag(answerDescription);
        if (!answerTag)
        {
            commit(std::move(local), std::move(remote)); complete(done); return;
        }
        const auto* remoteSection = remote.session.findMedia(*answerTag->mid);
        const auto& remoteTag = remote.session.transportMedia(*remoteSection, remote.type);
        const auto attrs = remote.session.effectiveAttributes(remoteTag);
        const auto localAttrs = local.session.effectiveAttributes(local.session.transportMedia(*local.session.findMedia(*answerTag->mid), local.type));
        const auto lc = credentials(local, *local.session.findMedia(*answerTag->mid));
        const auto rc = credentials(remote, *remoteSection);
        const auto localSetup = local.type == SdpDescriptionType::Answer ? *localAttrs.setup : answerSetupRole(*attrs.setup);
        const auto source = ice->snapshot();
        const bool restart = !sameCredentials(lc, source.ice.localCredentials);
        const bool remoteRestart = source.ice.remoteCredentials && !sameCredentials(rc, *source.ice.remoteCredentials);
        UAV_LOG_INFO << "[WebRTC][PeerConnection " << this << "][g=" << source.generation << "] applying transport negotiation restart=" << restart
            << " remote_restart=" << remoteRestart << " dtls_role=" << (localSetup == SdpSetupRole::Active ? "client" : "server");
        if ((remoteRestart && (!restart || rc.ufrag == source.ice.remoteCredentials->ufrag || rc.password == source.ice.remoteCredentials->password)) ||
            (activeTransport && !restart && (activeLocalSetup != localSetup || fingerprintIdentity(activeRemoteFingerprints) != fingerprintIdentity(attrs.fingerprints))))
        { complete(done, net::error::invalid_argument); return; }
        if (local.type == SdpDescriptionType::Answer && (restart || !activeTransport) && localSetup != (config.ice.preferDtlsClient ? SdpSetupRole::Active : SdpSetupRole::Passive))
        { complete(done, net::error::invalid_argument); return; }
        // 本地和远端必须引用同一个 BUNDLE 传输；为拒绝原 tag 后的 answer 选中新的 tag。
        const auto selectedMid = *remoteTag.mid;
        activating = true;
        auto proceed = [local = std::move(local), remote = std::move(remote), selectedMid, rc, localSetup,
                        fingerprints = attrs.fingerprints, restart](Impl& self, ErrorCode ec, Completion finish) mutable {
            if (ec) { self.activating = false; self.updateConnection(); complete(finish, ec); return; }
            self.current.ice = self.ice->snapshot();
            if (restart)
            {
                if (!self.activeRemoteUfrag.empty())
                {
                    if (self.staleUfrags.size() >= 256) self.staleUfrags.erase(self.staleUfrags.begin());
                    self.staleUfrags.insert(self.activeRemoteUfrag);
                }
                self.seenRemoteCandidates.clear(); self.failedRemoteCandidates.clear(); self.candidateImports = 0;
                self.remoteEnd = false; self.remoteEndRequested = false;
                self.srtpKeys.reset(); self.keysDelivered = false;
            }
            const bool reuse = self.activeTransport && !restart;
            self.activeTransport = true; self.activeLocalSetup = localSetup;
            self.activeRemoteFingerprints = fingerprints; self.activeRemoteMid = selectedMid; self.activeRemoteUfrag = rc.ufrag;
            if (reuse)
            {
                self.commit(std::move(local), std::move(remote)); self.importCandidates(); complete(finish); return;
            }
            // 元数据与候选分开导入，mDNS/DNS 失败不会把一个有效 SDP 变成半完成的信令操作。
            auto metadata = remote.session;
            for (auto& m : metadata.media) { m.candidates.clear(); m.endOfCandidates = false; }
            const auto remoteType = remote.type;
            self.ice->setRemoteDescription(std::move(metadata), selectedMid, remoteType,
                self.resume(std::move(finish), [local = std::move(local), remote = std::move(remote)](Impl& owner, ErrorCode result, Completion end) mutable {
                    owner.current.ice = owner.ice->snapshot();
                    if (result) { owner.activating = false; owner.updateConnection(); complete(end, result); return; }
                    owner.commit(std::move(local), std::move(remote)); owner.importCandidates();
                    const auto weak = owner.weak_from_this();
                    owner.ice->start([weak](ErrorCode startError) { if (startError) if (auto p = weak.lock()) net::post(p->strand, [p, startError] { p->error(startError, "ICE checks could not start"); }); });
                    complete(end);
                }));
        };
        if (restart)
        {
            updateConnection();
            ice->restart(lc, source.ice.role, resume(std::move(done), std::move(proceed)));
        }
        else proceed(*this, {}, std::move(done));
    }
    ErrorCode candidateTarget(const Candidate& c, const PeerSessionDescription& remote, const SdpMedia*& media, std::string& ufrag) const
    {
        media = nullptr;
        if (c.signal.sdpMid) media = remote.session.findMedia(*c.signal.sdpMid);
        if (c.signal.sdpMid && !media) return net::error::invalid_argument;
        if (c.signal.sdpMLineIndex)
        {
            if (*c.signal.sdpMLineIndex >= remote.session.media.size()) return net::error::invalid_argument;
            const auto* indexed = &remote.session.media[*c.signal.sdpMLineIndex];
            if (media && media != indexed) return net::error::invalid_argument;
            media = indexed;
        }
        if (!media)
        {
            if (c.parsed) return net::error::invalid_argument;
            media = activeTransport && current.currentRemoteDescription && &remote == &*current.currentRemoteDescription ?
                    remote.session.findMedia(activeRemoteMid) : transportTag(remote);
        }
        if (!media || media->isRejected()) return net::error::invalid_argument;
        ufrag = credentials(remote, *media).ufrag;
        if (!c.signal.usernameFragment.empty() && c.signal.usernameFragment != ufrag) return net::error::operation_aborted;
        return {};
    }
    void recordRemoteCandidate(const Candidate& c, const SdpMedia& media)
    {
        if (!current.currentRemoteDescription) return;
        auto* target = current.currentRemoteDescription->session.findMedia(*media.mid);
        if (!target) return;
        if (c.parsed)
        {
            const auto text = c.parsed->toCandidateString();
            if (std::none_of(target->candidates.begin(), target->candidates.end(), [&](const auto& old) { return old.toCandidateString() == text; })) target->candidates.push_back(*c.parsed);
        }
        else target->endOfCandidates = true;
        publish();
    }
    void finishRemoteCandidates(Completion done = {})
    {
        // end-of-candidates 表示候选已收到；异步名称解析仍须完成。失败的已宣告候选可携带外部解析结果重试。
        if (!remoteEndRequested || remoteEnd || candidateImports || !failedRemoteCandidates.empty()) { complete(done); return; }
        remoteEnd = true;
        const auto self = shared_from_this(); const auto generation = ice->snapshot().generation;
        ice->endOfRemoteCandidates(activeRemoteUfrag, [self, generation, done = std::move(done)](ErrorCode result) {
            net::post(self->strand, [self, generation, result, done] {
                if (self->closing || self->ice->snapshot().generation != generation) { complete(done, net::error::operation_aborted); return; }
                if (result) self->remoteEnd = false;
                self->current.ice = self->ice->snapshot(); self->publish();
                complete(done, result);
                if (result && !done) self->error(result, "remote ICE gathering completion could not be imported");
            });
        });
    }
    void applyCandidate(Candidate c, Completion done = {})
    {
        const auto& remote = *current.currentRemoteDescription;
        const SdpMedia* media = nullptr; std::string ufrag;
        auto ec = candidateTarget(c, remote, media, ufrag);
        if (ec) { complete(done, ec); return; }
        // 只采用 answer 选中的传输；普通 offer 的其它 ICE transport 不得混入此通道。
        const auto& owner = remote.session.transportMedia(*media, remote.type);
        const auto selectedAttrs = remote.session.effectiveAttributes(*remote.session.findMedia(activeRemoteMid));
        const auto ownerAttrs = remote.session.effectiveAttributes(owner);
        if (ufrag != activeRemoteUfrag || ownerAttrs.icePwd != selectedAttrs.icePwd)
        { complete(done); return; }
        if (c.parsed)
        {
            const auto key = c.parsed->toCandidateString();
            if (seenRemoteCandidates.count(key)) { complete(done); return; }
            if (remoteEnd || (remoteEndRequested && !failedRemoteCandidates.count(key))) { complete(done, net::error::already_started); return; }
            if (!failedRemoteCandidates.count(key) && seenRemoteCandidates.size() + failedRemoteCandidates.size() >= config.maxPendingCandidates)
            { complete(done, net::error::no_buffer_space); return; }
            seenRemoteCandidates.insert(key); ++candidateImports;
            recordRemoteCandidate(c, *media);
            const auto self = shared_from_this(); const auto generation = ice->snapshot().generation;
            ice->addRemoteCandidate(*c.parsed, [self, generation, key, done = std::move(done)](ErrorCode result) {
                net::post(self->strand, [self, generation, key, result, done] {
                    if (self->closing || self->ice->snapshot().generation != generation) { complete(done, net::error::operation_aborted); return; }
                    --self->candidateImports;
                    if (result) { self->seenRemoteCandidates.erase(key); self->failedRemoteCandidates.insert(key); }
                    else self->failedRemoteCandidates.erase(key);
                    self->finishRemoteCandidates();
                    self->current.ice = self->ice->snapshot(); self->publish();
                    complete(done, result);
                });
            }, c.signal.resolvedEndpoint, activeRemoteUfrag);
        }
        else
        {
            remoteEndRequested = true; recordRemoteCandidate(c, *media);
            finishRemoteCandidates(std::move(done));
        }
    }
    void importCandidates()
    {
        std::vector<Candidate> candidates;
        const auto& remote = *current.currentRemoteDescription;
        const auto selected = credentials(remote, *remote.session.findMedia(activeRemoteMid));
        for (std::size_t i = 0; i < remote.session.media.size(); ++i)
        {
            const auto& m = remote.session.media[i];
            if (m.isRejected() || !sameCredentials(credentials(remote, m), selected)) continue;
            const auto ufrag = credentials(remote, m).ufrag;
            for (const auto& c : m.candidates) candidates.push_back({{c.toCandidateString(), m.mid, i, ufrag, std::nullopt}, c});
        }
        auto staged = std::move(stagedCandidates); stagedCandidates.clear();
        for (auto& c : staged) candidates.push_back(std::move(c));
        // 相同 SDP 候选的外部解析结果优先，不能让前面的无解析副本抢先发起 DNS 并吞掉覆盖值。
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.signal.resolvedEndpoint.has_value() && !b.signal.resolvedEndpoint.has_value();
        });
        // 所有候选先入队，再发出全 transport 的结束标记，避免 BUNDLE 重复 end 提前封闭队列。
        for (auto& c : candidates) if (c.parsed)
        {
            if (!c.signal.usernameFragment.empty() && c.signal.usernameFragment != activeRemoteUfrag)
            {
                if (!staleUfrags.count(c.signal.usernameFragment) && stagedCandidates.size() < config.maxPendingCandidates)
                    stagedCandidates.push_back(std::move(c));
                continue;
            }
            const auto weak = weak_from_this();
            applyCandidate(std::move(c), [weak](ErrorCode ec) { if (ec) if (auto p = weak.lock()) p->error(ec, "remote ICE candidate could not be imported"); });
        }
        bool ended = false;
        for (const auto& m : remote.session.media)
            if (!m.isRejected() && sameCredentials(credentials(remote, m), selected) && m.endOfCandidates) ended = true;
        for (auto& c : candidates) if (!c.parsed)
        {
            const SdpMedia* m = nullptr; std::string ufrag;
            if (!candidateTarget(c, remote, m, ufrag) && sameCredentials(credentials(remote, *m), selected)) ended = true;
            else if (!c.signal.usernameFragment.empty() && !staleUfrags.count(c.signal.usernameFragment) && stagedCandidates.size() < config.maxPendingCandidates)
                stagedCandidates.push_back(std::move(c));
        }
        if (ended)
        {
            Candidate end; end.signal.sdpMid = activeRemoteMid; end.signal.usernameFragment = activeRemoteUfrag;
            const auto weak = weak_from_this();
            applyCandidate(std::move(end), [weak](ErrorCode ec) { if (ec) if (auto p = weak.lock()) p->error(ec, "remote ICE gathering completion could not be imported"); });
        }
    }
    void addCandidate(PeerIceCandidate signal, Completion done)
    {
        Candidate c; c.signal = std::move(signal);
        if (!c.signal.candidate.empty())
        {
            IceCandidate parsed;
            if (!IceCandidate::parse(c.signal.candidate, parsed) || (!c.signal.sdpMid && !c.signal.sdpMLineIndex)) { complete(done, net::error::invalid_argument); return; }
            if (parsed.component != 1 || parsed.transport != IceCandidateTransport::Udp) { complete(done, net::error::operation_not_supported); return; }
            if (const auto ufrag = parsed.extension("ufrag"))
            {
                if (!c.signal.usernameFragment.empty() && c.signal.usernameFragment != *ufrag) { complete(done, net::error::invalid_argument); return; }
                c.signal.usernameFragment = *ufrag;
            }
            if (c.signal.resolvedEndpoint && (!parsed.isHostname() || c.signal.resolvedEndpoint->port() != parsed.port ||
                                             c.signal.resolvedEndpoint->address().is_unspecified() || c.signal.resolvedEndpoint->address().is_multicast()))
            { complete(done, net::error::invalid_argument); return; }
            c.parsed = std::move(parsed);
        }
        else if (c.signal.resolvedEndpoint) { complete(done, net::error::invalid_argument); return; }
        if (!c.signal.usernameFragment.empty() && staleUfrags.count(c.signal.usernameFragment)) { complete(done, net::error::operation_aborted); return; }
        const auto* remote = current.pendingRemoteDescription ? &*current.pendingRemoteDescription :
                             current.currentRemoteDescription ? &*current.currentRemoteDescription : nullptr;
        const bool future = remote && !current.pendingRemoteDescription && !c.signal.usernameFragment.empty() &&
                            c.signal.usernameFragment != activeRemoteUfrag;
        if (remote && !future)
        {
            const SdpMedia* media = nullptr; std::string ufrag;
            const auto ec = candidateTarget(c, *remote, media, ufrag);
            if (ec) { complete(done, ec); return; }
            if (!current.pendingRemoteDescription && activeTransport)
            { applyCandidate(std::move(c), std::move(done)); return; }
        }
        if (stagedCandidates.size() >= config.maxPendingCandidates) { complete(done, net::error::no_buffer_space); return; }
        if (current.pendingRemoteDescription && !future)
        {
            auto& pending = *current.pendingRemoteDescription;
            const SdpMedia* target = nullptr; std::string ufrag;
            if (!candidateTarget(c, pending, target, ufrag))
            {
                auto* m = pending.session.findMedia(*target->mid);
                if (c.parsed)
                {
                    if (std::none_of(m->candidates.begin(), m->candidates.end(), [&](const auto& old) { return old.toCandidateString() == c.parsed->toCandidateString(); }))
                        m->candidates.push_back(*c.parsed);
                }
                else m->endOfCandidates = true;
            }
        }
        stagedCandidates.push_back(std::move(c)); publish(); complete(done);
    }
    void send(Bytes bytes, SendMode mode, SendHandler done)
    {
        const bool application = mode == SendMode::Dtls;
        ErrorCode ec;
        if (closing) ec = net::error::operation_aborted;
        else if (current.connectionState != PeerConnectionState::Connected || !hasMedia(application)) ec = net::error::not_connected;
        else if (mode == SendMode::Secure && classifyIcePacket(bytes) != IcePacketType::Rtp && classifyIcePacket(bytes) != IcePacketType::Rtcp) ec = net::error::invalid_argument;
        if (ec) { if (done) done(ec, 0); return; }
        ++pendingSends; const auto self = shared_from_this();
        SendHandler finish = [self, done = std::move(done)](ErrorCode result, std::size_t size) {
            net::post(self->strand, [self, done, result, size] {
                --self->pendingSends; if (done) done(result, size); self->finishClose();
            });
        };
        if (mode == SendMode::Secure) ice->asyncSend(std::move(bytes), 1, std::move(finish));
        else if (mode == SendMode::Rtp) dtls->asyncSendRtp(std::move(bytes), ice->snapshot().generation, std::move(finish));
        else if (mode == SendMode::Rtcp) dtls->asyncSendRtcp(std::move(bytes), ice->snapshot().generation, std::move(finish));
        else dtls->asyncSend(std::move(bytes), std::move(finish));
    }
    void finishClose()
    {
        if (!closing || !transportClosed || pendingSends || closed) return;
        closed = true;
        if (ice) current.ice = ice->snapshot();
        if (dtls) current.dtls = dtls->snapshot();
        publish();
        UAV_LOG_INFO << "[WebRTC][PeerConnection " << this << "][g=" << (current.ice ? current.ice->generation : 0) << "] close completed";
        auto completions = std::move(closeCallbacks); for (const auto& done : completions) complete(done);
    }
    void close(Completion done)
    {
        if (closed) { complete(done); return; }
        if (done) closeCallbacks.push_back(std::move(done));
        if (closing) return;
        closing = true; ++tokenCounter; srtpKeys.reset();
        auto active = std::move(activeDone); auto queued = std::move(commands); commands.clear(); busy = false;
        signal(PeerSignalingState::Closed); updateConnection();
        complete(active, net::error::operation_aborted);
        for (const auto& command : queued) complete(command.done, net::error::operation_aborted);
        const auto self = shared_from_this();
        auto closeTransports = [self](ErrorCode) {
            if (!self->ice) { net::post(self->strand, [self] { self->transportClosed = true; self->finishClose(); }); return; }
            self->dtls->close([self](ErrorCode) {
                self->ice->close([self](ErrorCode) { net::post(self->strand, [self] { self->transportClosed = true; self->finishClose(); }); });
            });
        };
        if (dataChannels) dataChannels->close(std::move(closeTransports)); else closeTransports({});
    }
};

PeerConnection::PeerConnection(net::io_context& io) : PeerConnection(io, Config{}) {}
PeerConnection::PeerConnection(net::io_context& io, Config config, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(io, std::move(config), std::move(callbacks))) {}
PeerConnection::~PeerConnection() { if (impl_) close(); }
PeerConnection::Snapshot PeerConnection::snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = impl_->published;
    // 传输统计可能变化但未产生 state 事件；读取各自线程安全的最新快照。
    if (impl_->ice) result.ice = impl_->ice->snapshot();
    if (impl_->dtls) result.dtls = impl_->dtls->snapshot();
    if (impl_->dataChannels) result.dataChannels = impl_->dataChannels->snapshot();
    return result;
}
void PeerConnection::addMedia(PeerMedia media, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, media = std::move(media), done = std::move(done)]() mutable {
        self->enqueue([media = std::move(media)](Impl& p, Completion finish) mutable {
            if (p.current.signalingState != PeerSignalingState::Stable) { complete(finish, net::error::already_started); return; }
            if (p.config.media.size() >= 64) { complete(finish, net::error::no_buffer_space); return; }
            p.assignMid(media); p.config.media.push_back(std::move(media)); complete(finish);
        }, std::move(done), "addMedia");
    });
}
void PeerConnection::setDirection(std::string mid, SdpDirection direction, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, mid = std::move(mid), direction, done = std::move(done)]() mutable {
        self->enqueue([mid, direction](Impl& p, Completion finish) {
            if (p.current.signalingState != PeerSignalingState::Stable) { complete(finish, net::error::already_started); return; }
            const auto* cap = p.capability(mid);
            if (!cap || !directionString(direction)) { complete(finish, net::error::invalid_argument); return; }
            for (auto& c : p.config.media) if (&c == cap) c.direction = direction;
            complete(finish);
        }, std::move(done), "setDirection");
    });
}
void PeerConnection::createOffer(DescriptionHandler done) { createOffer(OfferOptions{}, std::move(done)); }
void PeerConnection::createOffer(OfferOptions options, DescriptionHandler done)
{
    auto self = impl_; auto result = std::make_shared<std::optional<PeerSessionDescription>>();
    net::post(self->strand, [self, options, result, done = std::move(done)]() mutable {
        self->enqueue([options, result](Impl& p, Completion finish) {
            if (p.current.signalingState != PeerSignalingState::Stable) { complete(finish, net::error::already_started); return; }
            auto d = p.offer(options); const auto ec = descriptionValid(d);
            if (!ec) *result = std::move(d);
            if (!ec) UAV_LOG_INFO << "[WebRTC][PeerConnection " << &p << "] offer created media=" << (*result)->session.media.size() << " ice_restart=" << options.iceRestart;
            complete(finish, ec);
        }, [result, done = std::move(done)](ErrorCode ec) { if (done) done(ec, ec ? std::nullopt : std::move(*result)); }, "createOffer");
    });
}
void PeerConnection::createAnswer(DescriptionHandler done)
{
    auto self = impl_; auto result = std::make_shared<std::optional<PeerSessionDescription>>();
    net::post(self->strand, [self, result, done = std::move(done)]() mutable {
        self->enqueue([result](Impl& p, Completion finish) {
            if (p.current.signalingState != PeerSignalingState::HaveRemoteOffer) { complete(finish, net::error::already_started); return; }
            auto d = p.answer(); auto ec = descriptionValid(d); if (!ec) ec = answerValid(*p.current.pendingRemoteDescription, d);
            if (!ec) *result = std::move(d);
            if (!ec) UAV_LOG_INFO << "[WebRTC][PeerConnection " << &p << "] answer created media=" << (*result)->session.media.size();
            complete(finish, ec);
        }, [result, done = std::move(done)](ErrorCode ec) { if (done) done(ec, ec ? std::nullopt : std::move(*result)); }, "createAnswer");
    });
}
void PeerConnection::setLocalDescription(PeerSessionDescription d, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, d = std::move(d), done = std::move(done)]() mutable {
        self->enqueue([d = std::move(d)](Impl& p, Completion finish) mutable { p.setDescription(std::move(d), true, std::move(finish)); }, std::move(done), "setLocalDescription");
    });
}
void PeerConnection::setRemoteDescription(PeerSessionDescription d, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, d = std::move(d), done = std::move(done)]() mutable {
        self->enqueue([d = std::move(d)](Impl& p, Completion finish) mutable { p.setDescription(std::move(d), false, std::move(finish)); }, std::move(done), "setRemoteDescription");
    });
}
void PeerConnection::setRemoteDescription(SdpDescriptionType type, std::string text, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, type, text = std::move(text), done = std::move(done)]() mutable {
        self->enqueue([type, text = std::move(text)](Impl& p, Completion finish) mutable {
            PeerSessionDescription d;
            if (!PeerSessionDescription::parse(type, text, d)) { complete(finish, net::error::invalid_argument); return; }
            p.setDescription(std::move(d), false, std::move(finish));
        }, std::move(done), "setRemoteDescription");
    });
}
void PeerConnection::addIceCandidate(PeerIceCandidate candidate, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, candidate = std::move(candidate), done = std::move(done)]() mutable {
        self->enqueue([candidate = std::move(candidate)](Impl& p, Completion finish) mutable { p.addCandidate(std::move(candidate), std::move(finish)); }, std::move(done), "addIceCandidate");
    });
}
void PeerConnection::rollback(Completion done)
{
    auto self = impl_; net::post(self->strand, [self, done = std::move(done)]() mutable {
        self->enqueue([](Impl& p, Completion finish) {
            if (p.current.signalingState == PeerSignalingState::Stable) { complete(finish, net::error::already_started); return; }
            p.current.pendingLocalDescription.reset(); p.current.pendingRemoteDescription.reset(); p.preparedCredentials.reset();
            p.stagedCandidates.clear();
            if (p.current.currentRemoteDescription) p.updateTrickle(*p.current.currentRemoteDescription);
            else p.current.canTrickleIceCandidates.reset();
            p.signal(PeerSignalingState::Stable); p.refreshLocalCandidates(); complete(finish);
        }, std::move(done), "rollback");
    });
}
void PeerConnection::createDataChannel(DataChannelConfig config, DataChannelTransport::CreateHandler done)
{ createDataChannel(std::move(config), {}, std::move(done)); }
void PeerConnection::createDataChannel(DataChannelConfig config, DataChannel::Callbacks callbacks, DataChannelTransport::CreateHandler done)
{
    const auto self = impl_; auto result = std::make_shared<std::shared_ptr<DataChannel>>();
    net::post(self->strand, [self, config = std::move(config), callbacks = std::move(callbacks), done = std::move(done), result]() mutable {
        self->enqueue([config = std::move(config), callbacks = std::move(callbacks), result](Impl& p, Completion finish) mutable {
            if (!config.validate()) { complete(finish, net::error::invalid_argument); return; }
            if (!p.config.enableDataChannels)
            {
                if (p.current.signalingState != PeerSignalingState::Stable) { complete(finish, net::error::already_started); return; }
                p.prepareDataCapability(); p.config.enableDataChannels = true;
            }
            p.ensureDataChannels(); p.updateDataChannels();
            p.dataChannels->createDataChannel(std::move(config), std::move(callbacks), [result, finish = std::move(finish)](ErrorCode ec, std::shared_ptr<DataChannel> c) mutable {
                *result = std::move(c); complete(finish, ec);
            });
        }, [result, done = std::move(done)](ErrorCode ec) { if (done) done(ec, ec ? nullptr : *result); }, "createDataChannel");
    });
}
void PeerConnection::asyncSendDtls(Bytes bytes, SendHandler done)
{
    auto self = impl_; net::post(self->strand, [self, bytes = std::move(bytes), done = std::move(done)]() mutable { self->send(std::move(bytes), Impl::SendMode::Dtls, std::move(done)); });
}
void PeerConnection::asyncSendSecurePacket(Bytes bytes, SendHandler done)
{
    auto self = impl_; net::post(self->strand, [self, bytes = std::move(bytes), done = std::move(done)]() mutable { self->send(std::move(bytes), Impl::SendMode::Secure, std::move(done)); });
}
void PeerConnection::asyncSendRtp(Bytes bytes, SendHandler done)
{
    auto self = impl_; net::post(self->strand, [self, bytes = std::move(bytes), done = std::move(done)]() mutable { self->send(std::move(bytes), Impl::SendMode::Rtp, std::move(done)); });
}
void PeerConnection::asyncSendRtcp(Bytes bytes, SendHandler done)
{
    auto self = impl_; net::post(self->strand, [self, bytes = std::move(bytes), done = std::move(done)]() mutable { self->send(std::move(bytes), Impl::SendMode::Rtcp, std::move(done)); });
}
void PeerConnection::close(Completion done)
{
    auto self = impl_; net::post(self->strand, [self, done = std::move(done)]() mutable { self->close(std::move(done)); });
}
void PeerConnection::requestPli(std::uint32_t mediaSsrc, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, mediaSsrc, done = std::move(done)]() mutable {
        if (self->closing || self->current.connectionState != PeerConnectionState::Connected) { complete(done, net::error::not_connected); return; }
        const auto generation = self->ice->snapshot().generation;
        self->dtls->requestPli(mediaSsrc, generation, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { complete(done, ec); });
        });
    });
}
void PeerConnection::setPacingRate(std::uint64_t rateBps, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, rateBps, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        self->dtls->setPacingRate(rateBps, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { self->publish(); complete(done, ec); });
        });
    });
}
void PeerConnection::setPacingRates(std::uint64_t rateBps, std::uint64_t paddingRateBps, Completion done)
{
    auto self = impl_; net::post(self->strand, [self, rateBps, paddingRateBps, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        self->dtls->setPacingRates(rateBps, paddingRateBps, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { self->publish(); complete(done, ec); });
        });
    });
}
void PeerConnection::setPacingPaused(bool paused, Completion done)
{
    const auto self = impl_;
    net::post(self->strand, [self, paused, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        self->dtls->setPacingPaused(paused, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { self->publish(); complete(done, ec); });
        });
    });
}
void PeerConnection::setBitrateConstraints(TargetRateConstraints constraints, Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, constraints, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        self->dtls->setBitrateConstraints(constraints, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { self->publish(); complete(done, ec); });
        });
    });
}
void PeerConnection::setStreamsConfig(StreamsConfig streams, Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, streams, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        self->dtls->setStreamsConfig(streams, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { self->publish(); complete(done, ec); });
        });
    });
}
void PeerConnection::setNetworkStateEstimate(NetworkStateEstimate estimate, Completion done)
{
    const auto self = impl_; net::post(self->strand, [self, estimate, done = std::move(done)]() mutable {
        if (self->closing) { complete(done, net::error::operation_aborted); return; }
        self->dtls->setNetworkStateEstimate(estimate, [self, done = std::move(done)](ErrorCode ec) {
            net::post(self->strand, [self, done, ec] { self->publish(); complete(done, ec); });
        });
    });
}
} // namespace webrtc
