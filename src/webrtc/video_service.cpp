#include"webrtc/video_service.h"
#include"webrtc/video_sender.h"
#include"log/log.h"
#include<boost/asio/executor_work_guard.hpp>
#include<boost/asio/post.hpp>
#include<boost/asio/steady_timer.hpp>
#include<openssl/rand.h>
#include<openssl/crypto.h>
#include<algorithm>
#include<atomic>
#include<cctype>
#include<map>
#include<mutex>
#include<thread>

namespace webrtc
{
namespace net = boost::asio;
namespace
{
using Clock = std::chrono::steady_clock;
std::string randomId()
{
    unsigned char bytes[16]; if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("cannot generate video session ID");
    std::string result; for (auto b : bytes) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; } return result;
}
bool equal(const std::string& a, const std::string& b)
{ return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0; }
bool authorized(const VideoService::Authorization& check)
{ try { return check && check(); } catch (...) { return false; } }
std::optional<std::uint64_t> decimalParameter(const SdpCodec& codec, const char* key)
{
    const auto text = codec.parameter(key); if (!text || text->empty() || text->size() > 9) return {};
    std::uint64_t value = 0; for (auto c : *text) { if (c < '0' || c > '9') return {}; value = value * 10 + c - '0'; } return value;
}
bool supports(const SdpCodec& codec, const encode::EncoderConfig& config)
{
    auto name = codec.encoding; std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    auto profile = codec.parameter("profile-level-id").value_or("42e01f");
    std::transform(profile.begin(), profile.end(), profile.begin(), [](unsigned char c) { return std::tolower(c); });
    if (name != "h264" || codec.clockRate != 90000 || codec.channels != 1 || codec.parameter("packetization-mode").value_or("0") != "1" ||
        profile.size() != 6 || (profile.substr(0, 4) != "42e0" && profile.substr(0, 4) != "4200")) return false;
    const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    const auto high = hex(profile[4]), low = hex(profile[5]); if (high < 0 || low < 0) return false;
    struct Level { int id; std::uint64_t fs, mbps; };
    static constexpr Level levels[] = {{10,99,1485},{11,396,3000},{12,396,6000},{13,396,11880},{20,396,11880},
        {21,792,19800},{22,1620,20250},{30,1620,40500},{31,3600,108000},{32,5120,216000},
        {40,8192,245760},{41,8192,245760},{42,8704,522240},{50,22080,589824},{51,36864,983040},{52,36864,2073600}};
    const auto level = std::find_if(std::begin(levels), std::end(levels), [&](auto value) { return value.id == high * 16 + low; });
    if (level == std::end(levels)) return false;
    const auto fs = std::uint64_t((config.width + 15) / 16) * ((config.height + 15) / 16);
    const auto maxFs = decimalParameter(codec, "max-fs"), maxMbps = decimalParameter(codec, "max-mbps");
    if ((codec.parameter("max-fs") && !maxFs) || (codec.parameter("max-mbps") && !maxMbps)) return false;
    // RFC 6184 permits receiver limits above the default profile-level-id via max-fs/max-mbps.
    return fs <= std::max(level->fs, maxFs.value_or(0)) && fs * config.fps <= std::max(level->mbps, maxMbps.value_or(0));
}
}
struct VideoService::Impl : std::enable_shared_from_this<Impl>
{
    struct Session
    {
        std::string id, credential; Authorization authorization; Handler pending;
        std::shared_ptr<PeerConnection> peer; std::shared_ptr<VideoSender> sender;
        Clock::time_point deadline; bool localSet{false}, closing{false};
    };
    media::MediaService& media; Config config;
    net::io_context io; net::steady_timer timer{io};
    std::optional<net::executor_work_guard<net::io_context::executor_type>> work;
    std::thread worker; std::mutex lifecycle, submission;
    std::atomic<bool> running{false}; bool stopping{false};
    std::map<std::string, std::shared_ptr<Session>> sessions;
    Impl(media::MediaService& source, Config c) : media(source), config(std::move(c))
    {
        config.encoder.validate(); const auto& e = config.encoder;
        if (e.codec != media::VideoCodec::H264 || e.profile != 0 ||
            config.negotiationTimeout.count() < 1 || config.connectionTimeout.count() < 1 || config.disconnectedTimeout.count() < 1)
            throw std::invalid_argument("WebRTC live video requires H264 Baseline and positive timeouts");
    }
    void finish(const std::shared_ptr<Session>& s, Reply reply)
    { auto done = std::move(s->pending); if (done) done(std::move(reply)); }
    void erase(const std::shared_ptr<Session>& s, unsigned status = 410, std::string error = "session_closed")
    {
        if (s->closing) return;
        UAV_LOG_INFO << "[WebRTC][VideoService] closing video session reason=" << error;
        s->closing = true; if (s->sender) s->sender->close(); finish(s, {status, {}, {}, std::move(error)});
        if (!s->peer) { sessions.erase(s->id); return; }
        const auto self = shared_from_this();
        s->peer->close([self, s](auto) {
            self->sessions.erase(s->id);
            if (self->stopping && self->sessions.empty()) self->work.reset();
        });
    }
    void answerReady(const std::shared_ptr<Session>& s)
    {
        if (s->closing || !s->pending || !s->localSet) return;
        const auto snapshot = s->peer->snapshot();
        if (!snapshot.ice || snapshot.ice->ice.gatheringState != IceGatheringState::Complete || !snapshot.currentLocalDescription) return;
        if (!authorized(s->authorization)) { erase(s, 403, "playback_expired"); return; }
        const auto& description = *snapshot.currentLocalDescription;
        const auto active = std::find_if(description.session.media.begin(), description.session.media.end(), [](const auto& m) { return m.kind == "video" && !m.isRejected(); });
        if (active == description.session.media.end() || active->candidates.empty()) { erase(s, 503, "no_local_ice_candidates"); return; }
        s->deadline = Clock::now() + config.connectionTimeout;
        finish(s, {201, s->id, description.toSdp(), {}});
    }
    void create(std::string sdp, std::string credential, Authorization auth, Handler done)
    {
        if (stopping || !media.snapshot().running) { done({503, {}, {}, "media_unavailable"}); return; }
        if (!authorized(auth)) { done({403, {}, {}, "playback_expired"}); return; }
        PeerSessionDescription offered;
        if (sdp.size() > 60 * 1024 || !PeerSessionDescription::parse(SdpDescriptionType::Offer, sdp, offered))
        { done({400, {}, {}, "invalid_offer"}); return; }
        const auto video = std::find_if(offered.session.media.begin(), offered.session.media.end(), [&](const auto& m) {
            const auto direction = offered.session.effectiveDirection(m);
            return m.kind == "video" && !m.isRejected() && (direction == SdpDirection::RecvOnly || direction == SdpDirection::SendRecv);
        });
        if (video == offered.session.media.end() || std::count_if(offered.session.media.begin(), offered.session.media.end(),
            [](const auto& m) { return m.kind == "video" && !m.isRejected(); }) != 1)
        { done({406, {}, {}, "single_receiving_video_required"}); return; }
        // Advertise both browser Baseline variants; answer and RTP use the offered payload number.
        PeerMedia capability = PeerMedia::video("video", {
            {96, "H264", 90000, 1, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"},
            {98, "H264", 90000, 1, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"}}, SdpDirection::SendOnly);
        capability.feedback = {{std::nullopt, "nack"}, {std::nullopt, "nack pli"}, {std::nullopt, "ccm fir"}, {std::nullopt, "transport-cc"}};
        capability.extmaps = {{3, {}, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", {}}};
        auto selected = video->codecs.end();
        for (const auto& format : video->formats)
        {
            selected = std::find_if(video->codecs.begin(), video->codecs.end(), [&](const auto& codec) { return std::to_string(codec.payloadType) == format && supports(codec, config.encoder); });
            if (selected != video->codecs.end()) break;
        }
        if (selected == video->codecs.end()) { done({406, {}, {}, "h264_profile_or_resolution_not_supported"}); return; }
        capability.codecs = {{96, "H264", 90000, 1, selected->parameters}};
        auto s = std::make_shared<Session>(); s->id = randomId(); s->credential = std::move(credential); s->authorization = std::move(auth);
        s->pending = std::move(done); s->deadline = Clock::now() + config.negotiationTimeout;
        try
        {
            auto subscription = media.subscribe(media::Output::WebRtc);
            auto slot = std::make_shared<std::weak_ptr<PeerConnection>>();
            VideoSender::Config senderConfig; senderConfig.rtp.payloadType = static_cast<std::uint8_t>(selected->payloadType);
            s->sender = std::make_shared<VideoSender>(io, subscription, [slot](auto packet, auto complete) {
                if (auto peer = slot->lock()) peer->asyncSendRtp(std::move(packet), std::move(complete));
                else complete(net::error::operation_aborted, 0);
            }, senderConfig);
            auto peerConfig = config.peer; peerConfig.media = {std::move(capability)}; peerConfig.enableDataChannels = false;
            peerConfig.ice.ice.role = StunIceRole::Controlled;
            peerConfig.dtls.enableSrtp = peerConfig.dtls.requireSrtp = true;
            auto& gcc = peerConfig.dtls.congestionControl;
            gcc.enabled = true; gcc.min_bitrate = config.encoder.minBitrateBps; gcc.start_bitrate = config.encoder.startBitrateBps; gcc.max_bitrate = config.encoder.maxBitrateBps;
            PeerConnection::Callbacks callbacks; const auto self = shared_from_this(); const auto weak = std::weak_ptr<Session>(s);
            callbacks.onGatheringState = [self, weak](auto) { if (auto session = weak.lock()) self->answerReady(session); };
            callbacks.onConnectionState = [self, weak](auto state) {
                if (auto session = weak.lock(); session && !session->closing)
                {
                    if (state == PeerConnectionState::Connected) { session->deadline = Clock::time_point::max(); UAV_LOG_INFO << "[WebRTC][VideoService] live video connected"; }
                    else if (state == PeerConnectionState::Disconnected) session->deadline = Clock::now() + self->config.disconnectedTimeout;
                    else if (state == PeerConnectionState::Failed || state == PeerConnectionState::Closed) self->erase(session, 502, "peer_connection_failed");
                }
            };
            callbacks.onError = [](auto ec, const auto&) { UAV_LOG_WARN << "[WebRTC][VideoService] peer operation failed: " << ec.message(); };
            s->sender->bindCallbacks(callbacks);
            s->peer = std::make_shared<PeerConnection>(io, std::move(peerConfig), std::move(callbacks)); *slot = s->peer; sessions.emplace(s->id, s);
            UAV_LOG_INFO << "[WebRTC][VideoService] negotiate H264 PT=" << selected->payloadType << " input=" << config.encoder.width << 'x' << config.encoder.height;
            s->peer->setRemoteDescription(std::move(offered), [self, s](auto ec) {
                if (s->closing) return;
                if (ec) { self->erase(s, 400, "offer_rejected"); return; }
                s->peer->createAnswer([self, s](auto answerError, auto answer) {
                    if (s->closing) return;
                    if (answerError || !answer) { self->erase(s, 406, "answer_failed"); return; }
                    auto media = std::find_if(answer->session.media.begin(), answer->session.media.end(), [](const auto& m) { return m.kind == "video" && !m.isRejected(); });
                    if (media == answer->session.media.end()) { self->erase(s, 406, "video_codec_rejected"); return; }
                    const auto ssrc = std::to_string(s->sender->ssrc());
                    media->common.attributes.push_back({"msid", "uav video"});
                    media->common.attributes.push_back({"ssrc", ssrc + " cname:uav"});
                    media->common.attributes.push_back({"ssrc", ssrc + " msid:uav video"});
                    s->peer->setLocalDescription(std::move(*answer), [self, s](auto localError) {
                        if (s->closing) return;
                        if (localError) { self->erase(s, 400, "local_description_failed"); return; }
                        s->localSet = true; self->answerReady(s);
                    });
                });
            });
        }
        catch (const std::system_error& e)
        { erase(s, e.code() == std::errc::resource_unavailable_try_again ? 409 : 503, "video_slot_unavailable"); }
        catch (const std::exception&) { erase(s, 503, "video_session_failed"); }
    }
    void tick()
    {
        timer.expires_after(std::chrono::milliseconds(100)); const auto self = shared_from_this();
        timer.async_wait([self](auto ec) {
            if (ec || self->stopping) return;
            std::vector<std::shared_ptr<Session>> expired;
            for (const auto& entry : self->sessions) if (!entry.second->closing &&
                (Clock::now() >= entry.second->deadline || !authorized(entry.second->authorization))) expired.push_back(entry.second);
            for (const auto& s : expired) self->erase(s, 408, "session_timeout_or_playback_expired");
            self->tick();
        });
    }
};
VideoService::VideoService(media::MediaService& source, Config config) : impl_(std::make_shared<Impl>(source, std::move(config))) {}
VideoService::~VideoService() { stop(); }
void VideoService::start()
{
    auto s = impl_; std::lock_guard<std::mutex> operation(s->lifecycle); std::lock_guard<std::mutex> lock(s->submission);
    if (s->running) return;
    s->io.restart(); s->stopping = false; s->work.emplace(net::make_work_guard(s->io)); s->running = true;
    s->tick(); s->worker = std::thread([s] { s->io.run(); });
}
void VideoService::stop()
{
    auto s = impl_; std::lock_guard<std::mutex> operation(s->lifecycle);
    { std::lock_guard<std::mutex> lock(s->submission); if (!s->running.exchange(false)) return;
        net::post(s->io, [s] {
            s->stopping = true; s->timer.cancel(); std::vector<std::shared_ptr<Impl::Session>> copy;
            for (const auto& entry : s->sessions) copy.push_back(entry.second);
            for (const auto& session : copy) s->erase(session);
            if (s->sessions.empty()) s->work.reset();
        }); }
    if (s->worker.joinable()) s->worker.join();
}
void VideoService::offer(std::string sdp, std::string credential, Authorization auth, Handler done)
{
    auto s = impl_; std::unique_lock<std::mutex> lock(s->submission);
    if (!s->running) { lock.unlock(); done({503, {}, {}, "video_service_stopped"}); return; }
    net::post(s->io, [s, sdp = std::move(sdp), credential = std::move(credential), auth = std::move(auth), done = std::move(done)]() mutable {
        s->create(std::move(sdp), std::move(credential), std::move(auth), std::move(done));
    });
}
void VideoService::close(std::string id, std::string credential, Handler done)
{
    auto s = impl_; std::unique_lock<std::mutex> lock(s->submission);
    if (!s->running) { lock.unlock(); done({503, {}, {}, "video_service_stopped"}); return; }
    net::post(s->io, [s, id = std::move(id), credential = std::move(credential), done = std::move(done)] {
        const auto it = s->sessions.find(id);
        if (it == s->sessions.end() || !equal(it->second->credential, credential)) { done({404, {}, {}, "session_not_found"}); return; }
        s->erase(it->second); done({200, id, {}, {}});
    });
}
} // namespace webrtc
