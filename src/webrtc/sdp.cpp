#include"webrtc/sdp.h"

#include<boost/asio/ip/address.hpp>

#include<algorithm>
#include<charconv>
#include<limits>
#include<set>
#include<stdexcept>
#include<utility>

namespace webrtc
{
namespace
{
constexpr std::size_t kMaxBytes = 256 * 1024, kMaxLines = 4096, kMaxLineBytes = 8192, kMaxMedia = 64;
bool fail(std::string* reason, const std::string& message) { if (reason) *reason = message; return false; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool equal(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}
bool digit(char c) { return c >= '0' && c <= '9'; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool space(char c) { return c == ' ' || c == '\t'; }
std::string_view trim(std::string_view value)
{
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    return value;
}
bool textValue(std::string_view value, bool empty = false)
{
    return (empty || !value.empty()) && value.size() <= kMaxLineBytes - 2 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return (c >= 0x20 && c != 0x7F) || c == '\t'; });
}
bool word(std::string_view value)
{
    return !value.empty() && value.size() <= kMaxLineBytes - 2 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7E; });
}
bool token(std::string_view value)
{
    return word(value) && std::all_of(value.begin(), value.end(), [](char c) {
        return alpha(c) || digit(c) || std::string_view("!#$%&'*+-.^_`{|}~").find(c) != std::string_view::npos;
    });
}
std::vector<std::string_view> split(std::string_view value, char delimiter)
{
    std::vector<std::string_view> result;
    for (;;)
    {
        const auto at = value.find(delimiter);
        result.push_back(value.substr(0, at));
        if (at == std::string_view::npos) return result;
        value.remove_prefix(at + 1);
    }
}
std::vector<std::string_view> words(std::string_view value)
{
    std::vector<std::string_view> result;
    while (!(value = trim(value)).empty())
    {
        const auto at = value.find_first_of(" \t");
        result.push_back(value.substr(0, at));
        if (at == std::string_view::npos) break;
        value.remove_prefix(at + 1);
    }
    return result;
}
bool number(std::string_view value, std::uint64_t maximum, std::uint64_t& out)
{
    if (value.empty() || value.size() > 20 || !std::all_of(value.begin(), value.end(), digit)) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && out <= maximum;
}
std::optional<unsigned> payload(std::string_view value)
{
    std::uint64_t parsed;
    if (!number(value, 127, parsed)) return std::nullopt;
    return static_cast<unsigned>(parsed);
}
bool parseDirection(std::string_view value, SdpDirection& out)
{
    if (equal(value, "sendrecv")) out = SdpDirection::SendRecv;
    else if (equal(value, "sendonly")) out = SdpDirection::SendOnly;
    else if (equal(value, "recvonly")) out = SdpDirection::RecvOnly;
    else if (equal(value, "inactive")) out = SdpDirection::Inactive;
    else return false;
    return true;
}
bool parseSetup(std::string_view value, SdpSetupRole& out)
{
    if (equal(value, "actpass")) out = SdpSetupRole::ActPass;
    else if (equal(value, "active")) out = SdpSetupRole::Active;
    else if (equal(value, "passive")) out = SdpSetupRole::Passive;
    else if (equal(value, "holdconn")) out = SdpSetupRole::HoldConn;
    else return false;
    return true;
}
bool validHost(std::string_view name)
{
    if (!name.empty() && name.back() == '.') name.remove_suffix(1);
    if (name.empty() || name.back() == '.' || name.size() > 253) return false;
    const auto labels = split(name, '.');
    for (const auto label : labels)
    {
        if (label.empty() || label.size() > 63 || !(alpha(label.front()) || digit(label.front())) ||
            !(alpha(label.back()) || digit(label.back()))) return false;
        for (char c : label) if (!alpha(c) && !digit(c) && c != '-') return false;
    }
    return std::any_of(labels.back().begin(), labels.back().end(), alpha);
}
bool validConnection(const SdpConnection& connection)
{
    if ((connection.addressType != "IP4" && connection.addressType != "IP6") ||
        connection.address.size() > 254 || connection.address.find_first_of("%[]/") != std::string::npos) return false;
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(connection.address, ec);
    if (ec) return validHost(connection.address);
    return !address.is_multicast() && (address.is_v4() == (connection.addressType == "IP4"));
}
bool parseConnection(const std::vector<std::string_view>& parts, std::size_t offset, SdpConnection& out)
{
    if (parts.size() != offset + 3 || parts[offset] != "IN") return false;
    out = {std::string(parts[offset + 1]), std::string(parts[offset + 2])};
    return validConnection(out);
}
bool iceString(std::string_view value, std::size_t minimum)
{
    return value.size() >= minimum && value.size() <= 256 &&
        std::all_of(value.begin(), value.end(), [](char c) { return alpha(c) || digit(c) || c == '+' || c == '/'; });
}
bool validFingerprint(const SdpFingerprint& fingerprint)
{
    if (!token(fingerprint.algorithm) || fingerprint.value.empty() || fingerprint.value.size() > 383) return false;
    const auto octets = split(fingerprint.value, ':');
    for (const auto octet : octets)
        if (octet.size() != 2 || !std::all_of(octet.begin(), octet.end(), [](char c) {
            return digit(c) || (lower(c) >= 'a' && lower(c) <= 'f');
        })) return false;
    const std::vector<std::pair<std::string_view, unsigned>> sizes{
        {"sha-1", 20}, {"sha-224", 28}, {"sha-256", 32}, {"sha-384", 48}, {"sha-512", 64}, {"md5", 16}, {"md2", 16}};
    for (const auto& size : sizes) if (equal(fingerprint.algorithm, size.first)) return octets.size() == size.second;
    return true; // 未知算法保留信息；是否支持/使用由 DTLS 实现决定
}
bool validExtmapId(unsigned id) { return (id >= 1 && id <= 256) || (id >= 4096 && id <= 4351); }
bool commonName(std::string_view name)
{
    return name == "sendrecv" || name == "sendonly" || name == "recvonly" || name == "inactive" ||
        name == "ice-ufrag" || name == "ice-pwd" || name == "ice-options" || name == "fingerprint" ||
        name == "setup" || name == "extmap" || name == "extmap-allow-mixed";
}
bool reserved(std::string_view name)
{
    return commonName(name) || name == "group" || name == "ice-lite" || name == "mid" || name == "bundle-only" ||
        name == "rtcp" || name == "rtcp-mux" || name == "rtcp-mux-only" || name == "rtcp-rsize" ||
        name == "rtpmap" || name == "fmtp" || name == "rtcp-fb" || name == "candidate" ||
        name == "end-of-candidates" || name == "sctp-port" || name == "max-message-size";
}
bool validCommon(const SdpCommonAttributes& common, std::string* reason)
{
    if (common.direction && !directionString(*common.direction)) return fail(reason, "Invalid media direction");
    if (common.iceUfrag && !iceString(*common.iceUfrag, 4)) return fail(reason, "Invalid ICE ufrag (4~256 ICE characters)");
    if (common.icePwd && !iceString(*common.icePwd, 22)) return fail(reason, "Invalid ICE password (22~256 ICE characters)");
    if (common.setup && !setupRoleString(*common.setup)) return fail(reason, "Invalid DTLS setup role");
    std::set<std::string> algorithms;
    for (const auto& fingerprint : common.fingerprints)
    {
        if (!validFingerprint(fingerprint)) return fail(reason, "Invalid DTLS fingerprint");
        auto algorithm = fingerprint.algorithm;
        std::transform(algorithm.begin(), algorithm.end(), algorithm.begin(), lower);
        auto value = fingerprint.value;
        std::transform(value.begin(), value.end(), value.begin(), lower);
        if (!algorithms.insert(algorithm + ":" + value).second) return fail(reason, "Duplicate DTLS fingerprint");
    }
    for (const auto& option : common.iceOptions) if (!token(option)) return fail(reason, "Invalid ICE option");
    std::set<unsigned> ids;
    for (const auto& extmap : common.extmaps)
    {
        if (!validExtmapId(extmap.id) || (extmap.direction && !directionString(*extmap.direction)) ||
            !word(extmap.uri) || (extmap.attributes && !textValue(*extmap.attributes))) return fail(reason, "Invalid RTP extmap");
        if (extmap.id < 4096 && !ids.insert(extmap.id).second) return fail(reason, "Duplicate RTP extension ID");
    }
    if (common.attributes.size() > kMaxLines) return fail(reason, "Too many SDP attributes");
    for (const auto& attribute : common.attributes)
        if (!token(attribute.name) || reserved(attribute.name) || (attribute.value && !textValue(*attribute.value, true)))
            return fail(reason, "Invalid or reserved opaque SDP attribute");
    return true;
}
bool parseCommon(const SdpAttribute& attribute, SdpCommonAttributes& common, std::string* reason)
{
    const auto& name = attribute.name;
    const auto value = attribute.value ? std::string_view(*attribute.value) : std::string_view{};
    SdpDirection direction;
    if (parseDirection(name, direction))
    {
        if (attribute.value || common.direction) return fail(reason, "Duplicate direction or unexpected direction value");
        common.direction = direction;
    }
    else if (name == "ice-ufrag" || name == "ice-pwd")
    {
        auto& destination = name == "ice-ufrag" ? common.iceUfrag : common.icePwd;
        if (!attribute.value || value.empty() || destination) return fail(reason, "Missing or duplicate ICE credential");
        destination = value;
    }
    else if (name == "ice-options")
    {
        const auto parts = words(value);
        if (!attribute.value || parts.empty() || !common.iceOptions.empty()) return fail(reason, "Missing or duplicate ICE options");
        for (auto item : parts) common.iceOptions.emplace_back(item);
    }
    else if (name == "fingerprint")
    {
        const auto parts = words(value);
        if (parts.size() != 2) return fail(reason, "Invalid fingerprint attribute");
        common.fingerprints.push_back({std::string(parts[0]), std::string(parts[1])});
    }
    else if (name == "setup")
    {
        SdpSetupRole role;
        if (!parseSetup(value, role) || common.setup) return fail(reason, "Invalid or duplicate DTLS setup attribute");
        common.setup = role;
    }
    else if (name == "extmap-allow-mixed")
    {
        if (attribute.value || common.extmapAllowMixed) return fail(reason, "Invalid or duplicate extmap-allow-mixed");
        common.extmapAllowMixed = true;
    }
    else if (name == "extmap")
    {
        const auto parts = words(value);
        if (parts.size() < 2) return fail(reason, "Invalid extmap attribute");
        const auto id = split(parts[0], '/');
        std::uint64_t numeric;
        if (id.size() > 2 || !number(id[0], 4351, numeric)) return fail(reason, "Invalid extmap ID");
        SdpExtMap extmap;
        extmap.id = static_cast<unsigned>(numeric);
        if (id.size() == 2)
        {
            if (!parseDirection(id[1], direction)) return fail(reason, "Invalid extmap direction");
            extmap.direction = direction;
        }
        extmap.uri = parts[1];
        if (parts.size() > 2) extmap.attributes = std::string(value.substr(static_cast<std::size_t>(parts[2].data() - value.data())));
        common.extmaps.push_back(std::move(extmap));
    }
    else return fail(reason, "Unknown common attribute");
    return true;
}
SdpCodec& ensureCodec(SdpMedia& section, unsigned pt)
{
    for (auto& codec : section.codecs) if (codec.payloadType == pt) return codec;
    section.codecs.push_back({pt, {}, 0, 1, std::nullopt});
    return section.codecs.back();
}
bool parseMediaAttribute(const SdpAttribute& attribute, SdpMedia& section, std::string* reason)
{
    const auto& name = attribute.name;
    const auto value = attribute.value ? std::string_view(*attribute.value) : std::string_view{};
    if (commonName(name)) return parseCommon(attribute, section.common, reason);
    if (name == "mid")
    {
        if (!token(value) || section.mid) return fail(reason, "Invalid or duplicate MID");
        section.mid = value;
    }
    else if (name == "rtcp-mux" || name == "rtcp-mux-only" || name == "rtcp-rsize" || name == "bundle-only" || name == "end-of-candidates")
    {
        bool& flag = name == "rtcp-mux" ? section.rtcpMux : name == "rtcp-mux-only" ? section.rtcpMuxOnly :
                     name == "rtcp-rsize" ? section.rtcpRsize : name == "bundle-only" ? section.bundleOnly : section.endOfCandidates;
        if (attribute.value || flag) return fail(reason, "Invalid or duplicate flag attribute");
        flag = true;
    }
    else if (name == "rtcp")
    {
        const auto parts = words(value);
        std::uint64_t port;
        if ((parts.size() != 1 && parts.size() != 4) || !number(parts[0], 65535, port) || port == 0 || section.rtcp)
            return fail(reason, "Invalid or duplicate RTCP address");
        SdpRtcp rtcp;
        rtcp.port = static_cast<std::uint16_t>(port);
        if (parts.size() == 4)
        {
            SdpConnection connection;
            if (!parseConnection(parts, 1, connection)) return fail(reason, "Invalid RTCP connection");
            rtcp.connection = std::move(connection);
        }
        section.rtcp = std::move(rtcp);
    }
    else if (name == "rtpmap" || name == "fmtp" || name == "rtcp-fb")
    {
        const auto at = value.find_first_of(" \t");
        if (at == std::string_view::npos) return fail(reason, "Missing RTP attribute value");
        const auto pt = payload(value.substr(0, at));
        const auto data = trim(value.substr(at + 1));
        if (!textValue(data)) return fail(reason, "Empty RTP attribute value");
        if (name == "rtcp-fb")
        {
            if (!pt && value.substr(0, at) != "*") return fail(reason, "Invalid RTCP feedback payload");
            section.feedback.push_back({pt, std::string(data)});
        }
        else
        {
            if (!pt) return fail(reason, "Invalid RTP payload type");
            auto& codec = ensureCodec(section, *pt);
            if (name == "fmtp")
            {
                if (codec.parameters) return fail(reason, "Duplicate fmtp");
                codec.parameters = data;
            }
            else
            {
                const auto mapping = split(data, '/');
                std::uint64_t rate, channels = 1;
                if ((mapping.size() != 2 && mapping.size() != 3) || !token(mapping[0]) ||
                    !number(mapping[1], 0xFFFFFFFFu, rate) || rate == 0 ||
                    (mapping.size() == 3 && (!number(mapping[2], 65535, channels) || channels == 0)) || !codec.encoding.empty())
                    return fail(reason, "Invalid or duplicate rtpmap");
                codec.encoding = mapping[0]; codec.clockRate = static_cast<std::uint32_t>(rate); codec.channels = static_cast<unsigned>(channels);
            }
        }
    }
    else if (name == "candidate")
    {
        IceCandidate candidate;
        if (!IceCandidate::parse("candidate:" + std::string(value), candidate, reason)) return false;
        section.candidates.push_back(std::move(candidate));
    }
    else if (name == "sctp-port" || name == "max-message-size")
    {
        std::uint64_t numeric;
        if (!number(value, name == "sctp-port" ? 65535 : std::numeric_limits<std::uint64_t>::max(), numeric) ||
            (value.size() > 1 && value.front() == '0')) return fail(reason, "Invalid SCTP numeric attribute");
        if (name == "sctp-port")
        {
            if (numeric == 0 || section.sctpPort) return fail(reason, "Invalid or duplicate SCTP port");
            section.sctpPort = static_cast<std::uint16_t>(numeric);
        }
        else
        {
            if (section.maxMessageSize) return fail(reason, "Duplicate SCTP max-message-size");
            section.maxMessageSize = numeric;
        }
    }
    else
    {
        if (reserved(name)) return fail(reason, "Session attribute in media scope");
        section.common.attributes.push_back(attribute);
    }
    return true;
}

