#include"webrtc/rtcp_protocol.h"
#include"common/rtp_packet.h"

#include<algorithm>
#include<set>
#include<limits>
#include<stdexcept>

namespace webrtc
{
namespace
{
std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t u24(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }
std::uint32_t u32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | u24(p + 1); }
void put16(RtcpBytes& b, std::uint16_t v) { b.push_back(v >> 8); b.push_back(v & 255); }
void put24(RtcpBytes& b, std::uint32_t v) { b.push_back((v >> 16) & 255); b.push_back((v >> 8) & 255); b.push_back(v & 255); }
void put32(RtcpBytes& b, std::uint32_t v) { b.push_back(v >> 24); put24(b, v); }
RtcpBytes header(unsigned type, unsigned count, std::uint32_t ssrc)
{
    RtcpBytes b{static_cast<std::uint8_t>(0x80 | count), static_cast<std::uint8_t>(type), 0, 0}; put32(b, ssrc); return b;
}
RtcpBytes finish(RtcpBytes b)
{
    while (b.size() % 4) b.push_back(0);
    if (b.size() > 65507) throw std::invalid_argument("RTCP packet too large");
    const auto length = b.size() / 4 - 1; b[2] = length >> 8; b[3] = length & 255; return b;
}
bool twcc(const std::uint8_t* p, std::size_t size, RtcpTwcc& f)
{
    if (size < 20) return false;
    f.senderSsrc = u32(p + 4); f.mediaSsrc = u32(p + 8); f.baseSequence = u16(p + 12);
    const auto count = u16(p + 14); if (!count || count > 8192) return false;
    f.referenceTime = u24(p + 16); f.feedbackCount = p[19];
    std::vector<unsigned> symbols; symbols.reserve(count); std::size_t pos = 20;
    while (symbols.size() < count)
    {
        if (pos + 2 > size) return false;
        const auto chunk = u16(p + pos); pos += 2;
        if (!(chunk & 0x8000))
        {
            const unsigned symbol = (chunk >> 13) & 3, run = chunk & 0x1fff;
            if (symbol == 3 || !run || run > count - symbols.size()) return false;
            symbols.insert(symbols.end(), run, symbol);
        }
        else
        {
            const bool two = (chunk & 0x4000) != 0;
            for (unsigned i = 0; i < (two ? 7u : 14u) && symbols.size() < count; ++i)
            {
                const unsigned symbol = (chunk >> (two ? 12 - 2 * i : 13 - i)) & (two ? 3 : 1);
                if (symbol == 3) return false;
                symbols.push_back(symbol);
            }
        }
    }
    for (auto symbol : symbols)
    {
        if (!symbol) f.deltas.push_back(std::nullopt);
        else if (symbol == 1) { if (pos == size) return false; f.deltas.push_back(p[pos++]); }
        else
        {
            if (pos + 2 > size) return false;
            const auto value = u16(p + pos); pos += 2;
            f.deltas.push_back(value < 32768 ? std::int32_t(value) : std::int32_t(value) - 65536);
        }
    }
    if (size - pos > 3) return false;
    return std::all_of(p + pos, p + size, [](auto v) { return v == 0; });
}
struct Extension { std::uint8_t id; RtcpBytes value; };
bool extensions(const RtcpBytes& b, std::size_t& offset, std::size_t& end, std::uint16_t& profile, std::vector<Extension>& out)
{
    RtpPacketParser rtp(b.data(), b.size()); if (!rtp.valid()) return false;
    offset = 12 + rtp.csrcCount() * 4; end = offset; profile = 0xBEDE;
    if (!rtp.extension()) return true;
    profile = u16(b.data() + offset); end = offset + 4 + u16(b.data() + offset + 2) * 4;
    const bool one = profile == 0xBEDE;
    if (!one && (profile & 0xfff0) != 0x1000) return false;
    std::set<unsigned> ids;
    for (auto pos = offset + 4; pos < end;)
    {
        auto id = b[pos++]; if (!id) continue;
        std::size_t len;
        if (one) { len = (id & 15) + 1; id >>= 4; if (id == 15) break; if (!id) return false; }
        else { if (pos == end) return false; len = b[pos++]; }
        if (len > end - pos || !ids.insert(id).second) return false;
        out.push_back({id, RtcpBytes(b.begin() + pos, b.begin() + pos + len)}); pos += len;
    }
    return true;
}
} // namespace

