#include"ice_candidate.h"

#include<boost/asio/ip/address.hpp>

#include<algorithm>
#include<charconv>
#include<stdexcept>
#include<system_error>

namespace webrtc
{
namespace
{
constexpr std::size_t kMaxCandidateBytes = 4096;
constexpr std::uint32_t kMaxPriority = 0x7FFFFFFF;

bool fail(std::string* reason, const char* message)
{
    if (reason) *reason = message;
    return false;
}
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alnum(char c) { return alpha(c) || digit(c); }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equalNoCase(std::string_view first, std::string_view second)
{
    if (first.size() != second.size()) return false;
    for (std::size_t i = 0; i < first.size(); ++i)
        if (lower(first[i]) != lower(second[i])) return false;
    return true;
}
bool whitespace(char c) { return c == ' ' || c == '\t'; }

bool validFoundation(std::string_view value)
{
    return !value.empty() && value.size() <= 32 &&
           std::all_of(value.begin(), value.end(), [](char c) { return alnum(c) || c == '+' || c == '/'; });
}
bool validToken(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return alnum(c) || std::string_view("-.!%*_+`'~").find(c) != std::string_view::npos;
    });
}
bool validValue(std::string_view value)
{
    // 保留非空、可见 ASCII 的扩展值，防止空白/换行插入新的 SDP 字段。
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7E; });
}
std::optional<boost::asio::ip::address> numericAddress(std::string_view text)
{
    if (text.find_first_of("%[]") != std::string_view::npos) return std::nullopt; // SDP 无作用域/URL 方括号
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(std::string(text), ec);
    if (ec) return std::nullopt;
    return address;
}
bool validHostname(std::string_view name)
{
    if (!name.empty() && name.back() == '.') name.remove_suffix(1);
    if (name.empty() || name.back() == '.' || name.size() > 253 || name.find('.') == std::string_view::npos) return false;
    std::size_t offset = 0;
    std::string_view label;
    while (offset < name.size())
    {
        const auto dot = name.find('.', offset);
        label = name.substr(offset, dot == std::string_view::npos ? name.size() - offset : dot - offset);
        if (label.empty() || label.size() > 63 || !alnum(label.front()) || !alnum(label.back())) return false;
        for (char c : label) if (!alnum(c) && c != '-') return false;
        if (dot == std::string_view::npos) break;
        offset = dot + 1;
    }
    // 全数字的最后一段不是域名，不能将错误 IPv4（如 999.1.1.1）当成主机名。
    return std::any_of(label.begin(), label.end(), alpha);
}
bool validAddress(std::string_view address)
{
    return !address.empty() && address.size() <= 254 && (numericAddress(address).has_value() || validHostname(address));
}
bool reserved(std::string_view name)
{
    return equalNoCase(name, "raddr") || equalNoCase(name, "rport") || equalNoCase(name, "tcptype") || equalNoCase(name, "typ");
}
bool unsignedNumber(std::string_view text, std::uint32_t maximum, std::size_t maxDigits, std::uint32_t& value)
{
    if (text.empty() || text.size() > maxDigits || !std::all_of(text.begin(), text.end(), digit)) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value <= maximum;
}
std::vector<std::string_view> tokens(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t offset = 0;
    while (offset < line.size())
    {
        while (offset < line.size() && whitespace(line[offset])) ++offset;
        const auto start = offset;
        while (offset < line.size() && !whitespace(line[offset])) ++offset;
        if (offset > start) out.push_back(line.substr(start, offset - start));
    }
    return out;
}
void requireValid(const IceCandidate& candidate)
{
    std::string reason;
    if (!candidate.validate(&reason)) throw std::invalid_argument(reason);
}
} // namespace