struct Writer
{
    std::string text;
    std::size_t lines{0};
    void add(char type, const std::string& value)
    {
        if (!textValue(value, true) || value.size() + 2 > kMaxLineBytes || ++lines > kMaxLines || text.size() + value.size() + 4 > kMaxBytes)
            throw std::invalid_argument("SDP exceeds size/line limit or contains invalid characters");
        text += type; text += '='; text += value; text += "\r\n";
    }
    void attribute(const std::string& name, const std::optional<std::string>& value = std::nullopt)
    {
        add('a', name + (value ? ":" + *value : ""));
    }
};
std::string connectionText(const SdpConnection& connection) { return "IN " + connection.addressType + " " + connection.address; }
void writeCommon(Writer& writer, const SdpCommonAttributes& common)
{
    if (common.direction) writer.attribute(directionString(*common.direction));
    if (common.iceUfrag) writer.attribute("ice-ufrag", common.iceUfrag);
    if (common.icePwd) writer.attribute("ice-pwd", common.icePwd);
    if (!common.iceOptions.empty())
    {
        std::string options;
        for (const auto& option : common.iceOptions) { if (!options.empty()) options += ' '; options += option; }
        writer.attribute("ice-options", options);
    }
    for (const auto& fingerprint : common.fingerprints)
    {
        auto value = fingerprint.value;
        for (auto& c : value) if (c >= 'a' && c <= 'f') c = static_cast<char>(c - ('a' - 'A'));
        writer.attribute("fingerprint", fingerprint.algorithm + " " + value);
    }
    if (common.setup) writer.attribute("setup", setupRoleString(*common.setup));
    if (common.extmapAllowMixed) writer.attribute("extmap-allow-mixed");
    for (const auto& extmap : common.extmaps)
        writer.attribute("extmap", std::to_string(extmap.id) + (extmap.direction ? std::string("/") + directionString(*extmap.direction) : "") +
                         " " + extmap.uri + (extmap.attributes ? " " + *extmap.attributes : ""));
    for (const auto& attribute : common.attributes) writer.attribute(attribute.name, attribute.value);
}
void writeBandwidth(Writer& writer, const std::vector<SdpBandwidth>& entries)
{
    for (const auto& item : entries) writer.add('b', item.type + ":" + std::to_string(item.value));
}
std::string serialize(const SdpSession& session)
{
    Writer writer;
    writer.add('v', "0");
    writer.add('o', session.origin.username + " " + std::to_string(session.origin.sessionId) + " " +
                    std::to_string(session.origin.sessionVersion) + " " + connectionText(session.origin.connection));
    writer.add('s', session.sessionName);
    if (session.information) writer.add('i', *session.information);
    if (session.uri) writer.add('u', *session.uri);
    for (const auto& email : session.emails) writer.add('e', email);
    for (const auto& phone : session.phones) writer.add('p', phone);
    if (session.connection) writer.add('c', connectionText(*session.connection));
    writeBandwidth(writer, session.bandwidth);
    for (const auto& timing : session.timings)
    {
        writer.add('t', std::to_string(timing.start) + " " + std::to_string(timing.stop));
        for (const auto& repeat : timing.repeats) writer.add('r', repeat);
    }
    if (session.timeZones) writer.add('z', *session.timeZones);
    if (session.key) writer.add('k', *session.key);
    for (const auto& group : session.groups)
    {
        std::string value = group.semantics;
        for (const auto& mid : group.mids) value += " " + mid;
        writer.attribute("group", value);
    }
    if (session.iceLite) writer.attribute("ice-lite");
    writeCommon(writer, session.common);
    for (const auto& section : session.media)
    {
        std::string m = section.kind + " " + std::to_string(section.port) + (section.portCount ? "/" + std::to_string(*section.portCount) : "") + " " + section.protocol;
        for (const auto& format : section.formats) m += " " + format;
        writer.add('m', m);
        if (section.information) writer.add('i', *section.information);
        if (section.connection) writer.add('c', connectionText(*section.connection));
        writeBandwidth(writer, section.bandwidth);
        if (section.key) writer.add('k', *section.key);
        if (section.mid) writer.attribute("mid", section.mid);
        if (section.bundleOnly) writer.attribute("bundle-only");
        writeCommon(writer, section.common);
        if (section.rtcp) writer.attribute("rtcp", std::to_string(section.rtcp->port) + (section.rtcp->connection ? " " + connectionText(*section.rtcp->connection) : ""));
        if (section.rtcpMux) writer.attribute("rtcp-mux");
        if (section.rtcpMuxOnly) writer.attribute("rtcp-mux-only");
        if (section.rtcpRsize) writer.attribute("rtcp-rsize");
        for (const auto& codec : section.codecs)
        {
            const auto pt = std::to_string(codec.payloadType);
            if (!codec.encoding.empty()) writer.attribute("rtpmap", pt + " " + codec.encoding + "/" + std::to_string(codec.clockRate) + (codec.channels != 1 ? "/" + std::to_string(codec.channels) : ""));
            if (codec.parameters) writer.attribute("fmtp", pt + " " + *codec.parameters);
        }
        for (const auto& feedback : section.feedback) writer.attribute("rtcp-fb", (feedback.payloadType ? std::to_string(*feedback.payloadType) : "*") + " " + feedback.feedback);
        if (section.sctpPort) writer.attribute("sctp-port", std::to_string(*section.sctpPort));
        if (section.maxMessageSize) writer.attribute("max-message-size", std::to_string(*section.maxMessageSize));
        for (const auto& candidate : section.candidates) writer.add('a', candidate.toCandidateString());
        if (section.endOfCandidates) writer.attribute("end-of-candidates");
    }
    return writer.text;
}
} // namespace

