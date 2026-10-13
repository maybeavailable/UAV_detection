#include"webrtc/ice_candidate.h"
#include"common/udp_socket.h"

#include<chrono>
#include<iostream>
#include<limits>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
namespace net = boost::asio;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

IceCandidate parsed(std::string_view text)
{
    IceCandidate candidate;
    std::string reason = "old";
    CHECK(IceCandidate::parse(text, candidate, &reason));
    CHECK(reason.empty() && candidate.validate());
    return candidate;
}
template<class Function>
void expectInvalid(Function function)
{
    bool threw = false;
    try { function(); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void sdpTests()
{
    const std::vector<std::string> fixtures{
        "candidate:1 1 UDP 2130706431 192.0.2.1 5000 typ host",
        "candidate:2 1 UDP 1694498815 192.0.2.3 45664 typ srflx raddr 203.0.113.141 rport 8998", // RFC 8839 §5.1
        "candidate:3 1 UDP 1862270975 198.51.100.10 6000 typ prflx raddr 192.0.2.1 rport 5000",
        "candidate:4 1 UDP 16777215 203.0.113.10 7000 typ relay raddr 198.51.100.10 rport 6000",
        "candidate:+/AB09 2 UDP 2130706430 2001:db8::1 65535 typ host",
        "candidate:6 1 TCP 1518280447 192.0.2.1 9 typ host tcptype active",
        "candidate:7 1 TCP 1518280447 192.0.2.1 5000 typ host tcptype passive",
        "candidate:8 1 TCP 1518280447 2001:db8::1 5001 typ host tcptype so",
        "candidate:9 1 UDP 1694498815 198.51.100.1 5000 typ srflx raddr 0.0.0.0 rport 9",
        "candidate:10 1 UDP 16777215 203.0.113.1 5000 typ relay raddr 0.0.0.0 rport 0",
        "candidate:11 1 UDP 2130706431 2c670da7-8c2a-4798-a2a1-ef986e215a4a.local 5000 typ host generation 0 ufrag test network-id 1 network-cost 10",
        "candidate:12 256 UDP 2147483647 media.example.com. 1 typ host x-unknown a/b:c+7=",
        "candidate:13 1 UDP 1 127.0.0.1 1 typ host custom first custom second"
    };
    for (const auto& text : fixtures)
    {
        const auto candidate = parsed(text);
        CHECK(candidate.toCandidateString() == text);
        CHECK(candidate.toSdp() == "a=" + text + "\r\n");
        const auto again = parsed(candidate.toSdp());
        CHECK(again.toCandidateString() == text);
    }
    const auto mixed = parsed(" \ta=candidate:1\t1 udp 2130706431 192.0.2.1 5000 TyP HOST\t\r\n");
    CHECK(mixed.toCandidateString() == fixtures[0]);
    const auto reordered = parsed("candidate:1 1 tcp 10 192.0.2.1 9 typ srflx generation 0 rport 5000 tcptype ACTIVE raddr 10.0.0.1");
    CHECK(reordered.toCandidateString() == "candidate:1 1 TCP 10 192.0.2.1 9 typ srflx raddr 10.0.0.1 rport 5000 tcptype active generation 0");
    const auto mdns = parsed(fixtures[10]);
    CHECK(mdns.isHostname() && mdns.isMdns() && !mdns.udpEndpoint());
    CHECK(mdns.extension("NETWORK-ID") == "1" && mdns.extension("ufrag") == "test");
    CHECK(!mdns.extension("missing"));
    const auto fqdn = parsed(fixtures[11]);
    CHECK(fqdn.isHostname() && !fqdn.isMdns() && !fqdn.udpEndpoint());
    auto name = mdns;
    name.address = "DEVICE.LOCAL.";
    CHECK(name.isMdns());
    CHECK(parsed(fixtures.back()).extension("custom") == "first");

    const auto ipv4 = parsed(fixtures[0]);
    CHECK(!ipv4.isHostname() && !ipv4.isMdns());
    CHECK(ipv4.udpEndpoint() == IceCandidate::Endpoint(net::ip::make_address("192.0.2.1"), 5000));
    const auto ipv6 = parsed(fixtures[4]);
    CHECK(ipv6.udpEndpoint() == IceCandidate::Endpoint(net::ip::make_address("2001:db8::1"), 65535));
    CHECK(!parsed(fixtures[5]).udpEndpoint());

    // 精确的大小上限，序列化到 SDP 后应仍能无损解析。
    auto maximum = ipv4;
    maximum.extensions.emplace_back("x", "x");
    const auto overhead = maximum.toSdp().size() - 1;
    maximum.extensions[0].second.assign(4096 - overhead, 'x');
    CHECK(maximum.validate() && maximum.toSdp().size() == 4096);
    CHECK(parsed(maximum.toSdp()).toSdp() == maximum.toSdp());
    maximum.extensions[0].second.push_back('x');
    CHECK(!maximum.validate());
    expectInvalid([&] { maximum.toSdp(); });
}

void validationTests()
{
    const std::vector<std::string> invalid{
        "", "a=end-of-candidates", "end-of-candidates", "candidate:",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ",
        "candidate:bad-name 1 UDP 1 192.0.2.1 5000 typ host",
        "candidate:1 0 UDP 1 192.0.2.1 5000 typ host",
        "candidate:1 257 UDP 1 192.0.2.1 5000 typ host",
        "candidate:1 +1 UDP 1 192.0.2.1 5000 typ host",
        "candidate:1 0001 UDP 1 192.0.2.1 5000 typ host",
        "candidate:1 1 QUIC 1 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP 0 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP -1 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP +1 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP 2147483648 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP 4294967296 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP 9999999999999999999999 192.0.2.1 5000 typ host",
        "candidate:1 1 UDP 1 999.1.1.1 5000 typ host",
        "candidate:1 1 UDP 1 2001:db8:::1 5000 typ host",
        "candidate:1 1 UDP 1 [2001:db8::1] 5000 typ host",
        "candidate:1 1 UDP 1 fe80::1%eth0 5000 typ host",
        "candidate:1 1 UDP 1 -bad.local 5000 typ host",
        "candidate:1 1 UDP 1 bad-.local 5000 typ host",
        "candidate:1 1 UDP 1 bad..local 5000 typ host",
        "candidate:1 1 UDP 1 bad.local.. 5000 typ host",
        "candidate:1 1 UDP 1 192.0.2.1 0 typ host",
        "candidate:1 1 UDP 1 192.0.2.1 65536 typ host",
        "candidate:1 1 UDP 1 192.0.2.1 -1 typ host",
        "candidate:1 1 UDP 1 192.0.2.1 65535abc typ host",
        "candidate:1 1 UDP 1 192.0.2.1 5000 type host",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ custom",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host lone",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host raddr 10.0.0.1 rport 5000",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ srflx",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ relay raddr 0.0.0.0",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ relay rport 9",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ relay raddr 999.1.1.1 rport 9",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ relay raddr 0.0.0.0 rport 65536",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ relay raddr 0.0.0.0 rport 9 RADDR 0.0.0.0",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ relay raddr 0.0.0.0 rport 0 RPORT 9",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host tcptype active",
        "candidate:1 1 TCP 1 192.0.2.1 9 typ host",
        "candidate:1 1 TCP 1 192.0.2.1 9 typ host tcptype invalid",
        "candidate:1 1 TCP 1 192.0.2.1 5000 typ host tcptype active",
        "candidate:1 1 TCP 1 192.0.2.1 9 typ host tcptype active TCPTYPE passive",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host typ host",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host bad/name value",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host\r\na=ice-lite",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host\n\n",
        "candidate:1 1 UDP 1 192.0.2.1 5000 typ host\r",
        std::string(4097, 'x'),
        "candidate:" + std::string(33, 'a') + " 1 UDP 1 192.0.2.1 5000 typ host"
    };
    auto sentinel = parsed("candidate:1 1 UDP 1 192.0.2.1 5000 typ host");
    const auto expected = sentinel.toSdp();
    for (const auto& text : invalid)
    {
        std::string reason;
        CHECK(!IceCandidate::parse(text, sentinel, &reason));
        CHECK(!reason.empty() && sentinel.toSdp() == expected);
    }
    auto nul = expected;
    nul.insert(nul.begin() + 20, '\0');
    CHECK(!IceCandidate::parse(nul, sentinel));
    IceCandidate empty;
    CHECK(!empty.validate() && !empty.udpEndpoint());
    expectInvalid([&] { empty.toSdp(); });
    auto unsafe = sentinel;
    unsafe.extensions.emplace_back("ufrag", "abc\r\na=ice-lite");
    CHECK(!unsafe.validate());
    expectInvalid([&] { unsafe.toCandidateString(); });
    unsafe = sentinel; unsafe.extensions.emplace_back("RADDR", "10.0.0.1");
    CHECK(!unsafe.validate());
    unsafe = sentinel; unsafe.transport = static_cast<IceCandidateTransport>(255);
    CHECK(!unsafe.validate());
    unsafe = sentinel; unsafe.type = static_cast<IceCandidateType>(255);
    CHECK(!unsafe.validate());
}

void priorityAndFactoryTests()
{
    CHECK(candidatePriority(IceCandidateType::Host) == 2130706431);
    CHECK(candidatePriority(IceCandidateType::Host, 65535, 2) == 2130706430);
    CHECK(candidatePriority(IceCandidateType::ServerReflexive) == 1694498815);
    CHECK(candidatePriority(IceCandidateType::PeerReflexive) == 1862270975);
    CHECK(candidatePriority(IceCandidateType::Relay) == 16777215);
    CHECK(candidatePriority(0, 0, 1) == 255);
    CHECK(candidatePriority(126, 65535, 256) == 2130706176);
    expectInvalid([] { candidatePriority(127, 0); });
    expectInvalid([] { candidatePriority(126, 65536); });
    expectInvalid([] { candidatePriority(126, 0, 0); });
    expectInvalid([] { candidatePriority(126, 0, 257); });
    expectInvalid([] { candidatePriority(0, 0, 256); });
    expectInvalid([] { defaultTypePreference(static_cast<IceCandidateType>(255)); });
    CHECK(candidatePairPriority(100, 200, true) == 429496730000ULL);
    CHECK(candidatePairPriority(100, 200, false) == 429496730001ULL);
    CHECK(candidatePairPriority(200, 100, true) == candidatePairPriority(100, 200, false));
    CHECK(candidatePairPriority(1, 1, true) == 4294967298ULL);
    CHECK(candidatePairPriority(0x7FFFFFFF, 0x7FFFFFFF, true) == 9223372036854775806ULL);
    expectInvalid([] { candidatePairPriority(0, 1, true); });
    expectInvalid([] { candidatePairPriority(1, 0, false); });
    expectInvalid([] { candidatePairPriority(0x80000000, 1, true); });

    const IceCandidate::Endpoint local(net::ip::make_address("192.0.2.1"), 5000);
    const IceCandidate::Endpoint mapped(net::ip::make_address("198.51.100.1"), 6000);
    const auto host = IceCandidate::host(local, "1");
    CHECK(host.toSdp() == "a=candidate:1 1 UDP 2130706431 192.0.2.1 5000 typ host\r\n");
    for (const auto type : {IceCandidateType::ServerReflexive, IceCandidateType::PeerReflexive, IceCandidateType::Relay})
    {
        const auto candidate = IceCandidate::fromUdpEndpoint(mapped, type, "2", 1, 65535, local);
        CHECK(candidate.validate() && candidate.udpEndpoint() == mapped);
        CHECK(candidate.relatedAddress == "192.0.2.1" && candidate.relatedPort == 5000);
    }
    expectInvalid([&] { IceCandidate::fromUdpEndpoint(mapped, IceCandidateType::Relay, "2"); });
    expectInvalid([&] { IceCandidate::fromUdpEndpoint(local, IceCandidateType::Host, "1", 1, 65535, mapped); });
    expectInvalid([&] { IceCandidate::host({net::ip::udp::v4(), 5000}, "1"); });
    expectInvalid([&] { IceCandidate::host({net::ip::udp::v6(), 5000}, "1"); });
    expectInvalid([&] { IceCandidate::host({net::ip::make_address("239.1.1.1"), 5000}, "1"); });
    expectInvalid([&] { IceCandidate::host({net::ip::make_address("255.255.255.255"), 5000}, "1"); });
    expectInvalid([&] { IceCandidate::host({local.address(), 0}, "1"); });
    expectInvalid([&] { IceCandidate::host(local, "bad-name"); });
    expectInvalid([&] { IceCandidate::host(local, "1", 65537); });
}

void udpIntegrationTest()
{
    net::io_context io;
    UdpSocket::Config config;
    config.localEndpoint = {net::ip::make_address("127.0.0.1"), 0};
    UdpSocket first(io, config), second(io, config);
    const auto local = IceCandidate::host(first.localEndpoint(), "1");
    const auto remote = parsed(local.toCandidateString());
    CHECK(remote.udpEndpoint());
    bool received = false, sent = false;
    first.startReceive([&](auto datagram) {
        CHECK(datagram.payload == UdpSocket::Bytes({0, 0xFF, 1}));
        CHECK(datagram.remoteEndpoint == second.localEndpoint());
        received = true;
    });
    second.asyncSendTo({0, 0xFF, 1}, *remote.udpEndpoint(), [&](auto ec, auto size) { CHECK(!ec && size == 3); sent = true; });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while ((!received || !sent) && std::chrono::steady_clock::now() < deadline)
    {
        io.restart(); io.run_for(2ms);
    }
    CHECK(received && sent);
    unsigned closed = 0;
    first.close([&](auto ec) { CHECK(!ec); ++closed; });
    second.close([&](auto ec) { CHECK(!ec); ++closed; });
    io.restart(); io.run();
    CHECK(closed == 2);
}

int main()
{
    try
    {
        sdpTests(); validationTests(); priorityAndFactoryTests(); udpIntegrationTest();
        std::cout << "PASS: ICE candidate SDP, validation, priorities, factories and UDP integration\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