const char* candidateTypeString(IceCandidateType type)
{
    switch (type)
    {
    case IceCandidateType::Host: return "host";
    case IceCandidateType::ServerReflexive: return "srflx";
    case IceCandidateType::PeerReflexive: return "prflx";
    case IceCandidateType::Relay: return "relay";
    }
    return nullptr;
}
const char* transportString(IceCandidateTransport transport)
{
    switch (transport)
    {
    case IceCandidateTransport::Udp: return "UDP";
    case IceCandidateTransport::Tcp: return "TCP";
    }
    return nullptr;
}
const char* tcpTypeString(IceTcpType type)
{
    switch (type)
    {
    case IceTcpType::Active: return "active";
    case IceTcpType::Passive: return "passive";
    case IceTcpType::SimultaneousOpen: return "so";
    }
    return nullptr;
}
unsigned defaultTypePreference(IceCandidateType type)
{
    switch (type)
    {
    case IceCandidateType::Host: return 126;
    case IceCandidateType::ServerReflexive: return 100;
    case IceCandidateType::PeerReflexive: return 110;
    case IceCandidateType::Relay: return 0;
    }
    throw std::invalid_argument("Unknown ICE candidate type");
}
std::uint32_t candidatePriority(unsigned typePreference, unsigned localPreference, unsigned component)
{
    if (typePreference > 126 || localPreference > 65535 || component < 1 || component > 256)
        throw std::invalid_argument("Invalid ICE type/local preference or component");
    const auto priority = (static_cast<std::uint32_t>(typePreference) << 24) +
                          (static_cast<std::uint32_t>(localPreference) << 8) + (256 - component);
    if (priority == 0) throw std::invalid_argument("ICE candidate priority must be positive");
    return priority;
}
std::uint32_t candidatePriority(IceCandidateType type, unsigned localPreference, unsigned component)
{
    return candidatePriority(defaultTypePreference(type), localPreference, component);
}
std::uint64_t candidatePairPriority(std::uint32_t localPriority, std::uint32_t remotePriority, bool localControlling)
{
    if (localPriority == 0 || remotePriority == 0 || localPriority > kMaxPriority || remotePriority > kMaxPriority)
        throw std::invalid_argument("ICE candidate pair priorities must be in [1, 2^31-1]");
    const std::uint64_t g = localControlling ? localPriority : remotePriority;
    const std::uint64_t d = localControlling ? remotePriority : localPriority;
    return (std::min(g, d) << 32) + 2 * std::max(g, d) + (g > d ? 1 : 0);
}

bool IceCandidate::validate(std::string* reason) const
{
    if (!validFoundation(foundation)) return fail(reason, "ICE foundation must contain 1~32 ICE characters");
    if (component < 1 || component > 256) return fail(reason, "ICE component must be in [1, 256]");
    if (!transportString(transport)) return fail(reason, "Unsupported ICE transport");
    if (!candidateTypeString(type)) return fail(reason, "Unsupported ICE candidate type");
    if (priority == 0 || priority > kMaxPriority) return fail(reason, "ICE priority must be in [1, 2^31-1]");
    if (!validAddress(address)) return fail(reason, "Invalid ICE address or hostname");
    if (port == 0) return fail(reason, "ICE candidate port must be in [1, 65535]");
    if (relatedAddress.has_value() != relatedPort.has_value()) return fail(reason, "ICE raddr and rport must appear together");
    if (type == IceCandidateType::Host && relatedAddress) return fail(reason, "Host candidates must not have raddr/rport");
    if (type != IceCandidateType::Host && !relatedAddress) return fail(reason, "Reflexive/relay candidates require raddr/rport");
    if (relatedAddress && !validAddress(*relatedAddress)) return fail(reason, "Invalid related ICE address");
    // 接收端保留 rport=0 的隐私占位值，便于与浏览器互操作；不把它作为发送端点。
    if (transport == IceCandidateTransport::Udp && tcpType) return fail(reason, "UDP candidate must not contain tcptype");
    if (transport == IceCandidateTransport::Tcp)
    {
        if (!tcpType || !tcpTypeString(*tcpType)) return fail(reason, "TCP candidate requires valid tcptype");
        if (*tcpType == IceTcpType::Active && port != 9) return fail(reason, "Active TCP candidate port must be 9");
    }
    // 精确计算完整 SDP 行（包含 a= 和 CRLF），确保序列化后仍可被同一解析器接收。
    std::size_t length = 4 + 10 + foundation.size() + 1 + std::to_string(component).size() +
                         1 + std::string_view(transportString(transport)).size() + 1 + std::to_string(priority).size() +
                         1 + address.size() + 1 + std::to_string(port).size() + 5 + std::string_view(candidateTypeString(type)).size();
    if (relatedAddress) length += 7 + relatedAddress->size() + 7 + std::to_string(*relatedPort).size();
    if (tcpType) length += 9 + std::string_view(tcpTypeString(*tcpType)).size();
    for (const auto& item : extensions)
    {
        // 每一项先检查长度，再相加，避免不受信任的字段尺寸造成溢出。
        if (item.first.size() > kMaxCandidateBytes || item.second.size() > kMaxCandidateBytes)
            return fail(reason, "ICE candidate exceeds size limit");
        if (!validToken(item.first) || !validValue(item.second)) return fail(reason, "Invalid ICE extension name/value");
        if (reserved(item.first)) return fail(reason, "Reserved ICE field used as an extension");
        length += item.first.size() + item.second.size() + 2;
        if (length > kMaxCandidateBytes) return fail(reason, "ICE candidate exceeds size limit");
    }
    if (length > kMaxCandidateBytes) return fail(reason, "ICE candidate exceeds size limit");
    if (reason) reason->clear();
    return true;
}