const char* directionString(SdpDirection direction)
{
    switch (direction)
    {
    case SdpDirection::SendRecv: return "sendrecv";
    case SdpDirection::SendOnly: return "sendonly";
    case SdpDirection::RecvOnly: return "recvonly";
    case SdpDirection::Inactive: return "inactive";
    }
    return nullptr;
}
const char* setupRoleString(SdpSetupRole role)
{
    switch (role)
    {
    case SdpSetupRole::ActPass: return "actpass";
    case SdpSetupRole::Active: return "active";
    case SdpSetupRole::Passive: return "passive";
    case SdpSetupRole::HoldConn: return "holdconn";
    }
    return nullptr;
}
SdpDirection answerDirection(SdpDirection offered, bool wantSend, bool wantReceive)
{
    if (!directionString(offered)) throw std::invalid_argument("Invalid offered SDP direction");
    const bool send = wantSend && (offered == SdpDirection::SendRecv || offered == SdpDirection::RecvOnly);
    const bool receive = wantReceive && (offered == SdpDirection::SendRecv || offered == SdpDirection::SendOnly);
    return send ? (receive ? SdpDirection::SendRecv : SdpDirection::SendOnly) : (receive ? SdpDirection::RecvOnly : SdpDirection::Inactive);
}
SdpSetupRole answerSetupRole(SdpSetupRole offered, bool preferActive)
{
    switch (offered)
    {
    case SdpSetupRole::ActPass: return preferActive ? SdpSetupRole::Active : SdpSetupRole::Passive;
    case SdpSetupRole::Active: return SdpSetupRole::Passive;
    case SdpSetupRole::Passive: return SdpSetupRole::Active;
    default: throw std::invalid_argument("Unsupported offered DTLS setup role");
    }
}
std::optional<std::string> SdpCodec::parameter(std::string_view name) const
{
    if (parameters)
        for (auto part : split(*parameters, ';'))
        {
            const auto at = part.find('=');
            if (at != std::string_view::npos && equal(trim(part.substr(0, at)), name)) return std::string(trim(part.substr(at + 1)));
        }
    return std::nullopt;
}
bool SdpMedia::isRtp() const
{
    for (auto value : {"RTP/AVP", "RTP/AVPF", "RTP/SAVP", "RTP/SAVPF", "UDP/TLS/RTP/SAVP", "UDP/TLS/RTP/SAVPF", "TCP/TLS/RTP/SAVP", "TCP/TLS/RTP/SAVPF"})
        if (equal(protocol, value)) return true;
    return false;
}
bool SdpMedia::isSctp() const { return equal(protocol, "UDP/DTLS/SCTP") || equal(protocol, "TCP/DTLS/SCTP") || equal(protocol, "DTLS/SCTP"); }
bool SdpMedia::isRejected() const { return port == 0 && !bundleOnly; }
const SdpCodec* SdpMedia::codec(unsigned pt) const
{
    for (const auto& item : codecs) if (item.payloadType == pt) return &item;
    return nullptr;
}
std::vector<const SdpCodec*> SdpMedia::codecsNamed(std::string_view encoding) const
{
    std::vector<const SdpCodec*> result;
    for (const auto& item : codecs) if (equal(item.encoding, encoding)) result.push_back(&item);
    return result;
}
std::optional<unsigned> SdpMedia::rtxPayloadType(unsigned primary) const
{
    for (const auto& item : codecs)
        if (equal(item.encoding, "rtx"))
        {
            const auto apt = item.parameter("apt");
            if (apt && payload(*apt) == primary) return item.payloadType;
        }
    return std::nullopt;
}
const SdpAttribute* SdpMedia::attribute(std::string_view name) const
{
    for (const auto& item : common.attributes) if (item.name == name) return &item;
    return nullptr;
}
const SdpMedia* SdpSession::findMedia(std::string_view mid) const
{
    for (const auto& section : media) if (section.mid && *section.mid == mid) return &section;
    return nullptr;
}
SdpMedia* SdpSession::findMedia(std::string_view mid)
{
    for (auto& section : media) if (section.mid && *section.mid == mid) return &section;
    return nullptr;
}
const SdpGroup* SdpSession::bundleGroup(std::string_view mid) const
{
    for (const auto& group : groups)
        if (equal(group.semantics, "BUNDLE") && std::find(group.mids.begin(), group.mids.end(), mid) != group.mids.end()) return &group;
    return nullptr;
}
const SdpMedia& SdpSession::transportMedia(const SdpMedia& section, SdpDescriptionType type) const
{
    if (section.mid && (section.bundleOnly || type == SdpDescriptionType::Answer))
        if (const auto* group = bundleGroup(*section.mid))
            if (!group->mids.empty())
                if (const auto* tag = findMedia(group->mids.front())) return *tag;
    return section;
}
SdpCommonAttributes SdpSession::effectiveAttributes(const SdpMedia& section) const
{
    auto result = section.common;
    if (!result.direction) result.direction = common.direction;
    if (!result.iceUfrag) result.iceUfrag = common.iceUfrag;
    if (!result.icePwd) result.icePwd = common.icePwd;
    if (result.iceOptions.empty()) result.iceOptions = common.iceOptions;
    if (result.fingerprints.empty()) result.fingerprints = common.fingerprints;
    if (!result.setup) result.setup = common.setup;
    if (result.extmaps.empty()) result.extmaps = common.extmaps;
    result.extmapAllowMixed = result.extmapAllowMixed || common.extmapAllowMixed;
    return result;
}
SdpDirection SdpSession::effectiveDirection(const SdpMedia& section) const
{
    return section.common.direction.value_or(common.direction.value_or(SdpDirection::SendRecv));
}
std::optional<SdpConnection> SdpSession::effectiveConnection(const SdpMedia& section) const
{
    return section.connection ? section.connection : connection;
}
std::string SdpSession::toSdp() const
{
    std::string reason;
    if (!validate(&reason)) throw std::invalid_argument(reason);
    return serialize(*this);
}

