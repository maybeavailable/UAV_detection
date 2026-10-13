#include"webrtc/stun_protocol.h"

#include<openssl/crypto.h>
#include<openssl/evp.h>
#include<openssl/hmac.h>
#include<openssl/rand.h>

#include<algorithm>
#include<bitset>
#include<climits>
#include<limits>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace
{
using Type = StunAttributeType;
bool fail(std::string* reason, const char* text)
{
    if (reason) *reason = text;
    return false;
}
bool success(std::string* reason)
{
    if (reason) reason->clear();
    return true;
}
std::uint16_t read16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}
std::uint32_t read32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
void write16(std::uint8_t* p, std::uint16_t n)
{
    p[0] = static_cast<std::uint8_t>(n >> 8);
    p[1] = static_cast<std::uint8_t>(n);
}
void write32(std::uint8_t* p, std::uint32_t n)
{
    p[0] = static_cast<std::uint8_t>(n >> 24);
    p[1] = static_cast<std::uint8_t>(n >> 16);
    p[2] = static_cast<std::uint8_t>(n >> 8);
    p[3] = static_cast<std::uint8_t>(n);
}
std::size_t padded(std::size_t size) { return (size + 3) & ~std::size_t(3); }
bool isText(Type type)
{
    return type == Type::Username || type == Type::Realm || type == Type::Nonce || type == Type::Software;
}
bool isAddress(Type type)
{
    return type == Type::MappedAddress || type == Type::XorMappedAddress || type == Type::AlternateServer;
}
bool isSecurity(Type type)
{
    return type == Type::MessageIntegrity || type == Type::MessageIntegritySha256 || type == Type::Fingerprint;
}
bool known(Type type)
{
    switch (type)
    {
    case Type::MappedAddress: case Type::Username: case Type::MessageIntegrity:
    case Type::ErrorCode: case Type::UnknownAttributes: case Type::Realm: case Type::Nonce:
    case Type::MessageIntegritySha256: case Type::PasswordAlgorithm: case Type::PasswordAlgorithms:
    case Type::Userhash: case Type::XorMappedAddress:
    case Type::Priority: case Type::UseCandidate: case Type::Software: case Type::AlternateServer:
    case Type::Fingerprint: case Type::IceControlled: case Type::IceControlling: return true;
    }
    return false;
}
// 严格 RFC 3629 UTF-8，排除 overlong、surrogate 和 >U+10FFFF；不代替 PRECIS 规范化。
std::optional<std::size_t> utf8Characters(std::string_view text)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++count)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        unsigned remaining;
        std::uint32_t code, minimum;
        if (first < 0x80) continue;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; code = first & 0x07; minimum = 0x10000; }
        else return std::nullopt;
        if (remaining > text.size() - i) return std::nullopt;
        while (remaining--)
        {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return std::nullopt;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return std::nullopt;
    }
    return count;
}
bool validText(const StunBytes& bytes, bool sending, bool username)
{
    const auto limit = sending ? (username ? 508u : 509u) : 763u;
    if (bytes.size() > limit) return false;
    const auto count = utf8Characters(bytes.empty() ? std::string_view{} :
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    return count && (!sending || username || *count < 128);
}
bool validAttribute(const StunAttribute& attribute, bool sending)
{
    const auto type = attribute.type;
    const auto& value = attribute.value;
    if (value.size() > 65535) return false;
    if (isText(type)) return validText(value, sending, type == Type::Username);
    if (isAddress(type))
        return value.size() >= 4 && ((value[1] == 1 && value.size() == 8) || (value[1] == 2 && value.size() == 20) ||
                                    (!sending && value[1] != 1 && value[1] != 2));
    switch (type)
    {
    case Type::MessageIntegrity: return value.size() == 20;
    case Type::MessageIntegritySha256: return value.size() >= 16 && value.size() <= 32 && value.size() % 4 == 0;
    case Type::Userhash: return value.size() == 32;
    case Type::Fingerprint: case Type::Priority: return value.size() == 4;
    case Type::IceControlled: case Type::IceControlling: return value.size() == 8;
    case Type::UseCandidate: return value.empty();
    case Type::UnknownAttributes: return value.size() % 2 == 0;
    case Type::PasswordAlgorithm: case Type::PasswordAlgorithms:
    {
        std::size_t offset = 0;
        while (offset < value.size())
        {
            if (value.size() - offset < 4) return false;
            const auto algorithm = read16(value.data() + offset);
            const auto length = read16(value.data() + offset + 2);
            const auto size = 4 + padded(length);
            if (size > value.size() - offset || ((algorithm == 1 || algorithm == 2) && length != 0)) return false;
            offset += size;
            if (type == Type::PasswordAlgorithm) return offset == value.size();
        }
        return type == Type::PasswordAlgorithms;
    }
    case Type::ErrorCode:
        return value.size() >= 4 && (value[2] & 7) >= 3 && (value[2] & 7) <= 6 && value[3] <= 99 &&
               validText(StunBytes(value.begin() + 4, value.end()), sending, false);
    default: return true; // 扩展属性保留原始数据；由扩展协议解释
    }
}
std::uint16_t messageType(StunMethod method, StunClass messageClass)
{
    const auto m = static_cast<std::uint16_t>(method);
    const auto c = static_cast<unsigned>(messageClass);
    if (m == 0 || m > 0xfff || c > 3) throw std::invalid_argument("invalid STUN method/class");
    return static_cast<std::uint16_t>((m & 0x00f) | ((m & 0x070) << 1) | ((m & 0xf80) << 2) |
                                      ((c & 1) << 4) | ((c & 2) << 7));
}
StunBytes digest(const EVP_MD* algorithm, std::string_view text)
{
    StunBytes result(EVP_MAX_MD_SIZE);
    unsigned size = 0;
    if (EVP_Digest(text.data(), text.size(), result.data(), &size, algorithm, nullptr) != 1)
        throw std::runtime_error("STUN key digest failed");
    result.resize(size);
    return result;
}
StunBytes hmac(const StunBytes& wire, std::size_t offset, std::size_t valueSize,
               const StunBytes& key, const EVP_MD* algorithm)
{
    if (key.empty() || key.size() > INT_MAX) throw std::invalid_argument("invalid STUN integrity key length");
    StunBytes input(wire.begin(), wire.begin() + offset);
    write16(input.data() + 2, static_cast<std::uint16_t>(offset + 4 + valueSize - StunHeaderSize));
    StunBytes result(EVP_MAX_MD_SIZE);
    unsigned size = 0;
    if (!HMAC(algorithm, key.data(), static_cast<int>(key.size()), input.data(), input.size(), result.data(), &size))
        throw std::runtime_error("STUN HMAC failed");
    result.resize(size);
    return result;
}
std::uint32_t fingerprint(const StunBytes& wire, std::size_t size)
{
    std::uint32_t crc = 0xffffffff;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= wire[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return (crc ^ 0xffffffff) ^ 0x5354554e;
}
void appendAttribute(StunBytes& wire, const StunAttribute& attribute)
{
    const auto offset = wire.size();
    const auto size = attribute.value.size();
    if (size > 65535 || offset + 4 + padded(size) > StunMaxMessageSize)
        throw std::invalid_argument("STUN message too large");
    wire.resize(offset + 4 + padded(size), 0);
    write16(wire.data() + offset, static_cast<std::uint16_t>(attribute.type));
    write16(wire.data() + offset + 2, static_cast<std::uint16_t>(size));
    std::copy(attribute.value.begin(), attribute.value.end(), wire.begin() + offset + 4);
}
bool validUfrag(std::string_view fragment)
{
    return fragment.size() >= 4 && fragment.size() <= 256 &&
        std::all_of(fragment.begin(), fragment.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        });
}
void requireBindingRequest(const StunPacket& request)
{
    if (request.wire().empty() || request.method() != StunMethod::Binding || request.messageClass() != StunClass::Request)
        throw std::invalid_argument("expected a parsed STUN Binding request");
}
} // namespace

