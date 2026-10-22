#include"media/media_service.h"
#include"media/h264_packetizer.h"
#include"moq/video_source.h"
#include"webrtc/video_sender.h"
#include"rtsp/live_source.h"
#include"rtsp/rtp_transport.h"
#include"common/rtp_packet.h"
#include<algorithm>
#include<atomic>
#include<condition_variable>
#include<iostream>
#include<mutex>
#include<stdexcept>
#include<thread>

using namespace std::chrono_literals;
using namespace media;
namespace net = boost::asio;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
template<class F> void until(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!f() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    CHECK(f());
}
struct Pixels : FrameBuffer {};
RawFrame frame(std::uint64_t seq, std::uint64_t epoch = 0)
{
    auto f = std::make_shared<RawVideoFrame>(); f->width = 64; f->height = 64; f->sequence = seq; f->sourceEpoch = epoch;
    f->ptsUs = seq * 40000; f->buffer = std::make_shared<Pixels>(); return f;
}
struct BackendState
{
    std::atomic<unsigned> starts{0}, stops{0}, frames{0}, idrs{0}; std::atomic<std::uint64_t> rate{0};
    std::atomic<const void*> input{nullptr};
    std::mutex mutex; std::condition_variable wake; bool hold{false}, entered{false}, failStart{false};
    std::thread::id controlThread;
};
struct FakeEncoder : encode::VideoEncoder
{
    std::shared_ptr<BackendState> state; encode::EncoderConfig config; bool started{false}, key{false};
    explicit FakeEncoder(std::shared_ptr<BackendState> s) : state(std::move(s)) {}
    void start(const encode::EncoderConfig& c) override
    {
        ++state->starts; state->controlThread = std::this_thread::get_id(); if (state->failStart) throw std::runtime_error("injected encoder start failure");
        config = c; started = true; state->rate = c.startBitrateBps;
    }
    void setBitrate(std::uint64_t bps) override { CHECK(state->controlThread == std::this_thread::get_id()); state->rate = bps; }
    void requestKeyFrame() override { ++state->idrs; key = true; }
    EncodedVideoFrame encode(const RawFrame& f) override
    {
        state->input = f.get();
        { std::unique_lock<std::mutex> lock(state->mutex); state->entered = true; state->wake.notify_all(); state->wake.wait(lock, [&] { return !state->hold; }); }
        const auto n = state->frames.fetch_add(1); const bool idr = key || n % config.gop == 0; key = false;
        Bytes bytes;
        if (idr) bytes = {0,0,0,1,0x67,0x42,0,0x1f,0xe9,0,0,0,1,0x68,0xce,6,0xe2};
        bytes.insert(bytes.end(), {0,0,0,1,static_cast<std::uint8_t>(idr ? 0x65 : 0x41)}); bytes.resize(bytes.size() + 2400, 0x5a);
        EncodedVideoFrame result; result.data = std::make_shared<const Bytes>(std::move(bytes)); return result;
    }
    void stop() noexcept override { if (started) { ++state->stops; started = false; } }
};
struct Fixture
{
    std::array<std::shared_ptr<BackendState>, 3> states;
    std::unique_ptr<MediaService> service;
    explicit Fixture(MediaService::Config c = config())
    {
        for (auto& s : states) s = std::make_shared<BackendState>();
        service = std::make_unique<MediaService>(c, [&](auto output) { return std::make_unique<FakeEncoder>(states[static_cast<unsigned>(output)]); });
    }
    static MediaService::Config config()
    { auto c = MediaService::Config::forVideo(64, 64, 25); for (auto& e : c.encoders) e.gop = 4; return c; }
};
void sharedEncodersAndAdmission()
{
    Fixture f; f.service->start();
    std::vector<std::shared_ptr<MediaSubscription>> rtsp; std::vector<std::unique_ptr<moq::VideoSource>> moq;
    for (unsigned i = 0; i < 128; ++i) { rtsp.push_back(f.service->subscribe(Output::Rtsp)); moq.push_back(std::make_unique<moq::VideoSource>(*f.service)); }
    auto rtc = f.service->subscribe(Output::WebRtc); bool denied = false;
    try { (void)f.service->subscribe(Output::WebRtc); } catch (const std::system_error& e) { CHECK(e.code() == std::errc::resource_unavailable_try_again); denied = true; }
    CHECK(denied);
    auto raw = frame(0); CHECK(f.service->submit(raw) == 3);
    until([&] { return rtsp.back()->snapshot().queuedFrames && moq.back()->snapshot().queuedFrames && rtc->snapshot().queuedFrames; });
    auto firstRtsp = rtsp[0]->tryPop(); auto firstMoq = moq[0]->next(); CHECK(firstRtsp && firstMoq);
    for (unsigned i = 1; i < 128; ++i)
    {
        CHECK(rtsp[i]->tryPop() == firstRtsp);
        auto object = moq[i]->next(); CHECK(object && object->frame == firstMoq->frame && object->groupId == firstMoq->groupId && object->objectId == 0);
    }
    CHECK(firstRtsp->keyFrame && completeParameterSets(firstRtsp->codec, firstRtsp->parameterSets));
    for (const auto& s : f.states) CHECK(s->starts == 1 && s->input == raw.get() && s->controlThread != std::this_thread::get_id());
    CHECK(f.service->snapshot().subscribers == (std::array<std::size_t, 3>{128, 1, 128}));
    rtc.reset(); CHECK(f.service->snapshot().subscribers[1] == 0);
    std::vector<std::shared_ptr<MediaSubscription>> candidates(16); std::vector<std::thread> contenders;
    std::atomic<unsigned> admitted{0};
    for (unsigned i = 0; i < candidates.size(); ++i) contenders.emplace_back([&, i] {
        try { candidates[i] = f.service->subscribe(Output::WebRtc); ++admitted; } catch (const std::system_error&) {}
    });
    for (auto& t : contenders) t.join();
    CHECK(admitted == 1);
    f.service->stop(); for (const auto& s : f.states) CHECK(s->stops == 1);
    CHECK(rtsp[0]->snapshot().closed && moq[0]->snapshot().closed && !f.service->submit(frame(1)));
}
void independentRates()
{
    Fixture f; f.service->start(); auto rtsp = f.service->subscribe(Output::Rtsp), rtc = f.service->subscribe(Output::WebRtc);
    moq::VideoSource a(*f.service), b(*f.service);
    CHECK(!rtsp->updateBudget({0, 100000, 0, 0}));
    CHECK(rtc->updateBudget({4, 1000000, 20000, 0.1}));
    CHECK(a.updateBudget({0, 2000000, 0, 0}) && b.updateBudget({0, 500000, 0, 0}));
    until([&] { return f.states[1]->rate == 880000 && f.states[2]->rate == 500000; }); CHECK(f.states[0]->rate == 2000000);
    CHECK(!rtc->updateBudget({3, 100000, 0, 0})); CHECK(!rtc->updateBudget({4, 100000, 0, 2}));
    b.close(); until([&] { return f.states[2]->rate == 2000000; });
    CHECK(a.updateBudget({1, 500, 0, 0})); CHECK(f.service->snapshot().encoders[2].paused);
    CHECK(!f.service->snapshot().encoders[0].paused && !f.service->snapshot().encoders[1].paused);
    CHECK(a.updateBudget({2, 2000000, 0, 0})); until([&] { return !f.service->snapshot().encoders[2].paused; });
    CHECK(rtc->updateBudget({4, 0, 0, 0})); CHECK(f.service->snapshot().encoders[1].paused);
    CHECK(f.service->submit(frame(0)) == 2);
    until([&] { return f.states[0]->frames == 1 && f.states[2]->frames == 1; }); CHECK(f.states[1]->frames == 0);
    CHECK(rtc->updateBudget({5, 900000, 0, 0})); until([&] { return f.states[1]->rate == 900000; });
    CHECK(f.service->submit(frame(1)) == 3); until([&] { return rtc->snapshot().queuedFrames == 1; }); CHECK(rtc->tryPop()->keyFrame);
    f.service->stop();
}
void queuesAndEpochs()
{
    auto c = Fixture::config(); c.maxSubscriberFrames = 2; c.maxSubscriberDelay = 30ms;
    Fixture f(c); f.service->start(); auto fast = f.service->subscribe(Output::Rtsp), slow = f.service->subscribe(Output::Rtsp);
    for (unsigned i = 0; i < 12; ++i)
    {
        CHECK(f.service->submit(frame(i)) == 1); until([&] { return f.states[0]->frames == i + 1; });
        until([&] { return fast->snapshot().queuedFrames > 0; }); CHECK(fast->tryPop());
        CHECK(slow->snapshot().queuedFrames <= 2);
    }
    CHECK(fast->snapshot().droppedFrames == 0 && slow->snapshot().droppedFrames > 0);
    slow->resync(); CHECK(slow->snapshot().queuedFrames == 0 && slow->snapshot().waitingForKeyFrame);
    auto next = frame(0, 1); CHECK(f.service->submit(next) == 1);
    until([&] { return fast->snapshot().queuedFrames > 0; }); const auto key = fast->tryPop(); CHECK(key->keyFrame && key->sourceEpoch == 1);
    CHECK(!f.service->submit(frame(100, 0)));
    until([&] { return slow->snapshot().queuedFrames > 0; }); std::this_thread::sleep_for(40ms);
    CHECK(!slow->tryPop() && slow->snapshot().waitingForKeyFrame);
    f.service->stop(); f.service->start(); CHECK(f.service->snapshot().subscribers[0] == 0); f.service->stop();
}
void boundedRawOwnershipAndRollback()
{
    Fixture f; f.service->start(); auto sub = f.service->subscribe(Output::Rtsp);
    { std::lock_guard<std::mutex> lock(f.states[0]->mutex); f.states[0]->hold = true; }
    auto raw = frame(0); std::weak_ptr<const FrameBuffer> lease = raw->buffer; CHECK(f.service->submit(raw) == 1); raw.reset();
    until([&] { std::lock_guard<std::mutex> lock(f.states[0]->mutex); return f.states[0]->entered; }); CHECK(!lease.expired());
    for (unsigned i = 1; i < 40; ++i) (void)f.service->submit(frame(i));
    CHECK(f.service->snapshot().encoders[0].pendingFrames <= 2 && f.service->snapshot().encoders[0].droppedInputs > 0);
    std::atomic<bool> stopped{false}; std::thread stop([&] { f.service->stop(); stopped = true; });
    std::this_thread::sleep_for(10ms); CHECK(!stopped);
    { std::lock_guard<std::mutex> lock(f.states[0]->mutex); f.states[0]->hold = false; } f.states[0]->wake.notify_all(); stop.join();
    CHECK(stopped && lease.expired() && sub->snapshot().closed);
    Fixture failing; failing.states[1]->failStart = true; bool failed = false;
    try { failing.service->start(); } catch (const std::runtime_error&) { failed = true; }
    CHECK(failed && failing.states[0]->stops == 1 && !failing.service->snapshot().running);
}
void inFlightSourceSwitchAndRestart()
{
    Fixture f; f.service->start(); moq::VideoSource source(*f.service);
    { std::lock_guard<std::mutex> lock(f.states[2]->mutex); f.states[2]->hold = true; }
    CHECK(f.service->submit(frame(0)) == 1);
    until([&] { std::lock_guard<std::mutex> lock(f.states[2]->mutex); return f.states[2]->entered; });
    CHECK(f.service->submit(frame(0, 1)) == 1); // Replace the source while its previous frame is in VENC.
    { std::lock_guard<std::mutex> lock(f.states[2]->mutex); f.states[2]->hold = false; } f.states[2]->wake.notify_all();
    until([&] { return f.states[2]->frames == 2 && source.snapshot().queuedFrames == 1; });
    auto object = source.next(); CHECK(object && object->frame->sourceEpoch == 1 && object->frame->keyFrame && object->objectId == 0);
    const auto oldEpoch = object->epoch;
    CHECK(f.service->submit(frame(100, 1)) == 1);
    until([&] { return source.snapshot().queuedFrames == 1; }); CHECK(source.next());
    auto tooEarly = std::make_shared<RawVideoFrame>(*frame(100, 1)); ++tooEarly->ptsUs;
    CHECK(!f.service->submit(tooEarly)); // A timestamp jump must not permit an immediate extra frame.
    f.service->stop(); f.service->start(); moq::VideoSource restarted(*f.service);
    CHECK(f.service->submit(frame(0)) == 1); until([&] { return restarted.snapshot().queuedFrames == 1; });
    auto first = restarted.next(); CHECK(first && first->epoch > oldEpoch && first->objectId == 0 && first->frame->keyFrame);
    restarted.resync(); CHECK(restarted.snapshot().waitingForKeyFrame); f.service->stop();
}
void frameRateSampling()
{
    auto config = Fixture::config(); config.encoders[0].fps = 30;
    Fixture f(config); f.service->start(); auto subscription = f.service->subscribe(Output::Rtsp);
    unsigned accepted = 0;
    for (unsigned i = 0; i < 100; ++i)
    {
        auto input = std::make_shared<RawVideoFrame>(*frame(i)); input->ptsUs = i * 20000;
        accepted += f.service->submit(input) == 1;
    }
    CHECK(accepted == 60); f.service->stop();
}
void protocolAdapters()
{
    Fixture f; f.service->start(); net::io_context io;
    auto rtc = f.service->subscribe(Output::WebRtc); std::vector<Bytes> packets; bool fail = false; unsigned oldTarget = 0;
    webrtc::VideoSender::Config cfg; cfg.rtp.ssrc = 1234; cfg.rtp.maxPayloadBytes = 500;
    webrtc::VideoSender sender(io, rtc, [&](auto packet, auto done) {
        if (fail) { fail = false; done(net::error::network_unreachable, 0); }
        else { packets.push_back(packet); done({}, packet.size()); }
    }, cfg);
    webrtc::PeerConnection::Callbacks cb; cb.onTargetTransferRate = [&](auto) { ++oldTarget; }; sender.bindCallbacks(cb);
    webrtc::CongestionControlEvent rate; rate.rate.target_rate = 600000; cb.onTargetTransferRate(rate);
    until([&] { return f.states[1]->rate == 600000; }); CHECK(oldTarget == 1);
    cb.onConnectionState(webrtc::PeerConnectionState::Connected); io.restart(); io.run_for(2ms);
    CHECK(f.service->submit(frame(0)) == 1);
    const auto runUntil = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); } CHECK(predicate());
    };
    runUntil([&] { return !packets.empty() && (packets.back()[1] & 0x80); });
    std::uint16_t sequence = (packets[0][2] << 8) | packets[0][3]; bool fragmented = false; Bytes reassembled;
    for (std::size_t i = 0; i < packets.size(); ++i)
    {
        RtpPacketParser p(packets[i].data(), packets[i].size()); CHECK(p.valid() && p.ssrc() == 1234 && p.sequenceNumber() == sequence++);
        CHECK(p.marker() == (i + 1 == packets.size()) && p.payloadSize() <= 500); fragmented |= (p.payload()[0] & 31) == 28;
        if ((p.payload()[0] & 31) == 28)
        {
            if (p.payload()[1] & 0x80) reassembled.push_back((p.payload()[0] & 0xe0) | (p.payload()[1] & 31));
            reassembled.insert(reassembled.end(), p.payload() + 2, p.payload() + p.payloadSize());
        }
    }
    CHECK(fragmented && reassembled.size() == 2401 && reassembled[0] == 0x65);
    CHECK(std::all_of(reassembled.begin() + 1, reassembled.end(), [](auto byte) { return byte == 0x5a; }));
    fail = true; CHECK(f.service->submit(frame(1)) == 1);
    runUntil([&] { return !fail; }); CHECK(rtc->snapshot().waitingForKeyFrame);
    cb.onConnectionState(webrtc::PeerConnectionState::Closed); CHECK(rtc->snapshot().closed); io.restart(); io.run_for(5ms);
    auto replacement = f.service->subscribe(Output::WebRtc); replacement->close();

    auto subscription = f.service->subscribe(Output::Rtsp); LiveRtpSource source("/live", subscription);
    CHECK(source.makeSdp("127.0.0.1", 8554).find("packetization-mode=1") != std::string::npos);
    unsigned received = 0; std::uint32_t lastTimestamp = 0; std::vector<std::shared_ptr<RtpTransport>> clients;
    for (unsigned i = 0; i < 32; ++i)
    {
        RtpTransport::Config config; config.mode = RtpTransport::Mode::TCP; config.ssrc = source.ssrc();
        auto sink = std::make_shared<RtpTransport>(io, config, [&](auto channel, const auto& packet) {
            if (channel == 0) { RtpPacketParser p(packet->data(), packet->size()); CHECK(p.valid()); lastTimestamp = p.timestamp(); ++received; }
        }); sink->setPlaying(true); source.addSink(sink); clients.push_back(sink);
    }
    CHECK(f.service->submit(frame(2)) == 1); until([&] { return subscription->snapshot().queuedFrames > 0; }); CHECK(source.pump() == 1);
    io.restart(); io.run_for(10ms); CHECK(received >= 32);
    const auto previousTimestamp = lastTimestamp; const auto previousReceived = received;
    CHECK(f.service->submit(frame(0, 1)) == 1); until([&] { return subscription->snapshot().queuedFrames > 0; }); CHECK(source.pump() == 1);
    io.restart(); io.run_for(10ms); CHECK(received > previousReceived && lastTimestamp - previousTimestamp == 3600);
    CHECK(f.states[0]->starts == 1); for (const auto& client : clients) client->close(); f.service->stop();
}
}
int main()
{
    try
    {
        sharedEncodersAndAdmission(); independentRates(); queuesAndEpochs(); boundedRawOwnershipAndRollback(); inFlightSourceSwitchAndRestart(); frameRateSampling(); protocolAdapters();
        std::cout << "PASS: three encoders, 128 shared RTSP/MoQ subscribers, single WebRTC admission, independent budgets, leases, GOP recovery and protocol adapters\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