namespace
{
bool validBandwidth(const std::vector<SdpBandwidth>& bandwidth, std::string* reason)
{
    std::set<std::string> types;
    for (const auto& entry : bandwidth)
        if (!token(entry.type) || !types.insert(entry.type).second) return fail(reason, "Invalid or duplicate bandwidth type");
    return true;
}
bool validMedia(const SdpMedia& section, std::string* reason)
{
    if (!token(section.kind) || section.formats.empty() || section.formats.size() > 128 ||
        (section.portCount && (*section.portCount == 0 || *section.portCount > 65535))) return fail(reason, "Invalid media description");
    for (auto part : split(section.protocol, '/')) if (!token(part)) return fail(reason, "Invalid media protocol");
    if ((section.mid && !token(*section.mid)) || (section.connection && !validConnection(*section.connection)) ||
        (section.information && !textValue(*section.information, true)) || (section.key && !textValue(*section.key)))
        return fail(reason, "Invalid media MID/connection/text");
    if (!validCommon(section.common, reason) || !validBandwidth(section.bandwidth, reason)) return false;
    std::set<std::string> formats;
    std::set<unsigned> payloads;
    for (const auto& format : section.formats)
    {
        if (!token(format) || !formats.insert(format).second) return fail(reason, "Invalid or duplicate media format");
        if (section.isRtp())
        {
            const auto pt = payload(format);
            if (!pt || !payloads.insert(*pt).second) return fail(reason, "Invalid or duplicate RTP payload type");
        }
    }
    if (section.rtcpMuxOnly && !section.rtcpMux) return fail(reason, "rtcp-mux-only requires rtcp-mux");
    if (section.rtcp && (section.rtcp->port == 0 || (section.rtcp->connection && !validConnection(*section.rtcp->connection))))
        return fail(reason, "Invalid RTCP endpoint");
    if (!section.isRtp() && (!section.codecs.empty() || !section.feedback.empty()))
        return fail(reason, "RTP attributes on non-RTP media");
    if (!section.isSctp() && (section.sctpPort || section.maxMessageSize)) return fail(reason, "SCTP attributes on non-SCTP media");
    if (section.sctpPort && *section.sctpPort == 0) return fail(reason, "Invalid SCTP port");
    std::set<unsigned> codecs;
    if (section.codecs.size() > 128 || section.feedback.size() > kMaxLines || section.candidates.size() > kMaxLines)
        return fail(reason, "Too many media attributes");
    for (const auto& codec : section.codecs)
    {
        if (codec.payloadType > 127 || !payloads.count(codec.payloadType) || !codecs.insert(codec.payloadType).second ||
            codec.channels == 0 || codec.channels > 65535 || (codec.parameters && !textValue(*codec.parameters)))
            return fail(reason, "Invalid codec or payload not advertised by m=");
        if (codec.encoding.empty())
        {
            if (codec.clockRate != 0 || codec.channels != 1 || codec.payloadType >= 96) return fail(reason, "Missing rtpmap for codec");
        }
        else if (!token(codec.encoding) || codec.clockRate == 0) return fail(reason, "Invalid RTP codec mapping");
        if (equal(codec.encoding, "rtx"))
        {
            const auto apt = codec.parameter("apt");
            const auto target = apt ? payload(*apt) : std::nullopt;
            unsigned count = 0;
            if (codec.parameters)
                for (auto part : split(*codec.parameters, ';'))
                    if (equal(trim(part.substr(0, part.find('='))), "apt")) ++count;
            if (!target || count != 1 || *target == codec.payloadType || !payloads.count(*target)) return fail(reason, "Invalid RTX apt reference");
            const auto* primary = section.codec(*target);
            if (primary && (equal(primary->encoding, "rtx") || (primary->clockRate != 0 && primary->clockRate != codec.clockRate)))
                return fail(reason, "RTX must reference a primary codec with the same clock rate");
        }
    }
    if (section.isRtp() && !section.isRejected())
        for (auto pt : payloads)
            if (pt >= 96 && (!section.codec(pt) || section.codec(pt)->encoding.empty())) return fail(reason, "Dynamic RTP format requires rtpmap");
    for (const auto& feedback : section.feedback)
        if (!textValue(feedback.feedback) || (feedback.payloadType && !payloads.count(*feedback.payloadType)))
            return fail(reason, "Invalid RTCP feedback or unadvertised payload");
    for (const auto& candidate : section.candidates) if (!candidate.validate(reason)) return false;
    return true;
}
int sessionRank(char field)
{
    switch (field)
    {
    case 'v': return 0; case 'o': return 1; case 's': return 2; case 'i': return 3; case 'u': return 4;
    case 'e': return 5; case 'p': return 6; case 'c': return 7; case 'b': return 8;
    case 't': case 'r': case 'z': return 9; case 'k': return 10; case 'a': return 11;
    default: return -1;
    }
}
int mediaRank(char field)
{
    switch (field) { case 'i': return 1; case 'c': return 2; case 'b': return 3; case 'k': return 4; case 'a': return 5; default: return -1; }
}
bool parseBandwidth(std::string_view value, std::vector<SdpBandwidth>& entries, std::string* reason)
{
    const auto colon = value.find(':');
    std::uint64_t numeric;
    if (colon == std::string_view::npos || !token(value.substr(0, colon)) || !number(value.substr(colon + 1), std::numeric_limits<std::uint64_t>::max(), numeric))
        return fail(reason, "Invalid bandwidth field");
    entries.push_back({std::string(value.substr(0, colon)), numeric});
    return true;
}
struct ParseState
{
    SdpSession session;
    SdpMedia* section{nullptr};
    bool version{false}, origin{false}, name{false};
    int rank{-1};
};
bool parseLine(char field, std::string_view value, ParseState& state, std::string* reason)
{
    auto& session = state.session;
    if ((!state.version && field != 'v') || (state.version && !state.origin && field != 'o') ||
        (state.origin && !state.name && field != 's')) return fail(reason, "Expected v=, o=, s= in order");
    if (field == 'm')
    {
        if (session.timings.empty() || session.media.size() >= kMaxMedia) return fail(reason, "Missing timing or too many media sections");
        const auto parts = words(value);
        if (parts.size() < 4) return fail(reason, "Incomplete media description");
        const auto port = split(parts[1], '/');
        std::uint64_t numeric;
        if (port.size() > 2 || !number(port[0], 65535, numeric)) return fail(reason, "Invalid media port");
        SdpMedia section;
        section.kind = parts[0]; section.port = static_cast<std::uint16_t>(numeric); section.protocol = parts[2];
        if (port.size() == 2)
        {
            if (!number(port[1], 65535, numeric) || numeric == 0) return fail(reason, "Invalid media port count");
            section.portCount = static_cast<unsigned>(numeric);
        }
        for (std::size_t i = 3; i < parts.size(); ++i) section.formats.emplace_back(parts[i]);
        session.media.push_back(std::move(section));
        state.section = &session.media.back(); state.rank = 0;
        return true;
    }
    const int rank = state.section ? mediaRank(field) : sessionRank(field);
    if (rank < 0 || rank < state.rank) return fail(reason, "Unknown field or invalid SDP field order/scope");
    state.rank = rank;
    if (!state.section)
    {
        if (field == 'v')
        {
            if (value != "0" || state.version) return fail(reason, "Unsupported or duplicate SDP version");
            state.version = true;
        }
        else if (field == 'o')
        {
            const auto parts = words(value);
            if (state.origin || parts.size() != 6 || !word(parts[0]) ||
                !number(parts[1], std::numeric_limits<std::uint64_t>::max(), session.origin.sessionId) ||
                !number(parts[2], std::numeric_limits<std::uint64_t>::max(), session.origin.sessionVersion) ||
                !parseConnection(parts, 3, session.origin.connection)) return fail(reason, "Invalid or duplicate origin");
            session.origin.username = parts[0]; state.origin = true;
        }
        else if (field == 's')
        {
            if (state.name || !textValue(value)) return fail(reason, "Invalid or duplicate session name");
            session.sessionName = value; state.name = true;
        }
        else if (field == 'i' || field == 'u' || field == 'k' || field == 'z')
        {
            auto& destination = field == 'i' ? session.information : field == 'u' ? session.uri : field == 'k' ? session.key : session.timeZones;
            if (destination || !textValue(value, field == 'i')) return fail(reason, "Invalid or duplicate session field");
            if ((field == 'k' || field == 'z') && session.timings.empty()) return fail(reason, "Missing t= before session field");
            destination = value;
        }
        else if (field == 'e' || field == 'p')
            (field == 'e' ? session.emails : session.phones).emplace_back(value);
        else if (field == 'c')
        {
            SdpConnection connection;
            if (session.connection || !parseConnection(words(value), 0, connection)) return fail(reason, "Invalid or duplicate connection");
            session.connection = std::move(connection);
        }
        else if (field == 'b') return parseBandwidth(value, session.bandwidth, reason);
        else if (field == 't')
        {
            const auto parts = words(value);
            SdpTiming timing;
            if (parts.size() != 2 || !number(parts[0], std::numeric_limits<std::uint64_t>::max(), timing.start) ||
                !number(parts[1], std::numeric_limits<std::uint64_t>::max(), timing.stop)) return fail(reason, "Invalid timing field");
            session.timings.push_back(std::move(timing));
        }
        else if (field == 'r')
        {
            if (session.timings.empty() || !textValue(value)) return fail(reason, "Repeat field without timing");
            session.timings.back().repeats.emplace_back(value);
        }
    }
    else
    {
        auto& section = *state.section;
        if (field == 'i' || field == 'k')
        {
            auto& destination = field == 'i' ? section.information : section.key;
            if (destination || !textValue(value, field == 'i')) return fail(reason, "Invalid or duplicate media field");
            destination = value;
        }
        else if (field == 'c')
        {
            SdpConnection connection;
            if (section.connection || !parseConnection(words(value), 0, connection)) return fail(reason, "Invalid or duplicate media connection");
            section.connection = std::move(connection);
        }
        else if (field == 'b') return parseBandwidth(value, section.bandwidth, reason);
    }
    if (field == 'a')
    {
        if (session.timings.empty()) return fail(reason, "Missing timing before attributes");
        const auto colon = value.find(':');
        SdpAttribute attribute{std::string(value.substr(0, colon)), std::nullopt};
        if (colon != std::string_view::npos) attribute.value = value.substr(colon + 1);
        if (!token(attribute.name)) return fail(reason, "Invalid attribute name");
        if (state.section) return parseMediaAttribute(attribute, *state.section, reason);
        if (commonName(attribute.name)) return parseCommon(attribute, session.common, reason);
        if (attribute.name == "group")
        {
            const auto parts = words(attribute.value ? std::string_view(*attribute.value) : std::string_view{});
            if (parts.size() < 2) return fail(reason, "Invalid SDP group");
            SdpGroup group; group.semantics = parts[0];
            for (std::size_t i = 1; i < parts.size(); ++i) group.mids.emplace_back(parts[i]);
            session.groups.push_back(std::move(group));
        }
        else if (attribute.name == "ice-lite")
        {
            if (attribute.value || session.iceLite) return fail(reason, "Invalid or duplicate ice-lite");
            session.iceLite = true;
        }
        else
        {
            if (reserved(attribute.name)) return fail(reason, "Media attribute in session scope");
            session.common.attributes.push_back(std::move(attribute));
        }
    }
    return true;
}
} // namespace