bool IceCandidate::parse(std::string_view line, IceCandidate& out, std::string* reason)
{
    if (line.size() > kMaxCandidateBytes) return fail(reason, "ICE candidate exceeds size limit");
    if (line.size() >= 2 && line.substr(line.size() - 2) == "\r\n") line.remove_suffix(2);
    else if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    for (unsigned char c : line)
        if ((c < 0x20 && c != '\t') || c > 0x7E) return fail(reason, "ICE candidate contains invalid characters or multiple lines");
    while (!line.empty() && whitespace(line.front())) line.remove_prefix(1);
    while (!line.empty() && whitespace(line.back())) line.remove_suffix(1);
    if (line.substr(0, 2) == "a=") line.remove_prefix(2);
    if (line.substr(0, 10) != "candidate:") return fail(reason, "Expected candidate: attribute");
    line.remove_prefix(10);
    const auto parts = tokens(line);
    if (parts.size() < 8 || (parts.size() - 8) % 2 != 0) return fail(reason, "Incomplete ICE candidate or extension pair");
    IceCandidate parsed;
    parsed.foundation = parts[0];
    std::uint32_t number;
    if (!unsignedNumber(parts[1], 256, 3, number) || number == 0) return fail(reason, "Invalid ICE component");
    parsed.component = static_cast<std::uint16_t>(number);
    if (equalNoCase(parts[2], "UDP")) parsed.transport = IceCandidateTransport::Udp;
    else if (equalNoCase(parts[2], "TCP")) parsed.transport = IceCandidateTransport::Tcp;
    else return fail(reason, "Unsupported ICE transport");
    if (!unsignedNumber(parts[3], kMaxPriority, 10, number) || number == 0) return fail(reason, "Invalid ICE priority");
    parsed.priority = number;
    parsed.address = parts[4];
    if (!unsignedNumber(parts[5], 65535, 5, number) || number == 0) return fail(reason, "Invalid ICE candidate port");
    parsed.port = static_cast<std::uint16_t>(number);
    if (!equalNoCase(parts[6], "typ")) return fail(reason, "Missing ICE typ field");
    if (equalNoCase(parts[7], "host")) parsed.type = IceCandidateType::Host;
    else if (equalNoCase(parts[7], "srflx")) parsed.type = IceCandidateType::ServerReflexive;
    else if (equalNoCase(parts[7], "prflx")) parsed.type = IceCandidateType::PeerReflexive;
    else if (equalNoCase(parts[7], "relay")) parsed.type = IceCandidateType::Relay;
    else return fail(reason, "Unsupported ICE candidate type");
    for (std::size_t i = 8; i < parts.size(); i += 2)
    {
        const auto name = parts[i], value = parts[i + 1];
        if (equalNoCase(name, "raddr"))
        {
            if (parsed.relatedAddress) return fail(reason, "Duplicate ICE raddr");
            parsed.relatedAddress = value;
        }
        else if (equalNoCase(name, "rport"))
        {
            if (parsed.relatedPort) return fail(reason, "Duplicate ICE rport");
            if (!unsignedNumber(value, 65535, 5, number)) return fail(reason, "Invalid ICE related port");
            parsed.relatedPort = static_cast<std::uint16_t>(number);
        }
        else if (equalNoCase(name, "tcptype"))
        {
            if (parsed.tcpType) return fail(reason, "Duplicate ICE tcptype");
            if (equalNoCase(value, "active")) parsed.tcpType = IceTcpType::Active;
            else if (equalNoCase(value, "passive")) parsed.tcpType = IceTcpType::Passive;
            else if (equalNoCase(value, "so")) parsed.tcpType = IceTcpType::SimultaneousOpen;
            else return fail(reason, "Unsupported ICE TCP type");
        }
        else parsed.extensions.emplace_back(name, value);
    }
    if (!parsed.validate(reason)) return false;
    out = std::move(parsed);
    return true;
}