StunBytes stunShortTermKey(std::string_view normalizedPassword)
{
    if (normalizedPassword.empty()) throw std::invalid_argument("empty STUN password");
    return StunBytes(normalizedPassword.begin(), normalizedPassword.end());
}
StunBytes stunLongTermKey(std::string_view username, std::string_view realm, std::string_view password,
                         StunPasswordAlgorithm algorithm)
{
    const EVP_MD* md = nullptr;
    switch (algorithm)
    {
    case StunPasswordAlgorithm::Md5: md = EVP_md5(); break;
    case StunPasswordAlgorithm::Sha256: md = EVP_sha256(); break;
    default: throw std::invalid_argument("invalid STUN password algorithm");
    }
    return digest(md, std::string(username) + ":" + std::string(realm) + ":" + std::string(password));
}
StunBytes stunUserhash(std::string_view username, std::string_view realm)
{
    return digest(EVP_sha256(), std::string(username) + ":" + std::string(realm));
}
StunTransactionId generateStunTransactionId()
{
    StunTransactionId id{};
    if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1) throw std::runtime_error("STUN transaction RNG failed");
    return id;
}

StunAttribute StunAttribute::text(Type type, std::string_view text)
{
    if (!isText(type)) throw std::invalid_argument("not a STUN text attribute");
    StunAttribute result{type, StunBytes(text.begin(), text.end())};
    if (!validAttribute(result, true)) throw std::invalid_argument("invalid STUN text attribute");
    return result;
}
StunAttribute StunAttribute::uint32(Type type, std::uint32_t value)
{
    if (type != Type::Priority) throw std::invalid_argument("not a STUN uint32 attribute");
    StunAttribute result{type, StunBytes(4)};
    write32(result.value.data(), value);
    return result;
}
StunAttribute StunAttribute::uint64(Type type, std::uint64_t value)
{
    if (type != Type::IceControlled && type != Type::IceControlling)
        throw std::invalid_argument("not a STUN uint64 attribute");
    StunAttribute result{type, StunBytes(8)};
    write32(result.value.data(), static_cast<std::uint32_t>(value >> 32));
    write32(result.value.data() + 4, static_cast<std::uint32_t>(value));
    return result;
}
StunAttribute StunAttribute::flag(Type type)
{
    if (type != Type::UseCandidate) throw std::invalid_argument("not a STUN flag attribute");
    return {type, {}};
}
StunAttribute StunAttribute::address(Type type, const StunEndpoint& endpoint, const StunTransactionId& id)
{
    if (!isAddress(type)) throw std::invalid_argument("not a STUN address attribute");
    const bool ipv4 = endpoint.address().is_v4();
    if (!ipv4 && endpoint.address().to_v6().scope_id() != 0)
        throw std::invalid_argument("STUN address cannot encode an IPv6 scope ID");
    StunAttribute result{type, StunBytes(ipv4 ? 8 : 20, 0)};
    result.value[1] = ipv4 ? 1 : 2;
    write16(result.value.data() + 2, endpoint.port());
    if (ipv4)
    {
        const auto bytes = endpoint.address().to_v4().to_bytes();
        std::copy(bytes.begin(), bytes.end(), result.value.begin() + 4);
    }
    else
    {
        const auto bytes = endpoint.address().to_v6().to_bytes();
        std::copy(bytes.begin(), bytes.end(), result.value.begin() + 4);
    }
    if (type == Type::XorMappedAddress)
    {
        write16(result.value.data() + 2, static_cast<std::uint16_t>(endpoint.port() ^ (StunMagicCookie >> 16)));
        std::array<std::uint8_t, 16> mask{};
        write32(mask.data(), StunMagicCookie);
        std::copy(id.begin(), id.end(), mask.begin() + 4);
        for (std::size_t i = 4; i < result.value.size(); ++i) result.value[i] ^= mask[i - 4];
    }
    return result;
}
StunAttribute StunAttribute::error(std::uint16_t code, std::string_view reason)
{
    if (code < 300 || code > 699) throw std::invalid_argument("invalid STUN error code");
    StunAttribute result{Type::ErrorCode, {0, 0, static_cast<std::uint8_t>(code / 100), static_cast<std::uint8_t>(code % 100)}};
    result.value.insert(result.value.end(), reason.begin(), reason.end());
    if (!validAttribute(result, true)) throw std::invalid_argument("invalid STUN error reason");
    return result;
}
StunAttribute StunAttribute::unknownAttributes(const std::vector<std::uint16_t>& types)
{
    auto sorted = types;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.size() > 32767 || (!sorted.empty() && sorted.back() >= 0x8000))
        throw std::invalid_argument("invalid STUN unknown required attributes");
    StunAttribute result{Type::UnknownAttributes, StunBytes(sorted.size() * 2)};
    for (std::size_t i = 0; i < sorted.size(); ++i) write16(result.value.data() + i * 2, sorted[i]);
    return result;
}
StunAttribute StunAttribute::passwordAlgorithm(StunPasswordAlgorithm algorithm)
{
    if (algorithm != StunPasswordAlgorithm::Md5 && algorithm != StunPasswordAlgorithm::Sha256)
        throw std::invalid_argument("unsupported STUN password algorithm");
    StunAttribute result{Type::PasswordAlgorithm, StunBytes(4, 0)};
    write16(result.value.data(), static_cast<std::uint16_t>(algorithm));
    return result;
}
StunAttribute StunAttribute::passwordAlgorithms(const std::vector<StunPasswordAlgorithm>& algorithms)
{
    if (algorithms.size() > 16383) throw std::invalid_argument("too many STUN password algorithms");
    StunAttribute result{Type::PasswordAlgorithms, {}};
    for (const auto algorithm : algorithms)
    {
        const auto entry = passwordAlgorithm(algorithm);
        result.value.insert(result.value.end(), entry.value.begin(), entry.value.end());
    }
    return result;
}
std::optional<std::string> StunAttribute::asText() const
{
    if (!isText(type) || !validAttribute(*this, false)) return std::nullopt;
    return std::string(value.begin(), value.end());
}
std::optional<std::uint32_t> StunAttribute::asUint32() const
{
    if ((type != Type::Priority && type != Type::Fingerprint) || value.size() != 4) return std::nullopt;
    return read32(value.data());
}
std::optional<std::uint64_t> StunAttribute::asUint64() const
{
    if ((type != Type::IceControlled && type != Type::IceControlling) || value.size() != 8) return std::nullopt;
    return (std::uint64_t(read32(value.data())) << 32) | read32(value.data() + 4);
}
std::optional<StunEndpoint> StunAttribute::asAddress(const StunTransactionId& id) const
{
    if (!isAddress(type) || !validAttribute(*this, false)) return std::nullopt;
    if (value[1] != 1 && value[1] != 2) return std::nullopt; // RFC 8489 §14.1：忽略不支持的地址族
    StunBytes bytes(value.begin() + 4, value.end());
    auto port = read16(value.data() + 2);
    if (type == Type::XorMappedAddress)
    {
        port ^= StunMagicCookie >> 16;
        std::array<std::uint8_t, 16> mask{};
        write32(mask.data(), StunMagicCookie);
        std::copy(id.begin(), id.end(), mask.begin() + 4);
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] ^= mask[i];
    }
    if (value[1] == 1)
    {
        boost::asio::ip::address_v4::bytes_type address{};
        std::copy(bytes.begin(), bytes.end(), address.begin());
        return StunEndpoint(boost::asio::ip::address_v4(address), port);
    }
    boost::asio::ip::address_v6::bytes_type address{};
    std::copy(bytes.begin(), bytes.end(), address.begin());
    return StunEndpoint(boost::asio::ip::address_v6(address), port);
}
std::optional<StunError> StunAttribute::asError() const
{
    if (type != Type::ErrorCode || !validAttribute(*this, false)) return std::nullopt;
    return StunError{static_cast<std::uint16_t>((value[2] & 7) * 100 + value[3]),
                     std::string(value.begin() + 4, value.end())};
}
std::optional<std::vector<std::uint16_t>> StunAttribute::asUnknownAttributes() const
{
    if (type != Type::UnknownAttributes || value.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint16_t> result;
    for (std::size_t i = 0; i < value.size(); i += 2) result.push_back(read16(value.data() + i));
    return result;
}
std::optional<StunPasswordAlgorithm> StunAttribute::asPasswordAlgorithm() const
{
    if (type != Type::PasswordAlgorithm || value.size() != 4 || read16(value.data() + 2) != 0) return std::nullopt;
    const auto algorithm = static_cast<StunPasswordAlgorithm>(read16(value.data()));
    if (algorithm != StunPasswordAlgorithm::Md5 && algorithm != StunPasswordAlgorithm::Sha256) return std::nullopt;
    return algorithm;
}

StunBytes StunMessage::encode(const StunEncodeOptions& options) const
{
    StunBytes wire(StunHeaderSize, 0);
    write16(wire.data(), messageType(method, messageClass));
    write32(wire.data() + 4, StunMagicCookie);
    std::copy(transactionId.begin(), transactionId.end(), wire.begin() + 8);
    for (const auto& attribute : attributes)
    {
        if (isSecurity(attribute.type)) throw std::invalid_argument("encode STUN security attributes through options");
        if (!validAttribute(attribute, true)) throw std::invalid_argument("invalid STUN attribute");
        appendAttribute(wire, attribute);
    }
    if (options.integrity)
    {
        const auto algorithm = *options.integrity;
        if (algorithm != StunIntegrity::Sha1 && algorithm != StunIntegrity::Sha256 && algorithm != StunIntegrity::Sha1AndSha256)
            throw std::invalid_argument("invalid STUN integrity algorithm");
        auto appendIntegrity = [&](Type type, std::size_t size, const EVP_MD* md) {
            const auto offset = wire.size();
            appendAttribute(wire, {type, StunBytes(size, 0)});
            const auto value = hmac(wire, offset, size, options.key, md);
            std::copy_n(value.begin(), size, wire.begin() + offset + 4);
        };
        if (algorithm != StunIntegrity::Sha256) appendIntegrity(Type::MessageIntegrity, 20, EVP_sha1());
        if (algorithm != StunIntegrity::Sha1) appendIntegrity(Type::MessageIntegritySha256, 32, EVP_sha256());
    }
    else if (!options.key.empty()) throw std::invalid_argument("STUN key supplied without integrity algorithm");
    std::optional<std::size_t> fingerprintOffset;
    if (options.fingerprint)
    {
        fingerprintOffset = wire.size();
        appendAttribute(wire, {Type::Fingerprint, StunBytes(4, 0)});
    }
    write16(wire.data() + 2, static_cast<std::uint16_t>(wire.size() - StunHeaderSize));
    if (fingerprintOffset) write32(wire.data() + *fingerprintOffset + 4, fingerprint(wire, *fingerprintOffset));
    return wire;
}
StunMessage StunMessage::bindingRequest(std::string_view software)
{
    StunMessage result;
    result.transactionId = generateStunTransactionId();
    if (!software.empty()) result.attributes.push_back(StunAttribute::text(Type::Software, software));
    return result;
}
StunMessage StunMessage::bindingIndication()
{
    auto result = bindingRequest();
    result.messageClass = StunClass::Indication;
    return result;
}
StunMessage StunMessage::iceBindingRequest(std::string_view localUfrag, std::string_view remoteUfrag,
                                         std::uint32_t priority, StunIceRole role,
                                         std::uint64_t tieBreaker, bool useCandidate)
{
    if (!validUfrag(localUfrag) || !validUfrag(remoteUfrag)) throw std::invalid_argument("invalid ICE ufrag");
    if (priority == 0 || priority > 0x7fffffff) throw std::invalid_argument("invalid ICE peer-reflexive priority");
    if (role != StunIceRole::Controlled && role != StunIceRole::Controlling) throw std::invalid_argument("invalid ICE role");
    if (useCandidate && role != StunIceRole::Controlling) throw std::invalid_argument("only controlling ICE agent can nominate");
    auto result = bindingRequest();
    result.attributes.push_back(StunAttribute::text(Type::Username, std::string(remoteUfrag) + ":" + std::string(localUfrag)));
    result.attributes.push_back(StunAttribute::uint32(Type::Priority, priority));
    result.attributes.push_back(StunAttribute::uint64(role == StunIceRole::Controlling ? Type::IceControlling : Type::IceControlled, tieBreaker));
    if (useCandidate) result.attributes.push_back(StunAttribute::flag(Type::UseCandidate));
    return result;
}
StunMessage StunMessage::bindingSuccess(const StunPacket& request, const StunEndpoint& mapped)
{
    requireBindingRequest(request);
    StunMessage result;
    result.messageClass = StunClass::SuccessResponse;
    result.transactionId = request.transactionId();
    result.attributes.push_back(StunAttribute::address(Type::XorMappedAddress, mapped, result.transactionId));
    return result;
}
StunMessage StunMessage::bindingError(const StunPacket& request, std::uint16_t code,
                                    std::string_view reason, const std::vector<std::uint16_t>& unknown)
{
    requireBindingRequest(request);
    if ((code == 420) != !unknown.empty()) throw std::invalid_argument("420 response requires unknown attributes only");
    StunMessage result;
    result.messageClass = StunClass::ErrorResponse;
    result.transactionId = request.transactionId();
    result.attributes.push_back(StunAttribute::error(code, reason));
    if (!unknown.empty()) result.attributes.push_back(StunAttribute::unknownAttributes(unknown));
    return result;
}

bool StunPacket::looksLikeStun(const void* data, std::size_t size)
{
    if (!data || size < StunHeaderSize || size > StunMaxMessageSize) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto length = read16(bytes + 2);
    return (bytes[0] & 0xc0) == 0 && read32(bytes + 4) == StunMagicCookie && length % 4 == 0 &&
           size == StunHeaderSize + length;
}
bool StunPacket::parse(const StunBytes& bytes, StunPacket& out, std::string* reason)
{
    return parse(bytes.data(), bytes.size(), out, reason);
}
bool StunPacket::parse(const void* data, std::size_t size, StunPacket& out, std::string* reason)
{
    if (!looksLikeStun(data, size)) return fail(reason, "invalid STUN header or datagram length");
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    const auto type = read16(bytes);
    const auto method = (type & 0x000f) | ((type & 0x00e0) >> 1) | ((type & 0x3e00) >> 2);
    if (method == 0) return fail(reason, "reserved STUN method");
    StunPacket result;
    result.method_ = static_cast<StunMethod>(method);
    result.class_ = static_cast<StunClass>(((type >> 4) & 1) | ((type >> 7) & 2));
    std::copy_n(bytes + 8, 12, result.transactionId_.begin());
    bool afterIntegrity = false;
    std::bitset<65536> seen;
    for (std::size_t offset = StunHeaderSize; offset < size;)
    {
        if (size - offset < 4) return fail(reason, "truncated STUN attribute header");
        const auto attributeType = static_cast<Type>(read16(bytes + offset));
        const auto length = read16(bytes + offset + 2);
        if (4 + padded(length) > size - offset) return fail(reason, "truncated STUN attribute value or padding");
        const bool security = isSecurity(attributeType);
        if (!afterIntegrity || security)
        {
            StunAttribute attribute{attributeType, StunBytes(bytes + offset + 4, bytes + offset + 4 + length)};
            const auto index = static_cast<std::uint16_t>(attributeType);
            // 普通重复属性只处理首项，避免后续副本改变已经读取的字段。
            if ((security || !seen[index]) && !validAttribute(attribute, false))
                return fail(reason, "invalid STUN attribute format");
            if (attributeType == Type::MessageIntegrity)
            {
                if (result.sha1Offset_ || result.sha256Offset_) return fail(reason, "duplicate or out-of-order STUN integrity");
                result.sha1Offset_ = offset;
                afterIntegrity = true;
            }
            else if (attributeType == Type::MessageIntegritySha256)
            {
                if (result.sha256Offset_) return fail(reason, "duplicate STUN SHA256 integrity");
                result.sha256Offset_ = offset;
                afterIntegrity = true;
            }
            else if (attributeType == Type::Fingerprint)
            {
                if (result.fingerprintOffset_ || offset + 8 != size) return fail(reason, "STUN FINGERPRINT must be last");
                result.fingerprintOffset_ = offset;
            }
            if (!seen[index]) result.attributes_.push_back(std::move(attribute));
            seen[index] = true;
        }
        offset += 4 + padded(length);
    }
    result.wire_.assign(bytes, bytes + size);
    out = std::move(result);
    return success(reason);
}
StunMethod StunPacket::method() const { return method_; }
StunClass StunPacket::messageClass() const { return class_; }
const StunTransactionId& StunPacket::transactionId() const { return transactionId_; }
const StunBytes& StunPacket::wire() const { return wire_; }
const std::vector<StunAttribute>& StunPacket::attributes() const { return attributes_; }
const StunAttribute* StunPacket::find(Type type) const
{
    const auto it = std::find_if(attributes_.begin(), attributes_.end(), [type](const auto& attribute) { return attribute.type == type; });
    return it == attributes_.end() ? nullptr : &*it;
}
std::optional<StunEndpoint> StunPacket::mappedAddress() const
{
    if (const auto* attribute = find(Type::XorMappedAddress))
        if (const auto endpoint = attribute->asAddress(transactionId_)) return endpoint;
    if (const auto* attribute = find(Type::MappedAddress)) return attribute->asAddress(transactionId_);
    return std::nullopt;
}
std::vector<std::uint16_t> StunPacket::unknownRequiredAttributes() const
{
    std::vector<std::uint16_t> result;
    for (const auto& attribute : attributes_)
    {
        const auto type = static_cast<std::uint16_t>(attribute.type);
        if (type < 0x8000 && !known(attribute.type)) result.push_back(type);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::optional<StunIntegrity> StunPacket::preferredIntegrity() const
{
    if (sha256Offset_) return StunIntegrity::Sha256;
    if (sha1Offset_) return StunIntegrity::Sha1;
    return std::nullopt;
}
bool StunPacket::verifyFingerprint(std::string* reason) const
{
    if (!fingerprintOffset_ || wire_.size() < *fingerprintOffset_ + 8) return fail(reason, "missing STUN FINGERPRINT");
    if (fingerprint(wire_, *fingerprintOffset_) != read32(wire_.data() + *fingerprintOffset_ + 4))
        return fail(reason, "STUN FINGERPRINT mismatch");
    return success(reason);
}
bool StunPacket::verifyIntegrity(const StunBytes& key, StunIntegrity algorithm,
                                 std::string* reason, bool allowTruncatedSha256) const
{
    if (key.empty() || key.size() > INT_MAX) return fail(reason, "invalid STUN integrity key length");
    if (algorithm != StunIntegrity::Sha1 && algorithm != StunIntegrity::Sha256 && algorithm != StunIntegrity::Sha1AndSha256)
        return fail(reason, "invalid STUN integrity algorithm");
    auto verify = [&](std::optional<std::size_t> offset, const EVP_MD* md, bool sha256) {
        if (!offset || wire_.size() < *offset + 4) return fail(reason, "missing required STUN integrity attribute");
        const auto size = read16(wire_.data() + *offset + 2);
        if (wire_.size() < *offset + 4 + size) return fail(reason, "missing required STUN integrity attribute");
        if (sha256 && size != 32 && !allowTruncatedSha256) return fail(reason, "truncated STUN SHA256 not permitted");
        const auto expected = hmac(wire_, *offset, size, key, md);
        if (CRYPTO_memcmp(expected.data(), wire_.data() + *offset + 4, size) != 0)
            return fail(reason, "STUN integrity mismatch");
        return true;
    };
    if (algorithm != StunIntegrity::Sha256 && !verify(sha1Offset_, EVP_sha1(), false)) return false;
    if (algorithm != StunIntegrity::Sha1 && !verify(sha256Offset_, EVP_sha256(), true)) return false;
    return success(reason);
}
bool StunPacket::validateIceBindingRequest(std::string_view localUfrag, std::string_view remoteUfrag,
                                          const StunBytes& localKey, std::string* reason, StunIntegrity algorithm) const
{
    if (wire_.empty() || method_ != StunMethod::Binding || class_ != StunClass::Request)
        return fail(reason, "not an ICE Binding request");
    if (!validUfrag(localUfrag) || !validUfrag(remoteUfrag)) return fail(reason, "invalid expected ICE ufrag");
    if (!verifyFingerprint(reason) || !verifyIntegrity(localKey, algorithm, reason)) return false;
    const auto* username = find(Type::Username);
    if (!username || username->asText() != std::string(localUfrag) + ":" + std::string(remoteUfrag))
        return fail(reason, "unexpected ICE USERNAME");
    const auto* priority = find(Type::Priority);
    if (!priority || !priority->asUint32() || *priority->asUint32() == 0 || *priority->asUint32() > 0x7fffffff)
        return fail(reason, "missing or invalid ICE PRIORITY");
    const bool controlling = find(Type::IceControlling) != nullptr;
    const bool controlled = find(Type::IceControlled) != nullptr;
    if (controlling == controlled) return fail(reason, "ICE request requires exactly one role");
    if (find(Type::UseCandidate) && !controlling) return fail(reason, "controlled ICE peer cannot nominate");
    if (!unknownRequiredAttributes().empty()) return fail(reason, "unknown required STUN attributes");
    return success(reason);
}
bool StunPacket::isResponseTo(const StunMessage& request) const
{
    return !wire_.empty() && request.messageClass == StunClass::Request && method_ == request.method &&
           transactionId_ == request.transactionId && (class_ == StunClass::SuccessResponse || class_ == StunClass::ErrorResponse);
}
} // namespace webrtc
