#include"webrtc/stun_protocol.h"
#include"webrtc/ice_candidate.h"
#include"common/udp_socket.h"

#include<openssl/evp.h>
#include<openssl/hmac.h>

#include<algorithm>
#include<chrono>
#include<iostream>
#include<stdexcept>

using namespace webrtc;
using namespace std::chrono_literals;
namespace net = boost::asio;
using Type = StunAttributeType;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

namespace
{
StunBytes hex(std::string_view input)
{
    StunBytes result;
    int high = -1;
    for (const char ch : input)
    {
        if (ch == ' ' || ch == '\n' || ch == '\t') continue;
        const int value = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        CHECK(value >= 0);
        if (high < 0) high = value;
        else { result.push_back(static_cast<std::uint8_t>(high * 16 + value)); high = -1; }
    }
    CHECK(high == -1);
    return result;
}
StunPacket parsed(const StunBytes& wire)
{
    StunPacket result;
    std::string reason = "old";
    CHECK(StunPacket::looksLikeStun(wire.data(), wire.size()));
    if (!StunPacket::parse(wire, result, &reason)) throw std::runtime_error(reason);
    CHECK(reason.empty() && result.wire() == wire);
    return result;
}
void rejected(const StunBytes& wire)
{
    auto previous = parsed(StunMessage::bindingRequest().encode());
    const auto saved = previous.wire();
    std::string reason;
    CHECK(!StunPacket::parse(wire, previous, &reason));
    CHECK(!reason.empty() && previous.wire() == saved);
}
template<class Function>
void invalid(Function function)
{
    bool threw = false;
    try { function(); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
void setLength(StunBytes& wire)
{
    const auto size = wire.size() - 20;
    wire[2] = static_cast<std::uint8_t>(size >> 8);
    wire[3] = static_cast<std::uint8_t>(size);
}
void refreshFingerprint(StunBytes& wire)
{
    std::uint32_t crc = 0xffffffff;
    for (std::size_t i = 0; i < wire.size() - 8; ++i)
    {
        crc ^= wire[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    crc = ~crc ^ 0x5354554e;
    for (unsigned i = 0; i < 4; ++i) wire[wire.size() - 4 + i] = static_cast<std::uint8_t>(crc >> (24 - i * 8));
}
const StunBytes sampleRequest = hex(
    "000100582112a442b7e7a701bc34d686fa87dfae"
    "802200105354554e207465737420636c69656e74"
    "002400046e0001ff80290008932ff9b151263b36"
    "000600096576746a3a68367659202020"
    "000800149aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2"
    "80280004e57a3bcf");
const auto sampleKey = stunShortTermKey("VOkJxbRl1RmTxUk/WvJxBt");

// 独立 RFC 5769 golden bytes，包含非零 padding，避免只验证自身编解码的对称性。
void officialVectors()
{
    const auto request = parsed(sampleRequest);
    CHECK(request.method() == StunMethod::Binding && request.messageClass() == StunClass::Request);
    CHECK(request.verifyFingerprint() && request.verifyIntegrity(sampleKey));
    CHECK(request.validateIceBindingRequest("evtj", "h6vY", sampleKey));
    CHECK(request.find(Type::Username)->asText() == "evtj:h6vY");
    CHECK(request.find(Type::Priority)->asUint32() == 0x6e0001ff);
    CHECK(request.find(Type::IceControlled)->asUint64() == 0x932ff9b151263b36ULL);
    CHECK(request.preferredIntegrity() == StunIntegrity::Sha1);
    CHECK(!request.mappedAddress() && request.unknownRequiredAttributes().empty());
    CHECK(!request.verifyIntegrity(stunShortTermKey("wrong")));
    CHECK(!request.verifyIntegrity(sampleKey, StunIntegrity::Sha256));
    CHECK(!request.validateIceBindingRequest("h6vY", "evtj", sampleKey));

    const auto v4 = parsed(hex(
        "0101003c2112a442b7e7a701bc34d686fa87dfae"
        "8022000b7465737420766563746f7220"
        "002000080001a147e112a643"
        "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"
        "80280004c07d4c96"));
    CHECK(v4.messageClass() == StunClass::SuccessResponse);
    CHECK(v4.verifyIntegrity(sampleKey) && v4.verifyFingerprint());
    CHECK(v4.mappedAddress() == StunEndpoint(net::ip::make_address("192.0.2.1"), 32853));
    const auto v6 = parsed(hex(
        "010100482112a442b7e7a701bc34d686fa87dfae"
        "8022000b7465737420766563746f7220"
        "002000140002a1470113a9faa5d3f179bc25f4b5bed2b9d9"
        "00080014a382954e4be67bf11784c97c8292c275bfe3ed41"
        "80280004c8fb0b4c"));
    CHECK(v6.verifyIntegrity(sampleKey) && v6.verifyFingerprint());
    CHECK(v6.mappedAddress() == StunEndpoint(net::ip::make_address("2001:db8:1234:5678:11:2233:4455:6677"), 32853));

    const auto longTerm = parsed(hex(
        "000100602112a44278ad3433c6ad72c029da412e"
        "00060012e3839ee38388e383aae38383e382afe382b90000"
        "0015001c662f2f3439396b39353464364f4c33346f4c39465354767936347341"
        "0014000b6578616d706c652e6f726700"
        "00080014f67024656dd64a3e02b8e0712e85c9a28ca89666"));
    const auto key = stunLongTermKey(u8"マトリックス", "example.org", "TheMatrIX");
    CHECK(key.size() == 16 && longTerm.verifyIntegrity(key));
    CHECK(!longTerm.verifyFingerprint());
    StunMessage model;
    model.transactionId = longTerm.transactionId();
    model.attributes = {StunAttribute::text(Type::Username, u8"マトリックス"),
                        StunAttribute::text(Type::Nonce, "f//499k954d6OL34oL9FSTvy64sA"),
                        StunAttribute::text(Type::Realm, "example.org")};
    CHECK(model.encode({StunIntegrity::Sha1, key, false}) == longTerm.wire());

    // RFC 8489 B.1，采用已确认的 erratum 6268：长度 0x90 和 PASSWORD-ALGORITHM。
    // 显式传入向量规定的已处理密码；库本身不实现 Unicode profile。
    const auto sha256 = parsed(hex(
        "000100902112a44278ad3433c6ad72c029da412e"
        "001e00204a3cf38fef6992bda952c6780417da0f24819415569e60b205c46e41407f1704"
        "001500296f624d61744a6f733241414143662f2f3439396b39353464364f4c33346f4c39465354767936347341000000"
        "0014000b6578616d706c652e6f726700"
        "001d000400020000"
        "001c0020b5c7bf005b6c52a21c51c5e892f81924136296cb927c43149309278cc6518e65"));
    const auto sha256Key = stunLongTermKey(u8"マトリックス", "example.org", "TheMatrIX", StunPasswordAlgorithm::Sha256);
    CHECK(sha256Key.size() == 32 && sha256.verifyIntegrity(sha256Key, StunIntegrity::Sha256));
    CHECK(!sha256.verifyIntegrity(key, StunIntegrity::Sha256));
    CHECK(sha256.find(Type::Userhash)->value == stunUserhash(u8"マトリックス", "example.org"));
    CHECK(sha256.find(Type::PasswordAlgorithm)->asPasswordAlgorithm() == StunPasswordAlgorithm::Sha256);
    CHECK(sha256.unknownRequiredAttributes().empty());
    model.attributes = {{Type::Userhash, stunUserhash(u8"マトリックス", "example.org")},
                        StunAttribute::text(Type::Nonce, "obMatJos2AAACf//499k954d6OL34oL9FSTvy64sA"),
                        StunAttribute::text(Type::Realm, "example.org"),
                        StunAttribute::passwordAlgorithm(StunPasswordAlgorithm::Sha256)};
    CHECK(model.encode({StunIntegrity::Sha256, sha256Key, false}) == sha256.wire());
    model.attributes = {StunAttribute::passwordAlgorithms({StunPasswordAlgorithm::Sha256, StunPasswordAlgorithm::Md5})};
    CHECK(parsed(model.encode()).find(Type::PasswordAlgorithms)->value == hex("0002000000010000"));
}

void typesAttributesAndBoundaries()
{
    // 穷举 12 位 method / 两位 class 的交错位映射，覆盖扩展 method。
    for (unsigned method = 1; method <= 0xfff; ++method)
        for (unsigned cls = 0; cls < 4; ++cls)
        {
            StunMessage model;
            model.method = static_cast<StunMethod>(method);
            model.messageClass = static_cast<StunClass>(cls);
            const auto packet = parsed(model.encode());
            CHECK(packet.method() == model.method && packet.messageClass() == model.messageClass);
        }
    auto model = StunMessage::bindingRequest("UAV_detection");
    CHECK(model.transactionId != StunMessage::bindingRequest().transactionId);
    for (const auto& address : {"0.0.0.0", "192.0.2.1", "::", "2001:db8::ffff"})
        for (const auto port : {0, 65535})
            for (const auto type : {Type::MappedAddress, Type::XorMappedAddress, Type::AlternateServer})
            {
                const StunEndpoint endpoint(net::ip::make_address(address), static_cast<std::uint16_t>(port));
                const auto attribute = StunAttribute::address(type, endpoint, model.transactionId);
                CHECK(attribute.asAddress(model.transactionId) == endpoint);
            }
    model.attributes.push_back(StunAttribute::text(Type::Software, "duplicate"));
    model.attributes.push_back({static_cast<Type>(0x777), {1, 2, 3}});
    model.attributes.push_back({static_cast<Type>(0x666), {}});
    model.attributes.push_back({static_cast<Type>(0x777), {4}});
    model.attributes.push_back({static_cast<Type>(0xf123), {5}});
    const auto packet = parsed(model.encode({std::nullopt, {}, true}));
    CHECK(packet.find(Type::Software)->asText() == "UAV_detection");
    CHECK(packet.unknownRequiredAttributes() == std::vector<std::uint16_t>({0x666, 0x777}));
    CHECK(packet.verifyFingerprint() && !packet.preferredIntegrity());
    const auto error = parsed(StunMessage::bindingError(packet, 420, "Unknown Attribute", packet.unknownRequiredAttributes()).encode());
    CHECK(error.isResponseTo(model));
    CHECK(error.find(Type::ErrorCode)->asError()->code == 420);
    CHECK(error.find(Type::UnknownAttributes)->asUnknownAttributes() == packet.unknownRequiredAttributes());
    CHECK(!error.verifyIntegrity(sampleKey));
    for (const auto code : {300, 400, 401, 487, 699})
        CHECK(parsed(StunMessage::bindingError(packet, code, "error").encode()).find(Type::ErrorCode)->asError()->code == code);
    auto request = StunMessage::bindingRequest();
    const StunEndpoint endpoint(net::ip::make_address("127.0.0.1"), 9000);
    const auto response = parsed(StunMessage::bindingSuccess(parsed(request.encode()), endpoint).encode());
    CHECK(response.isResponseTo(request) && response.mappedAddress() == endpoint);
    request.transactionId[11] ^= 1;
    CHECK(!response.isResponseTo(request));
    request.messageClass = StunClass::Indication;
    CHECK(!response.isResponseTo(request));
    CHECK(parsed(StunMessage::bindingIndication().encode()).messageClass() == StunClass::Indication);
    StunMessage futureAddress = StunMessage::bindingRequest();
    futureAddress.attributes = {StunAttribute::address(Type::MappedAddress, endpoint, futureAddress.transactionId),
                               StunAttribute::address(Type::XorMappedAddress, endpoint, futureAddress.transactionId)};
    auto futureWire = futureAddress.encode();
    futureWire[37] = 3; // XOR-MAPPED-ADDRESS 的未知地址族应被忽略，回退到 MAPPED-ADDRESS
    CHECK(parsed(futureWire).mappedAddress() == endpoint);
    const StunAttribute unknownFamily{Type::XorMappedAddress, {0, 3, 0, 1, 1, 2, 3, 4}};
    CHECK(!unknownFamily.asAddress({}));
    futureAddress.attributes = {unknownFamily};
    invalid([&] { futureAddress.encode(); });

    // TLV 的有效值长度不含 padding；65532 为头部长度字段可表示的最大四字节倍数。
    request = StunMessage::bindingRequest();
    request.attributes = {{static_cast<Type>(0xaaaa), StunBytes(65528, 0x42)}};
    CHECK(parsed(request.encode()).wire().size() == StunMaxMessageSize);
    request.attributes[0].value.push_back(0);
    invalid([&] { request.encode(); });
    request.attributes[0].value.resize(65536);
    invalid([&] { request.encode(); });
    invalid([] { StunAttribute::text(Type::Username, std::string(509, 'a')); });
    invalid([] { StunAttribute::text(Type::Software, std::string(128, 'a')); });
    invalid([] { StunAttribute::text(Type::Username, "\xc0\x80"); });
    invalid([] { StunAttribute::error(200, "bad"); });
    invalid([] { StunAttribute::error(700, "bad"); });
    invalid([&] { StunMessage::bindingError(packet, 420, "missing list"); });
    invalid([&] { StunMessage::bindingError(packet, 400, "bad list", {0x666}); });
    invalid([] { StunAttribute::unknownAttributes({0x8000}); });
    invalid([&] { StunMessage::bindingSuccess(response, endpoint); });
    invalid([] { StunMessage::bindingRequest().encode({StunIntegrity::Sha1, {}, true}); });
    invalid([] { StunMessage::bindingRequest().encode({std::nullopt, sampleKey, true}); });
    request = StunMessage::bindingRequest(); request.method = static_cast<StunMethod>(0x1000);
    invalid([&] { request.encode(); });
    request.method = static_cast<StunMethod>(0);
    invalid([&] { request.encode(); });
    request.method = StunMethod::Binding; request.messageClass = static_cast<StunClass>(4);
    invalid([&] { request.encode(); });
    request.messageClass = StunClass::Request; request.attributes = {{Type::MessageIntegrity, StunBytes(20)}};
    invalid([&] { request.encode(); });
    CHECK(!StunAttribute::flag(Type::UseCandidate).asUint32());
    CHECK(!StunAttribute::uint32(Type::Priority, 1).asAddress({}));
    CHECK(!StunPacket{}.verifyFingerprint() && !StunPacket{}.verifyIntegrity(sampleKey));
    auto source = parsed(StunMessage::bindingRequest().encode({StunIntegrity::Sha1, sampleKey, true}));
    const auto moved = std::move(source);
    CHECK(moved.verifyIntegrity(sampleKey) && moved.verifyFingerprint());
    CHECK(!source.verifyIntegrity(sampleKey) && !source.verifyFingerprint());
}

void integrityAndMalformedPackets()
{
    auto model = StunMessage::iceBindingRequest("locl", "remt", 0x6e0001ff, StunIceRole::Controlling, 0xffffffffffffffff, true);
    for (const auto algorithm : {StunIntegrity::Sha1, StunIntegrity::Sha256, StunIntegrity::Sha1AndSha256})
        for (const bool fp : {false, true})
        {
            const auto wire = model.encode({algorithm, sampleKey, fp});
            const auto packet = parsed(wire);
            CHECK(packet.verifyIntegrity(sampleKey, algorithm));
            CHECK(packet.verifyFingerprint() == fp);
            CHECK(packet.validateIceBindingRequest("remt", "locl", sampleKey, nullptr, algorithm) == fp);
            CHECK(!packet.verifyIntegrity({}, algorithm));
            CHECK(packet.find(Type::IceControlling)->asUint64() == 0xffffffffffffffffULL);
        }
    invalid([] { StunMessage::iceBindingRequest("locl", "remt", 1, StunIceRole::Controlled, 1, true); });
    invalid([] { StunMessage::iceBindingRequest("locl", "remt", 0, StunIceRole::Controlling, 1); });
    invalid([] { StunMessage::iceBindingRequest("locl", "remt", 0x80000000, StunIceRole::Controlling, 1); });
    invalid([] { StunMessage::iceBindingRequest("bad:", "remt", 1, StunIceRole::Controlling, 1); });
    const auto signedWire = model.encode({StunIntegrity::Sha1, sampleKey, true});
    for (std::size_t i = 0; i < signedWire.size(); ++i)
    {
        auto corrupt = signedWire; corrupt[i] ^= 1;
        StunPacket packet;
        if (StunPacket::parse(corrupt, packet))
            CHECK(!packet.validateIceBindingRequest("remt", "locl", sampleKey));
    }
    // 攻击者可重算 CRC，但不能重新计算 HMAC。
    auto corrupt = signedWire; corrupt[28] ^= 1;
    refreshFingerprint(corrupt);
    const auto badHmac = parsed(corrupt);
    CHECK(badHmac.verifyFingerprint() && !badHmac.verifyIntegrity(sampleKey));
    for (std::size_t size = 0; size < sampleRequest.size(); ++size)
        rejected(StunBytes(sampleRequest.begin(), sampleRequest.begin() + size));
    corrupt = sampleRequest; corrupt.push_back(0); rejected(corrupt);
    corrupt = sampleRequest; corrupt[0] |= 0x40; rejected(corrupt);
    corrupt = sampleRequest; corrupt[4] ^= 1; rejected(corrupt);
    corrupt = sampleRequest; corrupt[3] |= 1; rejected(corrupt);
    corrupt = sampleRequest; corrupt[22] = 0xff; rejected(corrupt);
    corrupt = sampleRequest; corrupt[23] = 0xff; rejected(corrupt);
    corrupt = sampleRequest; corrupt[79] = 19; rejected(corrupt); // MI 必须 20 字节
    corrupt = sampleRequest; corrupt.insert(corrupt.end(), {0x80, 0x22, 0, 0}); setLength(corrupt); rejected(corrupt);
    CHECK(!StunPacket::looksLikeStun(nullptr, 20));
    StunPacket empty;
    CHECK(!StunPacket::parse(nullptr, 20, empty));

    // 完整性之后插入提名/优先级/未知必选属性，不让未经保护的数据进入 ICE 模型。
    auto noNomination = StunMessage::iceBindingRequest("locl", "remt", 1, StunIceRole::Controlled, 0);
    auto suffix = noNomination.encode({StunIntegrity::Sha1, sampleKey, false});
    const auto injection = hex("002500000024000400000002077700008028000400000000");
    suffix.insert(suffix.end(), injection.begin(), injection.end());
    setLength(suffix); refreshFingerprint(suffix);
    const auto ignored = parsed(suffix);
    CHECK(ignored.verifyFingerprint() && ignored.verifyIntegrity(sampleKey));
    CHECK(!ignored.find(Type::UseCandidate) && ignored.find(Type::Priority)->asUint32() == 1);
    CHECK(ignored.unknownRequiredAttributes().empty());
    CHECK(ignored.validateIceBindingRequest("remt", "locl", sampleKey));
    for (const auto role : {Type::IceControlling, Type::IceControlled})
    {
        auto invalidRole = noNomination;
        if (role == Type::IceControlling) invalidRole.attributes.push_back(StunAttribute::uint64(role, 1));
        else invalidRole.attributes.erase(invalidRole.attributes.begin() + 2);
        CHECK(!parsed(invalidRole.encode({StunIntegrity::Sha1, sampleKey, true})).validateIceBindingRequest("remt", "locl", sampleKey));
    }
    noNomination.attributes.push_back(StunAttribute::flag(Type::UseCandidate));
    CHECK(!parsed(noNomination.encode({StunIntegrity::Sha1, sampleKey, true})).validateIceBindingRequest("remt", "locl", sampleKey));

    auto duplicate = model.encode({StunIntegrity::Sha1, sampleKey, false});
    const StunBytes extraIntegrity(duplicate.end() - 24, duplicate.end());
    duplicate.insert(duplicate.end(), extraIntegrity.begin(), extraIntegrity.end());
    setLength(duplicate); rejected(duplicate);
    auto reversed = model.encode({StunIntegrity::Sha256, sampleKey, false});
    const auto sha1 = hex("000800140000000000000000000000000000000000000000");
    reversed.insert(reversed.end(), sha1.begin(), sha1.end());
    setLength(reversed); rejected(reversed);

    // 显式 STUN usage 才可接收截短 HMAC；手工构造独立输入，长度调整到截短属性末尾。
    auto truncated = StunMessage::bindingRequest().encode();
    truncated.insert(truncated.end(), {0, 0x1c, 0, 16});
    truncated.resize(40, 0); setLength(truncated);
    unsigned digestSize = 0;
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    CHECK(HMAC(EVP_sha256(), sampleKey.data(), static_cast<int>(sampleKey.size()), truncated.data(), 20, digest, &digestSize));
    std::copy_n(digest, 16, truncated.begin() + 24);
    const auto shortened = parsed(truncated);
    CHECK(!shortened.verifyIntegrity(sampleKey, StunIntegrity::Sha256));
    CHECK(shortened.verifyIntegrity(sampleKey, StunIntegrity::Sha256, nullptr, true));
}

void udpBindingRoundtrip()
{
    net::io_context io;
    UdpSocket::Config config;
    config.localEndpoint = {net::ip::make_address("127.0.0.1"), 0};
    UdpSocket client(io, config), server(io, config);
    const auto request = StunMessage::iceBindingRequest("locl", "remt",
        candidatePriority(IceCandidateType::PeerReflexive), StunIceRole::Controlling, 12345, true);
    const auto serverKey = stunShortTermKey("ServerPassword0123456789");
    const StunEncodeOptions auth{StunIntegrity::Sha1, serverKey, true};
    bool received = false, requestSent = false, responseSent = false;
    server.startReceive([&](auto datagram) {
        const auto packet = parsed(datagram.payload);
        CHECK(packet.validateIceBindingRequest("remt", "locl", serverKey));
        CHECK(datagram.remoteEndpoint == client.localEndpoint());
        auto response = StunMessage::bindingSuccess(packet, datagram.remoteEndpoint);
        server.asyncSendTo(response.encode(auth), datagram.remoteEndpoint, [&](auto ec, auto size) {
            CHECK(!ec && size > 20); responseSent = true;
        });
    });
    client.startReceive([&](auto datagram) {
        const auto packet = parsed(datagram.payload);
        CHECK(datagram.remoteEndpoint == server.localEndpoint());
        CHECK(packet.isResponseTo(request) && packet.verifyFingerprint() && packet.verifyIntegrity(serverKey));
        CHECK(!packet.find(Type::Username) && packet.mappedAddress() == client.localEndpoint());
        const auto candidate = IceCandidate::fromUdpEndpoint(*packet.mappedAddress(), IceCandidateType::ServerReflexive,
            "stun1", 1, 65535, client.localEndpoint());
        CHECK(candidate.validate() && candidate.udpEndpoint() == client.localEndpoint());
        received = true;
    });
    client.asyncSendTo(request.encode(auth), server.localEndpoint(), [&](auto ec, auto size) {
        CHECK(!ec && size > 20); requestSent = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while ((!received || !requestSent || !responseSent) && std::chrono::steady_clock::now() < deadline)
    { io.restart(); io.run_for(2ms); }
    CHECK(received && requestSent && responseSent);
    unsigned closed = 0;
    client.close([&](auto ec) { CHECK(!ec); ++closed; });
    server.close([&](auto ec) { CHECK(!ec); ++closed; });
    io.restart(); io.run();
    CHECK(closed == 2);
}
} // namespace

int main()
{
    try
    {
        officialVectors(); typesAttributesAndBoundaries(); integrityAndMalformedPackets(); udpBindingRoundtrip();
        std::cout << "PASS: STUN RFC vectors, codec, integrity, malformed packets, ICE and UDP Binding\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
