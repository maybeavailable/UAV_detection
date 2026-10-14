#include"webrtc/peerconnection.h"

#include<chrono>
#include<iostream>
#include<stdexcept>

using namespace webrtc;
namespace net = boost::asio;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)
namespace
{
template<class Predicate> void until(net::io_context& io, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
template<class Action> void perform(net::io_context& io, Action action)
{
    bool done = false; action([&](auto ec) { CHECK(!ec); done = true; }); until(io, [&] { return done; });
}
void peerConnection(bool ipv6)
{
    net::io_context io; std::unique_ptr<PeerConnection> a, b;
    std::shared_ptr<DataChannel> local, remote;
    std::vector<DataChannelMessage> received;
    unsigned errors = 0, remoteChannels = 0;
    PeerConnection::Config cfg;
    cfg.ice.ice.bindings = {{{net::ip::make_address(ipv6 ? "::1" : "127.0.0.1"), 0}, 1, 65535}};
    cfg.ice.ice.checkInterval = 5ms;
    PeerConnection::Callbacks ca, cb;
    ca.onIceCandidate = [&](auto candidate) { b->addIceCandidate(std::move(candidate), [](auto ec) { CHECK(!ec); }); };
    cb.onIceCandidate = [&](auto candidate) { a->addIceCandidate(std::move(candidate), [](auto ec) { CHECK(!ec); }); };
    ca.onError = cb.onError = [&](auto ec, auto message) { ++errors; std::cerr << ec.message() << ": " << message << '\n'; };
    cb.onDataChannel = [&](auto channel) {
        ++remoteChannels; remote = channel;
        DataChannel::Callbacks callbacks; callbacks.onMessage = [&](auto message) { received.push_back(std::move(message)); };
        channel->setCallbacks(callbacks);
    };
    a = std::make_unique<PeerConnection>(io, cfg, ca);
    cfg.enableDataChannels = true; cfg.dataChannel.maxMessageSize = 1024;
    b = std::make_unique<PeerConnection>(io, cfg, cb);
    DataChannelConfig options; options.label = "commands"; options.protocol = "json";
    a->createDataChannel(options, [&](auto ec, auto channel) { CHECK(!ec && channel); local = channel; });
    until(io, [&] { return bool(local); });
    auto negotiate = [&](bool restart) {
        std::optional<PeerSessionDescription> offer, answer;
        a->createOffer({restart}, [&](auto ec, auto d) { CHECK(!ec && d); offer = std::move(d); }); until(io, [&] { return bool(offer); });
        CHECK(offer->session.media.size() == 1 && offer->session.media[0].isSctp());
        perform(io, [&](auto done) { a->setLocalDescription(*offer, done); });
        perform(io, [&](auto done) { b->setRemoteDescription(SdpDescriptionType::Offer, offer->toSdp(), done); });
        b->createAnswer([&](auto ec, auto d) { CHECK(!ec && d); answer = std::move(d); }); until(io, [&] { return bool(answer); });
        perform(io, [&](auto done) { b->setLocalDescription(*answer, done); });
        perform(io, [&](auto done) { a->setRemoteDescription(SdpDescriptionType::Answer, answer->toSdp(), done); });
        until(io, [&] { return a->snapshot().connectionState == PeerConnectionState::Connected && b->snapshot().connectionState == PeerConnectionState::Connected &&
            a->snapshot().dataChannels && a->snapshot().dataChannels->state == DataChannelTransportState::Connected &&
            remote && local->snapshot().state == DataChannelState::Open && remote->snapshot().state == DataChannelState::Open; });
    };
    for (unsigned round = 0; round < 2; ++round)
    {
        negotiate(round != 0); CHECK(remoteChannels == 1 && errors == 0);
        CHECK(local->snapshot().maxMessageSize == 1024 && remote->snapshot().maxMessageSize == 1024);
        CHECK(a->snapshot().ice->generation == round);
        CHECK(*local->snapshot().config.id % 2 == (*a->snapshot().dtls->localSetup == SdpSetupRole::Active ? 0 : 1));
        bool sent = false; const auto before = received.size();
        local->sendText("hello", [&](auto ec, auto n) { CHECK(!ec && n == 5); sent = true; });
        until(io, [&] { return sent && received.size() == before + 1; });
        CHECK(!received.back().binary && received.back().data == UdpSocket::Bytes({'h','e','l','l','o'}));
    }
    bool oversized = false;
    local->sendBinary(UdpSocket::Bytes(1025), [&](auto ec, auto n) { CHECK(ec == net::error::message_size && n == 0); oversized = true; });
    until(io, [&] { return oversized; });
    std::shared_ptr<DataChannel> second;
    DataChannelConfig unreliable; unreliable.label = "telemetry"; unreliable.ordered = false; unreliable.maxRetransmits = 0;
    a->createDataChannel(unreliable, [&](auto ec, auto c) { CHECK(!ec); second = c; });
    until(io, [&] { return second && second->snapshot().state == DataChannelState::Open && remoteChannels == 2; });
    CHECK(remote->snapshot().config.maxRetransmits == 0 && !remote->snapshot().config.ordered);
    perform(io, [&](auto done) { second->close(done); });
    until(io, [&] { return second->snapshot().state == DataChannelState::Closed && remote->snapshot().state == DataChannelState::Closed; });
    unsigned closed = 0; a->close([&](auto ec) { CHECK(!ec); ++closed; }); b->close([&](auto ec) { CHECK(!ec); ++closed; });
    until(io, [&] { return closed == 2; });
    CHECK(local->snapshot().state == DataChannelState::Closed && a->snapshot().dataChannels->state == DataChannelTransportState::Closed && errors == 0);
}
} // namespace
int main()
{
    try {
        net::io_context io; PeerConnection::Config cfg; cfg.dataChannel.maxMessageSize = 0;
        bool rejected = false;
        try { PeerConnection peer(io, cfg); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        peerConnection(false); peerConnection(true); std::cout << "PASS: PeerConnection DataChannel DTLS/SCTP IPv4/IPv6/restart\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
