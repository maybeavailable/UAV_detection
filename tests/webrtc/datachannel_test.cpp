#include"webrtc/datachannel.h"

#include<chrono>
#include<iostream>
#include<stdexcept>
#include<thread>

using namespace webrtc;
namespace net = boost::asio;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
template<class Predicate> void until(net::io_context& io, Predicate predicate, std::chrono::milliseconds timeout = 6000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
void codecs()
{
    DataChannelConfig c; c.label = "测试"; c.protocol = "json"; c.ordered = false; c.maxRetransmits = 0; c.priority = 512;
    const auto bytes = DcepOpen{c}.encode(); CHECK(bytes[0] == 3 && bytes[1] == 0x81 && bytes[2] == 2 && bytes[3] == 0);
    DcepOpen parsed; CHECK(DcepOpen::parse(bytes, parsed) && parsed.channel.label == c.label && parsed.channel.protocol == c.protocol);
    CHECK(!parsed.channel.ordered && parsed.channel.maxRetransmits == 0);
    auto invalid = bytes; invalid.pop_back(); CHECK(!DcepOpen::parse(invalid, parsed)); CHECK(parsed.channel.label == c.label);
    invalid = bytes; invalid[1] = 3; CHECK(!DcepOpen::parse(invalid, parsed));
    c.maxPacketLifeTime = 10; CHECK(!c.validate()); c.maxRetransmits.reset(); CHECK(c.validate());
    c.label = std::string("\xc0\xaf", 2); CHECK(!c.validate());
    c.label.clear(); c.negotiated = true; CHECK(!c.validate()); c.id = 65535; CHECK(!c.validate()); c.id = 0; CHECK(c.validate());
    c.negotiated = false; c.maxPacketLifeTime.reset(); auto reliable = DcepOpen{c}.encode(); reliable[4] = 255;
    CHECK(DcepOpen::parse(reliable, parsed) && !parsed.channel.maxRetransmits && !parsed.channel.maxPacketLifeTime);
}
struct Pair
{
    net::io_context io;
    std::unique_ptr<DataChannelTransport> a, b;
    std::shared_ptr<DataChannel> local, remote;
    std::vector<DataChannelMessage> atA, atB;
    std::vector<std::shared_ptr<DataChannel>> remotes;
    std::vector<UdpSocket::Bytes> wire;
    unsigned errors{0}, opened{0};
    bool dropNext{false};
    std::uint64_t generation{0};
    Pair(std::size_t maxMessage = 65536, std::size_t sendMessages = 256, std::size_t sendBytes = 2 * 1024 * 1024,
        std::size_t receiveBytes = 4 * 1024 * 1024)
    {
        DataChannelTransport::Config cfg; cfg.maxMessageSize = maxMessage;
        cfg.maxBufferedSendMessages = sendMessages; cfg.maxBufferedSendBytes = sendBytes;
        cfg.maxBufferedReceiveBytes = receiveBytes;
        DataChannelTransport::Callbacks ca, cb;
        ca.onError = cb.onError = [&](auto ec, auto message) { ++errors; std::cerr << ec.message() << ": " << message << '\n'; };
        cb.onDataChannel = [&](auto c) {
            remote = c; remotes.push_back(c); DataChannel::Callbacks callbacks;
            callbacks.onMessage = [&](auto m) { atB.push_back(std::move(m)); };
            callbacks.onOpen = [&] { ++opened; }; c->setCallbacks(std::move(callbacks));
        };
        a = std::make_unique<DataChannelTransport>(io, [&](auto bytes, auto done) {
            CHECK(bytes.size() <= 1100); const auto n = bytes.size(); wire.push_back(bytes);
            if (dropNext) dropNext = false; else b->handlePacket(std::move(bytes), generation);
            done({}, n);
        }, cfg, ca);
        b = std::make_unique<DataChannelTransport>(io, [&](auto bytes, auto done) {
            CHECK(bytes.size() <= 1100); const auto n = bytes.size(); wire.push_back(bytes); a->handlePacket(std::move(bytes), generation); done({}, n);
        }, cfg, cb);
    }
    void start(std::size_t max = 65536)
    {
        DataChannelTransport::Parameters pa; pa.generation = generation; pa.localMaxMessageSize = max; pa.remoteMaxMessageSize = max;
        auto pb = pa; pb.localSetup = SdpSetupRole::Passive;
        a->start(pa, [](auto ec) { CHECK(!ec); }); b->start(pb, [](auto ec) { CHECK(!ec); });
    }
    void create(DataChannelConfig config = {})
    {
        local.reset();
        DataChannel::Callbacks callbacks; callbacks.onOpen = [&] { ++opened; };
        callbacks.onMessage = [&](auto m) { atA.push_back(std::move(m)); };
        a->createDataChannel(config, callbacks, [&](auto ec, auto c) { CHECK(!ec && c); local = c; });
        until(io, [&] { return bool(local); });
    }
    void connected()
    {
        until(io, [&] { return local && remote && local->snapshot().state == DataChannelState::Open && remote->snapshot().state == DataChannelState::Open; });
        CHECK(a->snapshot().state == DataChannelTransportState::Connected && b->snapshot().state == DataChannelTransportState::Connected && errors == 0);
        CHECK(local->snapshot().config.id && *local->snapshot().config.id % 2 == 0 && remote->snapshot().config.id == local->snapshot().config.id);
    }
    void close()
    {
        unsigned n = 0; a->close([&](auto ec) { CHECK(!ec); ++n; }); b->close([&](auto ec) { CHECK(!ec); ++n; }); until(io, [&] { return n == 2; });
    }
};
void data()
{
    Pair p(512 * 1024); DataChannelConfig config; config.label = "chat"; config.protocol = "test";
    p.create(config); p.start(512 * 1024); p.connected(); CHECK(p.opened == 2);
    unsigned sent = 0;
    p.local->sendText("你好", [&](auto ec, auto size) { CHECK(!ec && size == 6); ++sent; });
    p.local->sendText("", [&](auto ec, auto size) { CHECK(!ec && size == 0); ++sent; });
    p.remote->sendBinary({}, [&](auto ec, auto size) { CHECK(!ec && size == 0); ++sent; });
    UdpSocket::Bytes large(256 * 1024); for (std::size_t i = 0; i < large.size(); ++i) large[i] = i % 251;
    p.remote->sendBinary(large, [&](auto ec, auto size) { CHECK(!ec && size == large.size()); ++sent; });
    until(p.io, [&] { return sent == 4 && p.atA.size() == 2 && p.atB.size() == 2; });
    CHECK(!p.atB[0].binary && p.atB[0].data == UdpSocket::Bytes({0xe4,0xbd,0xa0,0xe5,0xa5,0xbd}));
    CHECK(p.atB[1].data.empty() && !p.atB[1].binary && p.atA[0].binary && p.atA[0].data.empty() && p.atA[1].data == large);
    bool done = false;
    p.local->close([&](auto ec) { CHECK(!ec); done = true; });
    until(p.io, [&] { return done && p.local->snapshot().state == DataChannelState::Closed && p.remote->snapshot().state == DataChannelState::Closed; });
    p.close();
}
void negotiatedAndPause()
{
    Pair p; DataChannelConfig cfg; cfg.negotiated = true; cfg.id = 7; cfg.ordered = false; cfg.maxRetransmits = 0;
    p.create(cfg);
    DataChannel::Callbacks cb; cb.onMessage = [&](auto m) { p.atB.push_back(std::move(m)); };
    p.b->createDataChannel(cfg, cb, [&](auto ec, auto c) { CHECK(!ec); p.remote = std::move(c); });
    p.start(); until(p.io, [&] { return p.remote && p.local->snapshot().state == DataChannelState::Open && p.remote->snapshot().state == DataChannelState::Open; });
    p.generation = 1; p.a->pause(1); p.b->pause(1);
    bool sent = false, low = false;
    DataChannel::Callbacks callbacks; callbacks.onBufferedAmountLow = [&] { low = true; }; p.local->setCallbacks(callbacks);
    p.local->sendText("queued", [&](auto ec, auto n) { CHECK(!ec && n == 6); sent = true; });
    until(p.io, [&] { return p.local->snapshot().bufferedAmount == 6; }); CHECK(!sent);
    p.start(); until(p.io, [&] { return sent && low && p.atB.size() == 1; });
    CHECK(p.local->snapshot().state == DataChannelState::Open && p.atB[0].data == UdpSocket::Bytes({'q','u','e','u','e','d'})); p.close();
}
void lossLargeDcepAndCancellation()
{
    Pair p; DataChannelConfig cfg; cfg.label.assign(65535, 'x'); cfg.protocol.assign(65535, 'y');
    p.create(cfg); p.start(); p.connected();
    CHECK(p.remote->snapshot().config.label == cfg.label && p.remote->snapshot().config.protocol == cfg.protocol);
    p.dropNext = true; bool sent = false;
    p.local->sendText("retransmit", [&](auto ec, auto n) { CHECK(!ec && n == 10); sent = true; });
    until(p.io, [&] { return sent && p.atB.size() == 1; });
    CHECK(!p.dropNext && p.atB[0].data == UdpSocket::Bytes({'r','e','t','r','a','n','s','m','i','t'}));
    p.a->pause(0); bool cancelled = false;
    p.local->sendText("cancel", [&](auto ec, auto n) { CHECK(ec == net::error::operation_aborted && n == 0); cancelled = true; });
    until(p.io, [&] { return p.local->snapshot().bufferedAmount == 6; });
    p.close(); CHECK(cancelled);
}
void expiryAndBackpressure()
{
    Pair p; DataChannelConfig cfg; cfg.negotiated = true; cfg.id = 2; cfg.maxPacketLifeTime = 20;
    p.create(cfg); p.b->createDataChannel(cfg, [&](auto ec, auto c) { CHECK(!ec); p.remote = c; }); p.start();
    until(p.io, [&] { return p.remote && p.local->snapshot().state == DataChannelState::Open && p.remote->snapshot().state == DataChannelState::Open; });
    p.a->pause(0); bool expired = false;
    p.local->sendText("expire", [&](auto ec, auto n) { CHECK(ec == net::error::timed_out && n == 0); expired = true; });
    p.io.restart(); p.io.run_for(40ms); CHECK(!expired && p.local->snapshot().bufferedAmount == 6);
    p.start(); until(p.io, [&] { return expired; }); CHECK(p.atB.empty()); p.close();

    net::io_context io; DataChannelTransport::Config c; c.connectTimeout = 40ms;
    unsigned errors = 0; DataChannelTransport::Callbacks callbacks;
    callbacks.onError = [&](auto ec, auto) { CHECK(ec == net::error::timed_out); ++errors; };
    DataChannelTransport timeout(io, [](auto bytes, auto done) { done({}, bytes.size()); }, c, callbacks);
    std::shared_ptr<DataChannel> channel; timeout.createDataChannel({}, [&](auto ec, auto p) { CHECK(!ec); channel = p; });
    timeout.start({}, [](auto ec) { CHECK(!ec); });
    until(io, [&] { return timeout.snapshot().state == DataChannelTransportState::Failed; });
    CHECK(channel && channel->snapshot().state == DataChannelState::Closed && errors == 1);
    bool closed = false; timeout.close([&](auto ec) { CHECK(!ec); closed = true; }); until(io, [&] { return closed; });
}
void limits()
{
    auto transportConfig = DataChannelTransport::Config{};
    CHECK(transportConfig.validate()); transportConfig.maxMessageSize = 0; CHECK(!transportConfig.validate());
    Pair p; p.create(); bool early = false;
    p.local->sendText("early", [&](auto ec, auto size) { CHECK(ec == net::error::not_connected && size == 0); early = true; });
    p.start(); p.connected(); CHECK(early);
    unsigned failed = 0;
    p.local->sendBinary(UdpSocket::Bytes(65537), [&](auto ec, auto size) { CHECK(ec == net::error::message_size && size == 0); ++failed; });
    p.local->sendText(std::string("\xc0\xaf", 2), [&](auto ec, auto size) { CHECK(ec == net::error::invalid_argument && size == 0); ++failed; });
    DataChannelConfig invalid; invalid.id = 1;
    p.a->createDataChannel(invalid, [&](auto ec, auto c) { CHECK(ec == net::error::invalid_argument && !c); ++failed; });
    until(p.io, [&] { return failed == 3; }); p.close();
}
void sendQueueAndClose()
{
    Pair p(65536, 2, 65536); p.create(); p.start(); p.connected();
    p.a->pause(0); until(p.io, [&] { return p.a->snapshot().paused; });
    unsigned rejected = 0, cancelled = 0;
    auto accepted = [&](auto ec, auto n) { CHECK(ec == net::error::operation_aborted && n == 0); ++cancelled; };
    auto full = [&](auto ec, auto n) { CHECK(ec == net::error::no_buffer_space && n == 0); ++rejected; };
    p.local->sendBinary(UdpSocket::Bytes(40000), accepted);
    p.local->sendBinary(UdpSocket::Bytes(30000), full); // 受字节上限限制
    p.local->sendText("", accepted);
    p.local->sendText("", full); // 空消息也占用消息数额度
    until(p.io, [&] { return rejected == 2 && p.local->snapshot().bufferedAmount == 40000; });
    p.close(); CHECK(cancelled == 2 && p.local->snapshot().bufferedAmount == 0);

    Pair concurrent(65536, 4); concurrent.create(); concurrent.start(); concurrent.connected();
    concurrent.a->pause(0); until(concurrent.io, [&] { return concurrent.a->snapshot().paused; });
    unsigned callbacks = 0, capacity = 0, aborted = 0;
    std::vector<std::thread> producers;
    for (unsigned i = 0; i < 8; ++i) producers.emplace_back([&] {
        for (unsigned j = 0; j < 8; ++j) concurrent.local->sendBinary(UdpSocket::Bytes(1024), [&](auto ec, auto n) {
            CHECK(n == 0); ++callbacks;
            if (ec == net::error::no_buffer_space) ++capacity;
            else { CHECK(ec == net::error::operation_aborted); ++aborted; }
        });
    });
    for (auto& thread : producers) thread.join();
    // io_context 尚未处理发送请求：投递前就必须限制跨线程排队量。
    concurrent.close(); CHECK(callbacks == 64 && capacity == 60 && aborted == 4);

    net::io_context io; UdpSocket::SendHandler pending;
    DataChannelTransport transport(io, [&](auto, auto done) { CHECK(!pending); pending = std::move(done); });
    transport.start({}); until(io, [&] { return bool(pending); });
    bool closed = false; transport.close([&](auto ec) { CHECK(!ec); closed = true; });
    io.restart(); io.run_for(20ms); CHECK(!closed);
    auto completion = std::move(pending); completion(net::error::operation_aborted, 0);
    until(io, [&] { return closed; });
}
void simultaneousResetAndReuse()
{
    Pair p; std::vector<std::shared_ptr<DataChannel>> locals;
    p.create(); locals.push_back(p.local); p.start(); p.connected();
    for (unsigned i = 0; i < 2; ++i) { p.create(); locals.push_back(p.local); p.connected(); }
    CHECK(p.remotes.size() == 3);
    const auto id = locals.front()->snapshot().config.id;
    for (auto& c : locals) c->close();
    until(p.io, [&] {
        for (const auto& c : locals) if (c->snapshot().state != DataChannelState::Closed) return false;
        for (const auto& c : p.remotes) if (c->snapshot().state != DataChannelState::Closed) return false;
        return true;
    });
    DataChannelConfig reused; reused.id = id; p.create(reused); p.connected();
    CHECK(p.local->snapshot().config.id == id && p.remotes.size() == 4);
    p.local->sendText("reused"); until(p.io, [&] { return p.atB.size() == 1; });
    CHECK(p.atB.back().data == UdpSocket::Bytes({'r','e','u','s','e','d'})); p.close();
}
void receiveBackpressure()
{
    Pair p(65536, 256, 2 * 1024 * 1024, 12 + 2 * 65535); p.create(); p.start(); p.connected();
    p.remote->setCallbacks({});
    p.io.restart(); p.io.run_for(20ms);
    unsigned sent = 0;
    auto done = [&](auto ec, auto) { CHECK(!ec); ++sent; };
    p.local->sendBinary(UdpSocket::Bytes(64000, 1), done);
    p.local->sendBinary(UdpSocket::Bytes(64000, 2), done);
    p.local->sendBinary(UdpSocket::Bytes(4096, 3), done);
    until(p.io, [&] { return sent == 3; });
    p.io.restart(); p.io.run_for(100ms);
    CHECK(p.atB.empty() && p.errors == 0 && p.b->snapshot().state == DataChannelTransportState::Connected);
    DataChannel::Callbacks callbacks; callbacks.onMessage = [&](auto m) { p.atB.push_back(std::move(m)); };
    p.remote->setCallbacks(callbacks);
    until(p.io, [&] { return p.atB.size() == 3; });
    CHECK(p.atB[0].data == UdpSocket::Bytes(64000, 1) && p.atB[1].data == UdpSocket::Bytes(64000, 2) && p.atB[2].data == UdpSocket::Bytes(4096, 3));
    p.close();
}
} // namespace
int main()
{
    try { codecs(); data(); negotiatedAndPause(); limits(); lossLargeDcepAndCancellation(); expiryAndBackpressure(); sendQueueAndClose(); simultaneousResetAndReuse(); receiveBackpressure(); std::cout << "PASS: DataChannel SCTP/DCEP\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