bool SdpSession::parse(std::string_view text, SdpSession& out, std::string* reason)
{
    if (text.empty() || text.size() > kMaxBytes) return fail(reason, "Empty SDP or SDP exceeds 256KiB");
    ParseState state;
    state.session.timings.clear();
    std::size_t offset = 0, lineNumber = 0;
    while (offset < text.size())
    {
        const auto end = text.find('\n', offset);
        auto line = text.substr(offset, end == std::string_view::npos ? text.size() - offset : end - offset);
        offset = end == std::string_view::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r')
        {
            if (end == std::string_view::npos) return fail(reason, "Bare CR at end of SDP");
            line.remove_suffix(1);
        }
        ++lineNumber;
        if (lineNumber > kMaxLines || line.size() > kMaxLineBytes || line.size() < 2 || line[1] != '=' || !textValue(line))
            return fail(reason, "Line " + std::to_string(lineNumber) + ": invalid field, character or line limit");
        std::string error;
        if (!parseLine(line[0], line.substr(2), state, &error)) return fail(reason, "Line " + std::to_string(lineNumber) + ": " + error);
    }
    if (!state.version || !state.origin || !state.name || state.session.timings.empty()) return fail(reason, "Missing mandatory v=/o=/s=/t= fields");
    if (!state.session.validate(reason)) return false;
    out = std::move(state.session);
    if (reason) reason->clear();
    return true;
}

