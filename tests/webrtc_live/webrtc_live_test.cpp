#include <cstdlib>
#include"gateway/gateway.h"
#include"webrtc/video_service.h"
#include"common/rtp_packet.h"
#include<boost/asio.hpp>
#include<boost/beast/http.hpp>
#include<boost/beast/core.hpp>
#include<atomic>
#include<csignal>
#include<fstream>
#include<iostream>
#include<stdexcept>
#include<thread>

using namespace std::chrono_literals;
namespace net = boost::asio;
namespace http = boost::beast::http;
namespace json = boost::json;
using namespace webrtc;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
template<class F> void until(F predicate, net::io_context* io = nullptr, std::chrono::milliseconds timeout = 4s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
    { if (io) { io->restart(); io->run_for(2ms); } else std::this_thread::sleep_for(2ms); }
    CHECK(predicate());
}
struct EncoderState { std::atomic<unsigned> keys{0}, frames{0}; std::atomic<std::uint64_t> rate{0}; media::SharedBytes sample; };
struct Encoder : encode::VideoEncoder
{
    std::shared_ptr<EncoderState> state; bool key{true};
    explicit Encoder(std::shared_ptr<EncoderState> s) : state(std::move(s)) {}
    void start(const encode::EncoderConfig& c) override { state->rate = c.startBitrateBps; }
    media::EncodedVideoFrame encode(const media::RawFrame&) override
    {
        if (state->sample) { ++state->frames; media::EncodedVideoFrame out; out.data = state->sample; return out; }
        media::Bytes bytes;
        if (key) bytes = {0,0,0,1,0x67,0x42,0xe0,0x1f,0xe9,0,0,0,1,0x68,0xce,6,0xe2};
        bytes.insert(bytes.end(), {0,0,0,1,static_cast<std::uint8_t>(key ? 0x65 : 0x41)}); bytes.resize(bytes.size() + 2400, 0x5a);
        key = false; ++state->frames; media::EncodedVideoFrame out; out.data = std::make_shared<const media::Bytes>(std::move(bytes)); return out;
    }
    void setBitrate(std::uint64_t bps) override { state->rate = bps; }
    void requestKeyFrame() override { key = true; ++state->keys; }
    void stop() noexcept override {}
};
struct Pixels : media::FrameBuffer {};
struct HttpReply { unsigned status; json::object value; std::string text; };
HttpReply request(unsigned short port, http::verb method, std::string path, std::string credential, json::object body = {})
{
    net::io_context io; boost::beast::tcp_stream socket(io); socket.expires_after(5s);
    socket.connect({net::ip::make_address("127.0.0.1"), port});
    http::request<http::string_body> input(method, path, 11); input.set(http::field::host, "127.0.0.1");
    if (!credential.empty()) input.set(http::field::authorization, "Bearer " + credential);
    input.set(http::field::content_type, "application/json"); input.body() = json::serialize(body); input.prepare_payload(); http::write(socket, input);
    boost::beast::flat_buffer buffer; http::response<http::string_body> response; http::read(socket, buffer, response);
    boost::system::error_code error; auto value = json::parse(response.body(), error);
    return {response.result_int(), !error && value.is_object() ? value.as_object() : json::object{}, response.body()};
}
std::string field(const json::object& value, const char* key) { return std::string(value.at(key).as_string()); }
struct Fixture
{
    std::shared_ptr<EncoderState> backend = std::make_shared<EncoderState>();
    media::MediaService::Config config = media::MediaService::Config::forVideo(64, 64, 25);
    media::MediaService media{config, [this](auto) { return std::make_unique<Encoder>(backend); }};
    std::shared_ptr<VideoService> service;
    std::unique_ptr<uav::gateway::Gateway> gateway;
    std::uint64_t sequence{0};
    explicit Fixture(std::chrono::milliseconds connectionTimeout = 700ms)
    {
        media.start(); VideoService::Config live; live.encoder = config.encoders[1]; live.connectionTimeout = connectionTimeout;
        live.peer.ice.ice.bindings = {{{net::ip::make_address("127.0.0.1"), 0}, 1, 65535}};
        service = std::make_shared<VideoService>(media, live); service->start();
        uav::gateway::Config c; c.port = 0; c.adminToken = "live-admin"; c.viewerToken = "live-viewer";
        if (const auto root = std::getenv("UAV_GATEWAY_WEB_ROOT")) c.webRoot = root;
        if (std::getenv("UAV_WEB_TEST_SHORT_TTL")) { c.playbackLifetime = std::chrono::seconds(8); c.accessLifetime = std::chrono::seconds(8); }
        gateway = std::make_unique<uav::gateway::Gateway>(c); gateway->registerDevice({"rgb", "camera", "RGB", true, {}, {}});
        gateway->registerStream({"rgb.main", "rgb", "h264", {"webrtc"}}, [](auto) -> std::optional<uav::gateway::PlaybackEndpoint> {
            return uav::gateway::PlaybackEndpoint{"/api/v1/webrtc/rgb.main", true, {}};
        });
        gateway->registerWebRtc("rgb.main", [service = service](auto operation, auto done) {
            auto complete = [done](auto value) { done({value.status, value.sessionId, value.sdp, value.error}); };
            if (operation.sessionId.empty()) service->offer(std::move(operation.sdp), operation.credential, operation.authorized, complete);
            else service->close(operation.sessionId, operation.credential, complete);
        });
        gateway->start();
    }
    ~Fixture() { gateway->stop(); service->stop(); media.stop(); }
    json::object grant()
    {
        auto response = request(gateway->port(), http::verb::post, "/api/v1/playback", "live-viewer", {{"stream_id", "rgb.main"}, {"protocol", "webrtc"}});
        CHECK(response.status == 200); return response.value.at("data").as_object();
    }
    void submit()
    {
        auto frame = std::make_shared<media::RawVideoFrame>(); frame->sequence = sequence; frame->ptsUs = sequence++ * 40000;
        frame->width = frame->height = 64; frame->buffer = std::make_shared<Pixels>(); media.submit(frame);
    }
};
struct Receiver
{
    net::io_context io; std::unique_ptr<PeerConnection> peer; std::string sdp;
    unsigned packets{0}; std::uint32_t ssrc{0}; bool marker{false};
    Receiver(unsigned width = 64, unsigned height = 64)
    {
        PeerConnection::Config config;
        auto video = PeerMedia::video("v", {{110, "H264", 90000, 1, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"}}, SdpDirection::RecvOnly);
        if (width != 64 || height != 64)
        {
            const auto fs = ((width + 15) / 16) * ((height + 15) / 16);
            *video.codecs[0].parameters += ";max-fs=" + std::to_string(fs) + ";max-mbps=" + std::to_string(fs * 30);
        }
        video.feedback = {{110, "nack"}, {110, "nack pli"}, {110, "transport-cc"}};
        video.extmaps = {{5, {}, "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", {}}};
        config.media = {video}; config.dtls.enableSrtp = true; config.dtls.congestionControl.enabled = false;
        config.ice.ice.bindings = {{{net::ip::make_address("127.0.0.1"), 0}, 1, 65535}};
        PeerConnection::Callbacks cb;
        cb.onRtpPacket = [this](auto event) {
            RtpPacketParser packet(event.datagram.payload.data(), event.datagram.payload.size());
            CHECK(packet.valid() && packet.payloadType() == 110); ++packets; ssrc = packet.ssrc(); marker |= packet.marker();
        };
        peer = std::make_unique<PeerConnection>(io, config, cb); bool ready = false;
        peer->createOffer([&](auto ec, auto offer) { CHECK(!ec && offer); peer->setLocalDescription(*offer, [&](auto error) { CHECK(!error); ready = true; }); });
        until([&] { auto snapshot = peer->snapshot(); return ready && snapshot.ice && snapshot.ice->ice.gatheringState == IceGatheringState::Complete; }, &io);
        sdp = peer->snapshot().pendingLocalDescription->toSdp();
    }
    ~Receiver() { peer->close(); io.restart(); io.run_for(100ms); }
    void answer(std::string sdp)
    {
        bool ready = false; peer->setRemoteDescription(SdpDescriptionType::Answer, std::move(sdp), [&](auto ec) { CHECK(!ec); ready = true; });
        until([&] { return ready && peer->snapshot().connectionState == PeerConnectionState::Connected; }, &io);
        CHECK(peer->snapshot().dtls->peerFingerprint && peer->snapshot().dtls->srtpProfile);
    }
};
void fullPath()
{
    Fixture f; Receiver receiver; auto permission = f.grant(); auto token = field(permission, "media_token");
    const std::string path = "/api/v1/webrtc/rgb.main";
    CHECK(request(f.gateway->port(), http::verb::get, "/webrtc", {}).status == 503); // Headless protocol fixture has no static site.
    CHECK(request(f.gateway->port(), http::verb::post, path, "live-admin", {{"sdp", receiver.sdp}}).status == 403);
    CHECK(request(f.gateway->port(), http::verb::post, path, token, {{"sdp", "invalid"}}).status == 400);
    auto reply = request(f.gateway->port(), http::verb::post, path, token, {{"sdp", receiver.sdp}});
    if (reply.status != 201) std::cerr << reply.text << '\n';
    CHECK(reply.status == 201);
    const auto answer = reply.value.at("data").as_object(); const auto id = field(answer, "session_id");
    CHECK(field(answer, "sdp").find("a=ssrc:") != std::string::npos);
    CHECK(request(f.gateway->port(), http::verb::post, path, token, {{"sdp", receiver.sdp}}).status == 409);
    receiver.answer(field(answer, "sdp"));
    until([&] { f.submit(); return receiver.packets > 10 && receiver.marker; }, &receiver.io);
    CHECK(receiver.ssrc != 0 && f.media.snapshot().subscribers[1] == 1);
    const auto refreshed = request(f.gateway->port(), http::verb::post, "/api/v1/playback/" + field(permission, "session_id") + "/renew", "live-viewer");
    CHECK(refreshed.status == 200); const auto oldToken = token; token = field(refreshed.value.at("data").as_object(), "media_token");
    CHECK(token != oldToken && request(f.gateway->port(), http::verb::delete_, path + '/' + id, oldToken).status == 403);
    const auto keyRequests = f.backend->keys.load(); bool pli = false;
    receiver.peer->requestPli(receiver.ssrc, [&](auto ec) { CHECK(!ec); pli = true; });
    until([&] { f.submit(); return pli && f.backend->keys > keyRequests; }, &receiver.io);
    auto anotherPermission = f.grant(); auto anotherToken = field(anotherPermission, "media_token");
    CHECK(request(f.gateway->port(), http::verb::delete_, path + '/' + id, anotherToken).status == 404);
    CHECK(request(f.gateway->port(), http::verb::delete_, path + '/' + id, token).status == 200);
    until([&] { return f.media.snapshot().subscribers[1] == 0; });
    // A successful new offer proves the slot can be reused after closing the previous peer.
    Receiver second; reply = request(f.gateway->port(), http::verb::post, path, anotherToken, {{"sdp", second.sdp}}); CHECK(reply.status == 201);
    second.answer(field(reply.value.at("data").as_object(), "sdp"));
    until([&] { f.submit(); return second.packets > 0; }, &second.io);
    CHECK(request(f.gateway->port(), http::verb::delete_, "/api/v1/playback/" + field(anotherPermission, "session_id"), "live-viewer").status == 200);
    until([&] { return f.media.snapshot().subscribers[1] == 0; }); // Revocation closes an established media session.
}
void abandonedAndStop()
{
    Fixture f; Receiver receiver; auto permission = f.grant(); const auto token = field(permission, "media_token");
    CHECK(request(f.gateway->port(), http::verb::post, "/api/v1/webrtc/rgb.main", token, {{"sdp", receiver.sdp}}).status == 201);
    until([&] { return f.media.snapshot().subscribers[1] == 0; }); // Answer never applied by a client.
    CHECK(request(f.gateway->port(), http::verb::post, "/api/v1/webrtc/rgb.main", token, {{"sdp", receiver.sdp}}).status == 201);
    f.service->stop(); CHECK(f.media.snapshot().subscribers[1] == 0); f.service->start();
    CHECK(request(f.gateway->port(), http::verb::post, "/api/v1/webrtc/rgb.main", token, {{"sdp", receiver.sdp}}).status == 201);
}
struct ThreeSources
{
    const std::array<std::string, 3> ids{{"rgb.main", "rgb.sub", "thermal.main"}};
    const std::array<unsigned, 3> widths{{2566,1920,640}}, heights{{1520,1080,512}};
    std::array<std::shared_ptr<EncoderState>, 3> backends;
    std::array<std::unique_ptr<media::MediaService>, 3> media;
    std::array<std::shared_ptr<VideoService>, 3> services;
    std::unique_ptr<uav::gateway::Gateway> gateway;
    std::uint64_t sequence{0};
    std::array<std::uint64_t, 3> lastEmissionUs{};
    ThreeSources()
    {
        uav::gateway::Config c; c.port = 0; c.adminToken = "live-admin"; c.viewerToken = "live-viewer";
        if (const auto root = std::getenv("UAV_GATEWAY_WEB_ROOT")) c.webRoot = root;
        if (std::getenv("UAV_WEB_TEST_SHORT_TTL")) { c.playbackLifetime = std::chrono::seconds(8); c.accessLifetime = std::chrono::seconds(8); }
        gateway = std::make_unique<uav::gateway::Gateway>(c);
        gateway->registerDevice({"rgb", "camera", "RGB", true, {}, {}}); gateway->registerDevice({"thermal", "camera", "Thermal", true, {}, {}});
        for (unsigned i = 0; i < 3; ++i)
        {
            backends[i] = std::make_shared<EncoderState>(); auto config = media::MediaService::Config::forVideo(widths[i], heights[i], 30);
            media[i] = std::make_unique<media::MediaService>(config, [state = backends[i]](auto) { return std::make_unique<Encoder>(state); }); media[i]->start();
            VideoService::Config live; live.encoder = config.encoders[1];
            live.peer.ice.ice.bindings = {{{net::ip::make_address("127.0.0.1"), 0}, 1, 65535}};
            services[i] = std::make_shared<VideoService>(*media[i], live); services[i]->start();
            const auto fs = ((widths[i] + 15) / 16) * ((heights[i] + 15) / 16); const auto path = "/api/v1/webrtc/" + ids[i];
            gateway->registerStream({ids[i], i == 2 ? "thermal" : "rgb", "h264", {"webrtc"}}, [path, fs](auto) -> std::optional<uav::gateway::PlaybackEndpoint> {
                return uav::gateway::PlaybackEndpoint{path, true, {{"max_fs", fs}, {"max_mbps", fs * 30}}};
            });
            gateway->registerWebRtc(ids[i], [service = services[i]](auto operation, auto done) {
                auto complete = [done](auto value) { done({value.status, value.sessionId, value.sdp, value.error}); };
                if (operation.sessionId.empty()) service->offer(std::move(operation.sdp), operation.credential, operation.authorized, complete);
                else service->close(operation.sessionId, operation.credential, complete);
            });
        }
        gateway->start();
    }
    ~ThreeSources() { gateway->stop(); for (auto& service : services) service->stop(); for (auto& source : media) source->stop(); }
    json::object grant(unsigned index)
    {
        auto reply = request(gateway->port(), http::verb::post, "/api/v1/playback", "live-viewer", {{"stream_id", ids[index]}, {"protocol", "webrtc"}});
        CHECK(reply.status == 200); return reply.value.at("data").as_object();
    }
    void submit()
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            // A replayed IDR cannot change size like VENC. Throttle this browser fixture
            // to its assigned encoder budget so the fixture itself cannot flood the pacer.
            if (backends[i]->sample)
            {
                const auto rate = std::max<std::uint64_t>(backends[i]->rate.load(), 1000);
                const auto intervalUs = backends[i]->sample->size() * 8000000ull / rate;
                if (lastEmissionUs[i] && sequence * 40000 < lastEmissionUs[i] + intervalUs) continue;
                lastEmissionUs[i] = sequence * 40000;
            }
            auto f = std::make_shared<media::RawVideoFrame>(); f->width = widths[i]; f->height = heights[i];
            f->ptsUs = sequence * 40000; f->sequence = sequence; f->buffer = std::make_shared<Pixels>(); media[i]->submit(f);
        }
        ++sequence;
    }
};
void threeConcurrentPeers()
{
    ThreeSources f; std::array<std::unique_ptr<Receiver>, 3> receivers; std::array<std::string, 3> credentials, ids;
    Receiver insufficient;
    for (unsigned i = 0; i < 3; ++i)
    {
        receivers[i] = std::make_unique<Receiver>(f.widths[i], f.heights[i]); const auto grant = f.grant(i); credentials[i] = field(grant, "media_token");
        const auto path = "/api/v1/webrtc/" + f.ids[i];
        if (i < 2) CHECK(request(f.gateway->port(), http::verb::post, path, credentials[i], {{"sdp", insufficient.sdp}}).status == 406);
        const auto reply = request(f.gateway->port(), http::verb::post, path, credentials[i], {{"sdp", receivers[i]->sdp}}); CHECK(reply.status == 201);
        const auto answer = reply.value.at("data").as_object(); ids[i] = field(answer, "session_id"); receivers[i]->answer(field(answer, "sdp"));
        CHECK(request(f.gateway->port(), http::verb::post, path, credentials[i], {{"sdp", receivers[i]->sdp}}).status == 409);
    }
    until([&] {
        f.submit(); bool received = true;
        for (auto& receiver : receivers) { receiver->io.restart(); receiver->io.run_for(2ms); received &= receiver->packets > 0; } return received;
    });
    for (unsigned i = 0; i < 3; ++i)
    {
        CHECK(f.media[i]->snapshot().subscribers[1] == 1);
        CHECK(request(f.gateway->port(), http::verb::delete_, "/api/v1/webrtc/" + f.ids[i] + '/' + ids[i], credentials[i]).status == 200);
    }
}
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 5 && std::string(argv[1]) == "--serve-three")
        {
            ThreeSources f;
            for (unsigned i = 0; i < 3; ++i)
            {
                std::ifstream input(argv[2 + i], std::ios::binary); CHECK(input);
                f.backends[i]->sample = std::make_shared<const media::Bytes>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
                CHECK(!f.backends[i]->sample->empty());
            }
            std::signal(SIGTERM, stop); std::signal(SIGINT, stop); std::cout << "PORT " << f.gateway->port() << std::endl;
            while (!stopped) { f.submit(); std::this_thread::sleep_for(40ms); } return 0;
        }
        // Optional real-browser fixture: FFmpeg produces a valid AU, then the production path serves it.
        if (argc == 3 && std::string(argv[1]) == "--serve")
        {
            std::ifstream input(argv[2], std::ios::binary); CHECK(input);
            auto sample = std::make_shared<const media::Bytes>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()); CHECK(!sample->empty());
            Fixture f(20s); f.backend->sample = std::move(sample); std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
            std::cout << "PORT " << f.gateway->port() << std::endl;
            while (!stopped) { f.submit(); std::this_thread::sleep_for(40ms); } return 0;
        }
        fullPath(); abandonedAndStop(); threeConcurrentPeers(); std::cout << "PASS: three concurrent video sources, HTTP/SDP -> ICE/DTLS/SRTP -> RTP, PLI, admission, renew/revoke and shutdown\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
