#include"webrtc/ice_agent.h"
#include"webrtc/sdp.h"

#include<boost/asio/executor_work_guard.hpp>
#include<boost/asio/post.hpp>

#include<atomic>
#include<chrono>
#include<iostream>
#include<stdexcept>
#include<thread>

using namespace webrtc;
using namespace std::chrono_literals;
namespace net = boost::asio;
using Type = StunAttributeType;
using Endpoint = UdpSocket::Endpoint;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

namespace
{
template<class Predicate>
void until(net::io_context& io, Predicate predicate, std::chrono::milliseconds timeout = 2500ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { io.restart(); io.run_for(2ms); }
    CHECK(predicate());
}
void runFor(net::io_context& io, std::chrono::milliseconds duration)
{
    io.restart(); io.run_for(duration);
}
IceAgent::Config options(StunIceRole role = StunIceRole::Controlling, const char* address = "127.0.0.1")
{
    IceAgent::Config config;
    config.role = role; config.checkInterval = 10ms;
    config.bindings = {{{net::ip::make_address(address), 0}, 1, 65535}};
    return config;
}
std::shared_ptr<UdpSocket> socket(net::io_context& io)
{
    UdpSocket::Config config;
    config.localEndpoint = {net::ip::make_address("127.0.0.1"), 0};
    return std::make_shared<UdpSocket>(io, config);
}
void close(net::io_context& io, IceAgent& a, IceAgent* b = nullptr, std::shared_ptr<UdpSocket> other = {})
{
    unsigned done = 0, expected = 1 + (b ? 1 : 0) + (other ? 1 : 0);
    auto complete = [&](auto ec) { CHECK(!ec); ++done; };
    a.close(complete); if (b) b->close(complete); if (other) other->close(complete);
    until(io, [&] { return done == expected; });
    CHECK(a.snapshot().state == IceAgentState::Closed);
    CHECK(a.snapshot().pendingTransactions == 0);
    runFor(io, 5ms);
}
void gather(net::io_context& io, IceAgent& a, IceAgent& b)
{
    unsigned done = 0;
    a.gather([&](auto ec) { CHECK(!ec); ++done; });
    b.gather([&](auto ec) { CHECK(!ec); ++done; });
    until(io, [&] { return done == 2 && a.snapshot().gatheringState == IceGatheringState::Complete &&
        b.snapshot().gatheringState == IceGatheringState::Complete; });
}
void exchange(IceAgent& a, IceAgent& b, bool sendToB = true)
{
    const auto first = a.snapshot(), second = b.snapshot();
    a.setRemoteCredentials(second.localCredentials);
    b.setRemoteCredentials(first.localCredentials);
    for (const auto& candidate : second.localCandidates) a.addRemoteCandidate(candidate);
    if (sendToB) for (const auto& candidate : first.localCandidates) b.addRemoteCandidate(candidate);
    a.endOfRemoteCandidates(second.localCredentials.ufrag);
    b.endOfRemoteCandidates(first.localCredentials.ufrag);
    a.start(); b.start();
}

void connection(StunIceRole firstRole, StunIceRole secondRole, bool ipv6 = false, bool multipleComponents = false)
{
    net::io_context io;
    auto firstOptions = options(firstRole, ipv6 ? "::1" : "127.0.0.1");
    auto secondOptions = options(secondRole, ipv6 ? "::1" : "127.0.0.1");
    firstOptions.tieBreaker = 100; secondOptions.tieBreaker = 200;
    if (multipleComponents)
    {
        firstOptions.components = secondOptions.components = {1, 2};
        firstOptions.bindings.push_back({{net::ip::make_address("127.0.0.1"), 0}, 2, 65535});
        secondOptions.bindings = firstOptions.bindings;
    }
    unsigned selectedA = 0, selectedB = 0, delivered = 0, sent = 0;
    IceAgent::Callbacks firstCallbacks, secondCallbacks;
    firstCallbacks.onSelectedPair = [&](auto pair) { CHECK(pair.valid && pair.nominated); ++selectedA; };
    secondCallbacks.onSelectedPair = [&](auto pair) { CHECK(pair.valid && pair.nominated); ++selectedB; };
    secondCallbacks.onData = [&](auto component, auto datagram) {
        CHECK(component == 1 || (multipleComponents && component == 2));
        CHECK(datagram.payload == UdpSocket::Bytes({22, 0xfe, 0xfd, 1, 2, 3})); ++delivered;
    };
    IceAgent a(io, firstOptions, firstCallbacks), b(io, secondOptions, secondCallbacks);
    gather(io, a, b); exchange(a, b);
    until(io, [&] { return a.snapshot().state == IceAgentState::Completed && b.snapshot().state == IceAgentState::Completed; });
    const auto first = a.snapshot(), second = b.snapshot();
    CHECK(first.role != second.role);
    CHECK(first.selectedPairs.size() == (multipleComponents ? 2u : 1u));
    CHECK(selectedA == first.selectedPairs.size() && selectedB == second.selectedPairs.size());
    for (auto component : firstOptions.components)
        a.asyncSend({22, 0xfe, 0xfd, 1, 2, 3}, component, [&](auto ec, auto size) { CHECK(!ec && size == 6); ++sent; });
    until(io, [&] { return delivered == firstOptions.components.size() && sent == delivered; });
    if (!ipv6)
    {
        auto intruder = socket(io);
        intruder->asyncSendTo({22, 0xfe, 0xfd, 1, 2, 3}, second.selectedPairs.front().localEndpoint);
        intruder->asyncSendTo({0, 1, 2}, second.selectedPairs.front().localEndpoint);
        runFor(io, 50ms);
        CHECK(delivered == firstOptions.components.size());
        intruder->close(); runFor(io, 5ms);
    }
    close(io, a, &b);
}

void trickleAndEarlyRequests()
{
    net::io_context io;
    IceAgent a(io, options()), b(io, options(StunIceRole::Controlled));
    gather(io, a, b);
    const auto first = a.snapshot(), second = b.snapshot();
    a.setRemoteCredentials(second.localCredentials); a.start();
    runFor(io, 80ms);
    CHECK(a.snapshot().state == IceAgentState::Checking); // 尚无候选，不提前失败
    a.addRemoteCandidate(second.localCandidates.front());
    until(io, [&] { return a.snapshot().state == IceAgentState::Completed; });
    CHECK(b.snapshot().remoteCandidates.empty()); // 在远端 SDP 前已经认证并响应请求
    b.setRemoteCredentials(first.localCredentials); b.start();
    until(io, [&] { return b.snapshot().state == IceAgentState::Completed; });
    CHECK(b.snapshot().remoteCandidates.front().type == IceCandidateType::PeerReflexive);
    CHECK(b.snapshot().selectedPairs.front().remoteEndpoint == first.localCandidates.front().udpEndpoint());
    close(io, a, &b);
}

void gatheringAndRetransmission()
{
    net::io_context io;
    auto server = socket(io);
    unsigned requests = 0;
    StunBytes firstRequest;
    const Endpoint mapped(net::ip::make_address("198.51.100.19"), 45678);
    server->startReceive([&](auto datagram) {
        StunPacket packet;
        CHECK(StunPacket::parse(datagram.payload, packet));
        ++requests;
        if (requests == 1) { firstRequest = datagram.payload; return; } // 丢弃首包
        CHECK(datagram.payload == firstRequest); // 重传使用相同 ID / 字节
        auto response = StunMessage::bindingSuccess(packet, mapped);
        server->asyncSendTo(response.encode({std::nullopt, {}, true}), datagram.remoteEndpoint);
    });
    auto config = options(); config.stunServers = {server->localEndpoint()};
    IceAgent agent(io, config);
    agent.gather();
    until(io, [&] { return agent.snapshot().gatheringState == IceGatheringState::Complete; });
    const auto snapshot = agent.snapshot();
    CHECK(requests == 2 && snapshot.localCandidates.size() == 2);
    const auto& reflexive = snapshot.localCandidates.back();
    CHECK(reflexive.type == IceCandidateType::ServerReflexive && reflexive.udpEndpoint() == mapped);
    CHECK(reflexive.relatedPort == snapshot.localCandidates.front().port && snapshot.pendingTransactions == 0);
    close(io, agent, nullptr, server);
}

// 使用独立的 STUN 对端检查认证、事务/来源绑定、regular nomination、487 和 consent。
void independentPeer()
{
    net::io_context io;
    auto peer = socket(io), wrongSource = socket(io);
    const auto peerCredentials = IceCredentials::generate();
    auto config = options(); config.tieBreaker = 111;
    unsigned nominations = 0, checks = 0, roleErrors = 0, data = 0, consent = 0;
    bool revoke = false;
    StunBytes retransmit;
    IceAgent agent(io, config);
    const auto ownCredentials = agent.snapshot().localCredentials;
    peer->startReceive([&](auto datagram) {
        if (!datagram.payload.empty() && datagram.payload.front() == 22) { ++data; return; }
        StunPacket packet;
        CHECK(StunPacket::parse(datagram.payload, packet));
        if (packet.messageClass() != StunClass::Request) return;
        CHECK(packet.validateIceBindingRequest(peerCredentials.ufrag, ownCredentials.ufrag, stunShortTermKey(peerCredentials.password)));
        if (checks == 0)
        {
            // 首次发送 487，必须改变角色和 tie-breaker，重新检查。
            CHECK(packet.find(Type::IceControlling));
            auto response = StunMessage::bindingError(packet, 487, "Role Conflict");
            peer->asyncSendTo(response.encode({StunIntegrity::Sha1, stunShortTermKey(peerCredentials.password), true}), datagram.remoteEndpoint);
            ++checks; ++roleErrors; return;
        }
        CHECK(packet.find(Type::IceControlled));
        CHECK(packet.find(Type::IceControlled)->asUint64() != 111);
        ++checks;
        if (checks == 2)
        {
            retransmit = datagram.payload;
            // 合法 HMAC 但从错误源端口发送；原事务应继续等待并重传。
            auto response = StunMessage::bindingSuccess(packet, datagram.remoteEndpoint);
            wrongSource->asyncSendTo(response.encode({StunIntegrity::Sha1, stunShortTermKey(peerCredentials.password), true}), datagram.remoteEndpoint);
            return;
        }
        if (checks == 3) CHECK(datagram.payload == retransmit);
        const auto signedResponse = [&](StunMessage response) {
            peer->asyncSendTo(response.encode({StunIntegrity::Sha1, stunShortTermKey(peerCredentials.password), true}), datagram.remoteEndpoint);
        };
        if (revoke) { ++consent; signedResponse(StunMessage::bindingError(packet, 403, "Forbidden")); return; }
        signedResponse(StunMessage::bindingSuccess(packet, datagram.remoteEndpoint));
        if (nominations == 0)
        {
            ++nominations;
            auto nomination = StunMessage::iceBindingRequest(peerCredentials.ufrag, ownCredentials.ufrag,
                candidatePriority(IceCandidateType::PeerReflexive), StunIceRole::Controlling, 999999, true);
            peer->asyncSendTo(nomination.encode({StunIntegrity::Sha1, stunShortTermKey(ownCredentials.password), true}), datagram.remoteEndpoint);
        }
    });
    agent.gather();
    until(io, [&] { return !agent.snapshot().localCandidates.empty(); });
    agent.setRemoteCredentials(peerCredentials);
    agent.addRemoteCandidate(IceCandidate::host(peer->localEndpoint(), "peer"));
    agent.endOfRemoteCandidates(); agent.start();
    until(io, [&] { return agent.snapshot().state == IceAgentState::Completed; });
    CHECK(roleErrors == 1 && checks >= 3 && nominations == 1 && agent.snapshot().role == StunIceRole::Controlled);
    agent.asyncSend({22, 1, 2}); until(io, [&] { return data == 1; });
    revoke = true;
    until(io, [&] { return agent.snapshot().state == IceAgentState::Failed; }, 7000ms);
    CHECK(consent == 1 && agent.snapshot().selectedPairs.empty());
    bool blocked = false;
    agent.asyncSend({22}, 1, [&](auto ec, auto size) { CHECK(ec && size == 0); blocked = true; });
    until(io, [&] { return blocked; });
    wrongSource->close(); close(io, agent, nullptr, peer);
}

void restartAndValidation()
{
    net::io_context io;
    auto bad = options(); bad.initialRto = 1ms;
    bool threw = false;
    try { IceAgent agent(io, bad); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    CHECK(IceCredentials::generate().validate());
    CHECK((!IceCredentials{"bad", "short"}.validate()));
    IceAgent a(io, options()), b(io, options(StunIceRole::Controlled));
    gather(io, a, b); exchange(a, b);
    until(io, [&] { return a.snapshot().state == IceAgentState::Completed && b.snapshot().state == IceAgentState::Completed; });
    const auto oldA = a.snapshot(), oldB = b.snapshot();
    unsigned restarted = 0;
    a.restart({}, StunIceRole::Controlling, [&](auto ec) { CHECK(!ec); ++restarted; });
    b.restart({}, StunIceRole::Controlled, [&](auto ec) { CHECK(!ec); ++restarted; });
    until(io, [&] { return restarted == 2; });
    CHECK(a.snapshot().state == IceAgentState::New && a.snapshot().selectedPairs.empty());
    CHECK(a.snapshot().localCredentials.ufrag != oldA.localCredentials.ufrag);
    CHECK(a.snapshot().localCandidates.front().udpEndpoint() == oldA.localCandidates.front().udpEndpoint());
    CHECK(a.snapshot().remoteCandidates.empty());
    unsigned rejected = 0;
    a.setRemoteCredentials(b.snapshot().localCredentials);
    a.addRemoteCandidate(oldB.localCandidates.front(), [&](auto ec) { CHECK(ec); ++rejected; }, {}, oldB.localCredentials.ufrag);
    a.asyncSend({22}, 1, [&](auto ec, auto size) { CHECK(ec == net::error::not_connected && size == 0); ++rejected; });
    until(io, [&] { return rejected == 2; });
    exchange(a, b);
    until(io, [&] { return a.snapshot().state == IceAgentState::Completed && b.snapshot().state == IceAgentState::Completed; });
    close(io, a, &b);

    IceAgent empty(io, options());
    empty.gather(); empty.setRemoteCredentials(IceCredentials::generate()); empty.endOfRemoteCandidates(); empty.start();
    until(io, [&] { return empty.snapshot().state == IceAgentState::Failed; });
    close(io, empty);
}

void litePeerAndConsentExpiry()
{
    net::io_context io;
    auto peer = socket(io);
    auto config = options(StunIceRole::Controlled);
    config.consentTimeout = 10000ms; config.disconnectedTimeout = 5000ms;
    const auto credentials = IceCredentials::generate();
    unsigned checks = 0, nominations = 0, consent = 0;
    StunBytes previousConsent;
    std::optional<StunPacket> late;
    Endpoint destination;
    IceAgent agent(io, config);
    const auto local = agent.snapshot().localCredentials;
    peer->startReceive([&](auto datagram) {
        StunPacket packet;
        CHECK(StunPacket::parse(datagram.payload, packet));
        CHECK(packet.validateIceBindingRequest(credentials.ufrag, local.ufrag, stunShortTermKey(credentials.password)));
        CHECK(packet.find(Type::IceControlling)); // 远端 lite 使本地自动转为 controlling
        destination = datagram.remoteEndpoint;
        if (nominations)
        {
            CHECK(!packet.find(Type::UseCandidate));
            CHECK(datagram.payload != previousConsent); // consent 每次使用新 ID，不重传
            previousConsent = datagram.payload; late = packet; ++consent;
            return; // 保持静默，验证失联与过期
        }
        if (packet.find(Type::UseCandidate)) ++nominations;
        else
        {
            ++checks;
            if (checks == 1)
            {
                // 错误 HMAC 不得将候选对变为 valid；等待原请求重传。
                const auto wrong = StunMessage::bindingSuccess(packet, datagram.remoteEndpoint);
                peer->asyncSendTo(wrong.encode({StunIntegrity::Sha1, stunShortTermKey("wrong-password"), true}), datagram.remoteEndpoint);
                return;
            }
        }
        auto response = StunMessage::bindingSuccess(packet, datagram.remoteEndpoint);
        peer->asyncSendTo(response.encode({StunIntegrity::Sha1, stunShortTermKey(credentials.password), true}), datagram.remoteEndpoint);
    });
    agent.gather(); until(io, [&] { return !agent.snapshot().localCandidates.empty(); });
    agent.setRemoteCredentials(credentials, true);
    agent.addRemoteCandidate(IceCandidate::host(peer->localEndpoint(), "lite"));
    agent.endOfRemoteCandidates(); agent.start();
    until(io, [&] { return agent.snapshot().state == IceAgentState::Completed; });
    CHECK(checks == 2 && nominations == 1 && agent.snapshot().role == StunIceRole::Controlling);
    until(io, [&] { return agent.snapshot().state == IceAgentState::Disconnected; }, 6000ms);
    until(io, [&] { return agent.snapshot().state == IceAgentState::Failed; }, 6000ms);
    CHECK(consent >= 1 && late && agent.snapshot().selectedPairs.empty());
    auto response = StunMessage::bindingSuccess(*late, destination);
    peer->asyncSendTo(response.encode({StunIntegrity::Sha1, stunShortTermKey(credentials.password), true}), destination);
    runFor(io, 30ms);
    CHECK(agent.snapshot().state == IceAgentState::Failed); // 迟到响应不能恢复已过期 consent
    close(io, agent, nullptr, peer);
}

void descriptionAndExternalSocket()
{
    net::io_context io;
    auto transport = socket(io);
    IceAgent a(io, options()), b(io, options(StunIceRole::Controlled));
    a.addLocalTransport(transport);
    b.gather();
    until(io, [&] { return a.snapshot().localCandidates.size() == 1 && b.snapshot().localCandidates.size() == 1; });
    const auto remote = b.snapshot();
    const std::string description = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE data\r\na=ice-ufrag:" + remote.localCredentials.ufrag + "\r\na=ice-pwd:" + remote.localCredentials.password +
        "\r\na=fingerprint:sha-256 00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F\r\n"
        "a=setup:actpass\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\na=mid:data\r\na=sctp-port:5000\r\n" +
        remote.localCandidates.front().toSdp() + "a=end-of-candidates\r\n";
    SdpSession session;
    CHECK(SdpSession::parse(description, session) && session.validateWebRtc(SdpDescriptionType::Offer));
    bool applied = false;
    a.setRemoteDescription(session, "data", SdpDescriptionType::Offer, [&](auto ec) { CHECK(!ec); applied = true; });
    b.setRemoteCredentials(a.snapshot().localCredentials);
    // mDNS 也可由上层解析后提交具体端点；测试不依赖机器是否安装 Avahi。
    auto candidate = a.snapshot().localCandidates.front(); candidate.address = "test-peer.local";
    b.addRemoteCandidate(candidate, [&](auto ec) { CHECK(!ec); }, transport->localEndpoint());
    a.start(); b.start();
    until(io, [&] { return applied && a.snapshot().state == IceAgentState::Completed && b.snapshot().state == IceAgentState::Completed; });
    close(io, a, &b);
    CHECK(!transport->isOpen());

    IceAgent resolving(io, options());
    auto unresolved = IceCandidate::host({net::ip::make_address("127.0.0.1"), 9999}, "dns");
    unresolved.address = "ice-test.invalid";
    bool canceled = false;
    resolving.addRemoteCandidate(unresolved, [&](auto ec) { CHECK(ec == net::error::operation_aborted); canceled = true; });
    close(io, resolving);
    CHECK(canceled);
}

void concurrentLifecycle()
{
    net::io_context io;
    auto work = net::make_work_guard(io);
    IceAgent a(io, options());
    std::atomic<unsigned> callbacks{0};
    std::thread first([&] { io.run(); }), second([&] { io.run(); });
    a.gather([&](auto ec) { CHECK(!ec); ++callbacks; });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (a.snapshot().localCandidates.empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    a.close([&](auto ec) { CHECK(!ec); ++callbacks; });
    a.close([&](auto ec) { CHECK(!ec); ++callbacks; });
    work.reset(); first.join(); second.join();
    CHECK(callbacks == 3 && a.snapshot().state == IceAgentState::Closed);
}
} // namespace

int main()
{
    try
    {
        connection(StunIceRole::Controlling, StunIceRole::Controlled);
        connection(StunIceRole::Controlling, StunIceRole::Controlling);
        connection(StunIceRole::Controlled, StunIceRole::Controlled);
        connection(StunIceRole::Controlling, StunIceRole::Controlled, true);
        connection(StunIceRole::Controlling, StunIceRole::Controlled, false, true);
        trickleAndEarlyRequests(); gatheringAndRetransmission(); independentPeer();
        restartAndValidation(); litePeerAndConsentExpiry(); descriptionAndExternalSocket(); concurrentLifecycle();
        std::cout << "PASS: ICE UDP checks, nomination, roles, trickle, STUN gathering, consent, restart and lifecycle\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