bool SdpSession::validate(std::string* reason) const
{
    if (!word(origin.username) || !validConnection(origin.connection) || !textValue(sessionName) || timings.empty() || media.size() > kMaxMedia)
        return fail(reason, "Invalid SDP origin/session name/timing or too many media sections");
    if ((information && !textValue(*information, true)) || (uri && !word(*uri)) || (key && !textValue(*key)) ||
        (timeZones && !textValue(*timeZones)) || (connection && !validConnection(*connection))) return fail(reason, "Invalid session field");
    for (const auto& value : emails) if (!textValue(value)) return fail(reason, "Invalid email field");
    for (const auto& value : phones) if (!textValue(value)) return fail(reason, "Invalid phone field");
    for (const auto& timing : timings)
    {
        if (timing.stop != 0 && timing.stop < timing.start) return fail(reason, "Timing stop precedes start");
        for (const auto& repeat : timing.repeats) if (!textValue(repeat)) return fail(reason, "Invalid timing repeat");
    }
    if (!validCommon(common, reason) || !validBandwidth(bandwidth, reason)) return false;
    std::set<std::string> mids, bundled;
    for (const auto& section : media)
    {
        if (!validMedia(section, reason)) return false;
        if (!effectiveConnection(section)) return fail(reason, "Missing session/media connection");
        if (section.mid && !mids.insert(*section.mid).second) return fail(reason, "Duplicate media MID");
        if (!common.extmaps.empty() && !section.common.extmaps.empty()) return fail(reason, "Cannot mix session-level and media-level extmap");
    }
    for (const auto& group : groups)
    {
        if (!token(group.semantics) || group.mids.empty()) return fail(reason, "Invalid SDP group");
        std::set<std::string> members;
        for (const auto& mid : group.mids)
        {
            if (!token(mid) || !mids.count(mid) || !members.insert(mid).second) return fail(reason, "Group references unknown or duplicate MID");
            if (equal(group.semantics, "BUNDLE") && !bundled.insert(mid).second) return fail(reason, "MID in multiple BUNDLE groups");
        }
    }
    for (const auto& section : media)
    {
        if (section.bundleOnly)
        {
            const auto* group = section.mid ? bundleGroup(*section.mid) : nullptr;
            if (section.port != 0 || !group || group->mids.front() == *section.mid) return fail(reason, "Invalid bundle-only media");
        }
        if (!section.isRtp() && (section.rtcp || section.rtcpMux || section.rtcpMuxOnly || section.rtcpRsize))
        {
            const auto* group = section.mid ? bundleGroup(*section.mid) : nullptr;
            bool hasRtp = false;
            if (group && group->mids.front() == *section.mid)
                for (const auto& mid : group->mids) hasRtp = hasRtp || findMedia(mid)->isRtp();
            if (!hasRtp) return fail(reason, "RTCP attributes require RTP media or its BUNDLE transport tag");
        }
    }
    try { (void)serialize(*this); }
    catch (const std::invalid_argument& error) { return fail(reason, error.what()); }
    if (reason) reason->clear();
    return true;
}
bool SdpSession::validateWebRtc(SdpDescriptionType type, std::string* reason) const
{
    if (type != SdpDescriptionType::Offer && type != SdpDescriptionType::Answer) return fail(reason, "Invalid SDP description type");
    if (!validate(reason)) return false;
    if (timings.size() != 1 || timings[0].start != 0 || timings[0].stop != 0 || !timings[0].repeats.empty() || timeZones)
        return fail(reason, "WebRTC SDP requires t=0 0");
    for (const auto& group : groups)
        if (equal(group.semantics, "BUNDLE"))
            for (const auto& mid : group.mids)
            {
                if (findMedia(mid)->isRejected() || (mid == group.mids.front() && findMedia(mid)->bundleOnly))
                    return fail(reason, "Rejected media or bundle-only tag in BUNDLE group");
                if (effectiveConnection(*findMedia(mid))->addressType != effectiveConnection(*findMedia(group.mids.front()))->addressType)
                    return fail(reason, "BUNDLE media must use the same connection address family");
            }
    for (const auto& section : media)
    {
        if (!section.mid) return fail(reason, "WebRTC media requires MID");
        if (section.isRejected()) continue;
        if (section.bundleOnly && type == SdpDescriptionType::Answer) return fail(reason, "bundle-only must not appear in an answer");
        if (section.portCount && *section.portCount != 1) return fail(reason, "WebRTC media uses one port");
        const auto& transport = transportMedia(section, type);
        const auto effective = effectiveAttributes(transport);
        if (!effective.iceUfrag || !effective.icePwd || effective.fingerprints.empty() || !effective.setup)
            return fail(reason, "WebRTC media requires ICE credentials, DTLS fingerprint and setup role");
        if ((type == SdpDescriptionType::Offer && *effective.setup != SdpSetupRole::ActPass) ||
            (type == SdpDescriptionType::Answer && *effective.setup != SdpSetupRole::Active && *effective.setup != SdpSetupRole::Passive))
            return fail(reason, "Invalid DTLS role for initial WebRTC offer/answer");
        if (type == SdpDescriptionType::Answer && &transport != &section)
        {
            // 接受旧式 answer 重复相同传输属性，但拒绝与 tagged 媒体冲突的值。
            if ((section.common.iceUfrag && section.common.iceUfrag != effective.iceUfrag) ||
                (section.common.icePwd && section.common.icePwd != effective.icePwd) ||
                (section.common.setup && section.common.setup != effective.setup))
                return fail(reason, "Conflicting BUNDLE ICE/DTLS transport attributes");
            for (const auto& fingerprint : section.common.fingerprints)
            {
                bool found = false;
                for (const auto& tagged : effective.fingerprints)
                    found = found || (equal(fingerprint.algorithm, tagged.algorithm) && equal(fingerprint.value, tagged.value));
                if (!found) return fail(reason, "Conflicting BUNDLE fingerprint");
            }
        }
        if (section.isRtp())
        {
            if ((section.kind != "audio" && section.kind != "video") ||
                (!equal(section.protocol, "UDP/TLS/RTP/SAVPF") && !equal(section.protocol, "TCP/TLS/RTP/SAVPF")) || !transport.rtcpMux)
                return fail(reason, "WebRTC RTP requires audio/video, DTLS-SRTP/SAVPF and rtcp-mux");
            for (const auto& format : section.formats)
            {
                const auto pt = *payload(format); // validate 已检查所有 RTP formats
                if (pt >= 64 && pt <= 95) return fail(reason, "RTP payload types 64~95 conflict with RTCP mux");
            }
        }
        else if (section.isSctp())
        {
            if (section.kind != "application" || equal(section.protocol, "DTLS/SCTP") || !section.sctpPort ||
                section.formats != std::vector<std::string>{"webrtc-datachannel"}) return fail(reason, "Invalid WebRTC SCTP data channel");
        }
        else return fail(reason, "Unsupported active WebRTC media transport");
    }
    if (reason) reason->clear();
    return true;
}
} // namespace webrtc