bool parseRtcp(const std::uint8_t* data, std::size_t size, std::vector<RtcpMessage>& out)
{
    if (!data || size < 8 || size > 65507 || size % 4) return false;
    std::vector<RtcpMessage> parsed;
    for (std::size_t offset = 0; offset < size;)
    {
        if (size - offset < 4) return false;
        const auto* p = data + offset; const auto length = (std::size_t(u16(p + 2)) + 1) * 4;
        if (p[0] >> 6 != 2 || p[1] < 192 || p[1] > 223 || length > size - offset || length < 4) return false;
        auto n = length; const unsigned fmt = p[0] & 31;
        if (p[0] & 0x20)
        {
            if (offset + length != size || !p[length - 1] || p[length - 1] > length - 4) return false;
            n -= p[length - 1];
        }
        if (p[1] == 200 || p[1] == 201)
        {
            const std::size_t base = p[1] == 200 ? 28 : 8;
            if (n < base + fmt * 24 || (n - base - fmt * 24) % 4) return false;
            RtcpReport r; r.senderSsrc = u32(p + 4); r.senderReport = p[1] == 200;
            if (r.senderReport)
            {
                r.ntp = (std::uint64_t(u32(p + 8)) << 32) | u32(p + 12); r.rtpTimestamp = u32(p + 16);
                r.packetCount = u32(p + 20); r.octetCount = u32(p + 24);
            }
            for (unsigned i = 0; i < fmt; ++i)
            {
                const auto* b = p + base + i * 24; const auto lost = u24(b + 5);
                r.reports.push_back({u32(b), b[4], lost & 0x800000 ? std::int32_t(lost) - 0x1000000 : std::int32_t(lost),
                    u32(b + 8), u32(b + 12), u32(b + 16), u32(b + 20)});
            }
            parsed.emplace_back(std::move(r));
        }
        else if (p[1] == 205 || p[1] == 206)
        {
            if (n < 12) return false;
            if (p[1] == 205 && fmt == 1)
            {
                if (n < 16 || (n - 12) % 4) return false;
                RtcpNack f{u32(p + 4), u32(p + 8), {}}; std::set<std::uint16_t> seen;
                for (std::size_t pos = 12; pos < n; pos += 4)
                {
                    const auto pid = u16(p + pos), blp = u16(p + pos + 2);
                    if (seen.insert(pid).second) f.sequences.push_back(pid);
                    for (unsigned i = 0; i < 16; ++i) if (blp & (1u << i))
                    {
                        const auto seq = static_cast<std::uint16_t>(pid + i + 1);
                        if (seen.insert(seq).second) f.sequences.push_back(seq);
                    }
                    if (f.sequences.size() > 8192) return false;
                }
                parsed.emplace_back(std::move(f));
            }
            else if (p[1] == 206 && fmt == 1)
            {
                if (n != 12) return false;
                parsed.emplace_back(RtcpKeyFrameRequest{u32(p + 4), u32(p + 8), RtcpKeyFrameRequest::Kind::Pli, 0});
            }
            else if (p[1] == 206 && fmt == 4)
            {
                if (n < 20 || (n - 12) % 8 || u32(p + 8)) return false;
                for (std::size_t pos = 12; pos < n; pos += 8)
                    parsed.emplace_back(RtcpKeyFrameRequest{u32(p + 4), u32(p + pos), RtcpKeyFrameRequest::Kind::Fir, p[pos + 4]});
            }
            else if (p[1] == 205 && fmt == 15)
            {
                RtcpTwcc f; if (!twcc(p, n, f)) return false; parsed.emplace_back(std::move(f));
            }
            else if (p[1] == 206 && fmt == 15 && n >= 16 && p[12] == 'R' && p[13] == 'E' && p[14] == 'M' && p[15] == 'B')
            {
                if (n < 20 || !p[16] || n != 20 + std::size_t(p[16]) * 4 || u32(p + 8)) return false;
                RtcpRemb f; f.senderSsrc = u32(p + 4);
                const unsigned exponent = p[17] >> 2;
                const auto mantissa = (std::uint64_t(p[17] & 3) << 16) | u16(p + 18);
                constexpr auto max = std::numeric_limits<std::uint64_t>::max();
                f.bitrateBps = mantissa > (max >> exponent) ? max : mantissa << exponent;
                for (unsigned i = 0; i < p[16]; ++i) f.ssrcs.push_back(u32(p + 20 + i * 4));
                parsed.emplace_back(std::move(f));
            }
        }
        else if (p[1] == 202)
        {
            std::size_t pos = 4;
            for (unsigned i = 0; i < fmt; ++i)
            {
                if (pos + 4 > n) return false;
                pos += 4; bool ended = false;
                while (pos < n)
                {
                    const auto type = p[pos++]; if (!type) { ended = true; break; }
                    if (pos == n) return false;
                    const auto len = p[pos++]; if (len > n - pos) return false; pos += len;
                }
                if (!ended) return false;
                while (pos % 4) { if (pos == n || p[pos++]) return false; }
            }
            if (pos != n) return false;
        }
        offset += length;
    }
    out = std::move(parsed); return true;
}
RtcpBytes buildRtcpReport(const RtcpReport& r)
{
    if (r.reports.size() > 31) throw std::invalid_argument("too many reception reports");
    auto b = header(r.senderReport ? 200 : 201, r.reports.size(), r.senderSsrc);
    if (r.senderReport)
    {
        put32(b, r.ntp >> 32); put32(b, r.ntp & 0xffffffff); put32(b, r.rtpTimestamp); put32(b, r.packetCount); put32(b, r.octetCount);
    }
    for (const auto& block : r.reports)
    {
        if (block.cumulativeLost < -8388608 || block.cumulativeLost > 8388607) throw std::invalid_argument("loss outside signed24");
        put32(b, block.ssrc); b.push_back(block.fractionLost); put24(b, static_cast<std::uint32_t>(block.cumulativeLost));
        put32(b, block.highestSequence); put32(b, block.jitter); put32(b, block.lsr); put32(b, block.dlsr);
    }
    return finish(std::move(b));
}
RtcpBytes buildRtcpRemb(const RtcpRemb& f)
{
    if (f.ssrcs.empty() || f.ssrcs.size() > 255) throw std::invalid_argument("invalid REMB SSRC count");
    auto b = header(206, 15, f.senderSsrc); put32(b, 0);
    b.insert(b.end(), {'R', 'E', 'M', 'B'}); b.push_back(static_cast<std::uint8_t>(f.ssrcs.size()));
    auto mantissa = f.bitrateBps; unsigned exponent = 0;
    while (mantissa > 0x3ffff) { mantissa >>= 1; ++exponent; }
    b.push_back(static_cast<std::uint8_t>((exponent << 2) | (mantissa >> 16))); put16(b, static_cast<std::uint16_t>(mantissa));
    for (auto ssrc : f.ssrcs) put32(b, ssrc);
    return finish(std::move(b));
}
RtcpBytes buildRtcpSdes(std::uint32_t ssrc, const std::string& cname)
{
    if (cname.empty() || cname.size() > 255) throw std::invalid_argument("invalid RTCP CNAME");
    auto b = header(202, 1, ssrc); b.push_back(1); b.push_back(cname.size()); b.insert(b.end(), cname.begin(), cname.end()); b.push_back(0); return finish(std::move(b));
}
RtcpBytes buildRtcpNack(const RtcpNack& f)
{
    if (f.sequences.empty() || f.sequences.size() > 8192) throw std::invalid_argument("invalid NACK count");
    auto b = header(205, 1, f.senderSsrc); put32(b, f.mediaSsrc);
    // Caller supplies increasing extended sequence order; unsigned subtraction handles wrap.
    std::set<std::uint16_t> seen; std::size_t pos = 0;
    while (pos < f.sequences.size())
    {
        const auto pid = f.sequences[pos++]; if (!seen.insert(pid).second) continue;
        std::uint16_t blp = 0;
        while (pos < f.sequences.size())
        {
            const auto d = static_cast<std::uint16_t>(f.sequences[pos] - pid);
            if (!d) { ++pos; continue; } if (d > 16) break;
            blp |= 1u << (d - 1); seen.insert(f.sequences[pos++]);
        }
        put16(b, pid); put16(b, blp);
    }
    return finish(std::move(b));
}
RtcpBytes buildRtcpKeyFrameRequest(const RtcpKeyFrameRequest& f)
{
    const bool fir = f.kind == RtcpKeyFrameRequest::Kind::Fir;
    auto b = header(206, fir ? 4 : 1, f.senderSsrc); put32(b, fir ? 0 : f.mediaSsrc);
    if (fir) { put32(b, f.mediaSsrc); b.push_back(f.sequence); b.insert(b.end(), 3, 0); }
    return finish(std::move(b));
}
RtcpBytes buildRtcpTwcc(const RtcpTwcc& f)
{
    if (f.deltas.empty() || f.deltas.size() > 8192 || f.referenceTime > 0xffffff) throw std::invalid_argument("invalid TWCC size/time");
    auto b = header(205, 15, f.senderSsrc); put32(b, f.mediaSsrc); put16(b, f.baseSequence); put16(b, f.deltas.size());
    put24(b, f.referenceTime); b.push_back(f.feedbackCount);
    std::vector<unsigned> symbols;
    for (const auto& delta : f.deltas)
    {
        if (delta && (*delta < -32768 || *delta > 32767)) throw std::invalid_argument("TWCC delta too large");
        symbols.push_back(!delta ? 0 : *delta >= 0 && *delta <= 255 ? 1 : 2);
    }
    for (std::size_t pos = 0; pos < symbols.size();)
    {
        std::size_t run = 1; while (pos + run < symbols.size() && symbols[pos + run] == symbols[pos] && run < 8191) ++run;
        if (run >= 7) { put16(b, (symbols[pos] << 13) | run); pos += run; }
        else
        {
            std::uint16_t chunk = 0xc000;
            for (unsigned i = 0; i < 7 && pos < symbols.size(); ++i, ++pos) chunk |= symbols[pos] << (12 - 2 * i);
            put16(b, chunk);
        }
    }
    for (const auto& delta : f.deltas) if (delta)
    {
        if (*delta >= 0 && *delta <= 255) b.push_back(*delta); else put16(b, static_cast<std::uint16_t>(*delta));
    }
    return finish(std::move(b));
}
std::optional<std::uint16_t> rtpTransportSequence(const RtcpBytes& b, std::uint8_t id)
{
    std::size_t offset, end; std::uint16_t profile; std::vector<Extension> values;
    if (!id || !extensions(b, offset, end, profile, values)) return std::nullopt;
    for (const auto& e : values) if (e.id == id && e.value.size() == 2) return u16(e.value.data());
    return std::nullopt;
}
bool setRtpTransportSequence(RtcpBytes& b, std::uint8_t id, std::uint16_t sequence)
{
    std::size_t offset, end; std::uint16_t profile; std::vector<Extension> values;
    if (!id || !extensions(b, offset, end, profile, values)) return false;
    if (profile == 0xBEDE && id > 14)
    {
        if (b[0] & 0x10) return false; // never silently switch the format of existing extensions
        profile = 0x1000;
    }
    bool found = false;
    for (auto& e : values) if (e.id == id) { e.value = {static_cast<std::uint8_t>(sequence >> 8), static_cast<std::uint8_t>(sequence)}; found = true; }
    if (!found) values.push_back({id, {static_cast<std::uint8_t>(sequence >> 8), static_cast<std::uint8_t>(sequence)}});
    RtcpBytes ext; put16(ext, profile); put16(ext, 0);
    for (const auto& e : values)
    {
        if (profile == 0xBEDE) ext.push_back((e.id << 4) | (e.value.size() - 1));
        else { ext.push_back(e.id); ext.push_back(e.value.size()); }
        ext.insert(ext.end(), e.value.begin(), e.value.end());
    }
    while (ext.size() % 4) ext.push_back(0);
    const auto words = (ext.size() - 4) / 4; ext[2] = words >> 8; ext[3] = words & 255;
    RtcpBytes result(b.begin(), b.begin() + offset); result[0] |= 0x10;
    result.insert(result.end(), ext.begin(), ext.end()); result.insert(result.end(), b.begin() + end, b.end());
    b = std::move(result); return true;
}
} // namespace webrtc