std::string IceCandidate::toCandidateString() const
{
    requireValid(*this);
    std::string result = "candidate:" + foundation + " " + std::to_string(component) + " " + transportString(transport) +
                         " " + std::to_string(priority) + " " + address + " " + std::to_string(port) + " typ " + candidateTypeString(type);
    if (relatedAddress) result += " raddr " + *relatedAddress + " rport " + std::to_string(*relatedPort);
    if (tcpType) result += std::string(" tcptype ") + tcpTypeString(*tcpType);
    for (const auto& item : extensions) result += " " + item.first + " " + item.second;
    return result;
}
std::string IceCandidate::toSdp() const { return "a=" + toCandidateString() + "\r\n"; }
bool IceCandidate::isHostname() const { return validHostname(address) && !numericAddress(address); }
bool IceCandidate::isMdns() const
{
    if (!isHostname()) return false;
    std::string_view name(address);
    if (name.back() == '.') name.remove_suffix(1);
    return name.size() > 6 && equalNoCase(name.substr(name.size() - 6), ".local");
}
std::optional<IceCandidate::Endpoint> IceCandidate::udpEndpoint() const
{
    if (transport != IceCandidateTransport::Udp || !validate()) return std::nullopt;
    const auto ip = numericAddress(address);
    if (!ip) return std::nullopt;
    return Endpoint(*ip, port);
}
std::optional<std::string> IceCandidate::extension(std::string_view name) const
{
    for (const auto& item : extensions) if (equalNoCase(item.first, name)) return item.second;
    return std::nullopt;
}
IceCandidate IceCandidate::fromUdpEndpoint(const Endpoint& endpoint, IceCandidateType type,
                                          std::string foundation, unsigned component, unsigned localPreference,
                                          std::optional<Endpoint> related)
{
    if (endpoint.address().is_unspecified() || endpoint.address().is_multicast() ||
        (endpoint.address().is_v4() && endpoint.address().to_v4() == boost::asio::ip::address_v4::broadcast()))
        throw std::invalid_argument("Local ICE candidate requires a concrete unicast address");
    IceCandidate candidate;
    candidate.foundation = std::move(foundation);
    candidate.priority = candidatePriority(type, localPreference, component);
    candidate.component = static_cast<std::uint16_t>(component);
    candidate.address = endpoint.address().to_string();
    candidate.port = endpoint.port();
    candidate.type = type;
    if (related)
    {
        candidate.relatedAddress = related->address().to_string();
        candidate.relatedPort = related->port();
    }
    requireValid(candidate);
    return candidate;
}
IceCandidate IceCandidate::host(const Endpoint& local, std::string foundation, unsigned component, unsigned localPreference)
{
    return fromUdpEndpoint(local, IceCandidateType::Host, std::move(foundation), component, localPreference);
}
} // namespace webrtc
