#include"rtsp/h264_access_unit.h"
#include"rtsp/rtp_source.h"

#include<map>
#include<optional>
#include<stdexcept>

namespace
{
[[noreturn]] void invalid() { throw std::invalid_argument("invalid or unsupported H.264 access unit headers"); }

class Bits
{
    std::vector<std::uint8_t> rbsp;
    std::size_t pos{0};
public:
    explicit Bits(NalUnit nal)
    {
        unsigned zeros = 0;
        for (std::size_t i = 1; i < nal.size; ++i)
        {
            const auto b = nal.data[i];
            if (zeros >= 2 && b == 3) { zeros = 0; continue; }
            rbsp.push_back(b);
            zeros = b == 0 ? zeros + 1 : 0;
        }
    }
    std::uint32_t get(unsigned n = 1)
    {
        if (n > 32 || pos + n > rbsp.size() * 8) invalid();
        std::uint32_t value = 0;
        for (unsigned i = 0; i < n; ++i, ++pos)
            value = (value << 1) | ((rbsp[pos / 8] >> (7 - pos % 8)) & 1);
        return value;
    }
    std::uint32_t ue()
    {
        unsigned zeros = 0;
        while (!get()) if (++zeros > 30) invalid();
        return ((1u << zeros) - 1) + get(zeros);
    }
    std::int32_t se()
    {
        const auto value = ue();
        return (value & 1) ? static_cast<std::int32_t>((value + 1) / 2)
                           : -static_cast<std::int32_t>(value / 2);
    }
};

struct Sps
{
    unsigned id{}, frameBits{}, pocType{}, pocBits{};
    bool separateColour{}, frameOnly{}, deltaAlwaysZero{};
    NalUnit nal;
};
struct Pps
{
    unsigned id{}, spsId{};
    bool bottomPoc{};
    NalUnit nal;
};

Sps readSps(NalUnit nal)
{
    Bits b(nal);
    Sps s;
    s.nal = nal;
    const auto profile = b.get(8);
    b.get(16); // constraint flags, level
    s.id = b.ue();
    if (s.id > 31) invalid();
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135)
    {
        const auto chroma = b.ue();
        if (chroma > 3) invalid();
        if (chroma == 3) s.separateColour = b.get();
        b.ue(); b.ue(); b.get(); // bit depths, transform bypass
        if (b.get())
        {
            const unsigned lists = chroma == 3 ? 12 : 8;
            for (unsigned i = 0; i < lists; ++i)
            {
                if (!b.get()) continue;
                int last = 8, next = 8;
                for (unsigned j = 0; j < (i < 6 ? 16u : 64u); ++j)
                {
                    if (next) next = static_cast<int>((static_cast<std::int64_t>(last) + b.se()) % 256 + 256) % 256;
                    if (next) last = next;
                }
            }
        }
    }
    const auto logFrame = b.ue();
    if (logFrame > 12) invalid();
    s.frameBits = logFrame + 4;
    s.pocType = b.ue();
    if (s.pocType == 0)
    {
        const auto logPoc = b.ue();
        if (logPoc > 12) invalid();
        s.pocBits = logPoc + 4;
    }
    else if (s.pocType == 1)
    {
        s.deltaAlwaysZero = b.get();
        b.se(); b.se();
        const auto cycle = b.ue();
        if (cycle > 255) invalid();
        for (unsigned i = 0; i < cycle; ++i) b.se();
    }
    else if (s.pocType != 2) invalid();
    b.ue(); b.get(); b.ue(); b.ue(); // refs, gaps, width, height
    s.frameOnly = b.get();
    return s;
}

Pps readPps(NalUnit nal)
{
    Bits b(nal);
    Pps p;
    p.nal = nal;
    p.id = b.ue(); p.spsId = b.ue();
    if (p.id > 255 || p.spsId > 31) invalid();
    b.get();
    p.bottomPoc = b.get();
    return p;
}

struct Picture
{
    unsigned pps{}, frame{}, idrId{}, pocLsb{}, pocType{}, colourPlane{};
    int deltaBottom{}, delta0{}, delta1{};
    bool field{}, bottom{}, reference{}, idr{};
    bool differs(const Picture& p) const
    {
        return pps != p.pps || frame != p.frame ||
            field != p.field || (field && bottom != p.bottom) || reference != p.reference ||
            pocType != p.pocType || pocLsb != p.pocLsb || deltaBottom != p.deltaBottom ||
            delta0 != p.delta0 || delta1 != p.delta1 || idr != p.idr || (idr && idrId != p.idrId);
    }
};

Picture readPicture(NalUnit nal, const std::map<unsigned, Sps>& sps, const std::map<unsigned, Pps>& pps)
{
    Bits b(nal);
    Picture pic;
    b.ue(); b.ue(); // first_mb_in_slice, slice_type（不能仅据 first_mb 判断图像边界）
    pic.pps = b.ue();
    const auto p = pps.find(pic.pps);
    if (p == pps.end()) invalid();
    const auto s = sps.find(p->second.spsId);
    if (s == sps.end()) invalid();
    const auto& seq = s->second;
    if (seq.separateColour) pic.colourPlane = b.get(2);
    pic.frame = b.get(seq.frameBits);
    if (!seq.frameOnly)
    {
        pic.field = b.get();
        if (pic.field) pic.bottom = b.get();
    }
    pic.reference = (nal.data[0] & 0x60) != 0;
    pic.idr = (nal.data[0] & 0x1f) == 5;
    if (pic.idr) pic.idrId = b.ue();
    pic.pocType = seq.pocType;
    if (seq.pocType == 0)
    {
        pic.pocLsb = b.get(seq.pocBits);
        if (p->second.bottomPoc && !pic.field) pic.deltaBottom = b.se();
    }
    if (seq.pocType == 1 && !seq.deltaAlwaysZero)
    {
        pic.delta0 = b.se();
        if (p->second.bottomPoc && !pic.field) pic.delta1 = b.se();
    }
    return pic;
}

void append(std::vector<std::uint8_t>& out, NalUnit nal)
{
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), nal.data, nal.data + nal.size);
}
}

std::vector<H264AccessUnit> parseH264AccessUnits(const std::uint8_t* data, std::size_t size)
{
    std::map<unsigned, Sps> sps;
    std::map<unsigned, Pps> pps;
    std::vector<H264AccessUnit> out;
    std::vector<NalUnit> current;
    std::optional<Picture> picture;
    auto flush = [&] {
        if (picture)
        {
            H264AccessUnit unit;
            unit.keyFrame = picture->idr;
            std::size_t start = 0;
            if (!current.empty() && (current[0].data[0] & 31) == 9)
                append(unit.data, current[start++]); // AUD 必须位于访问单元起点
            if (unit.keyFrame)
            {
                // 已知参数集按 ID 缓存；后续图像可能使用不同 PPS。
                // 不预读文件后部，也不丢弃访问单元原有的参数集/SEI 顺序。
                for (const auto& entry : sps) append(unit.data, entry.second.nal);
                for (const auto& entry : pps) append(unit.data, entry.second.nal);
            }
            for (; start < current.size(); ++start)
                append(unit.data, current[start]);
            out.push_back(std::move(unit));
        }
        current.clear();
        picture.reset();
    };
    for (const auto nal : splitAnnexB(data, size))
    {
        const unsigned type = nal.data[0] & 31;
        if ((nal.data[0] & 0x80) || type == 0 || type >= 13) invalid();
        if (type == 1 || type == 2 || type == 5)
        {
            const auto next = readPicture(nal, sps, pps);
            if (picture && picture->differs(next)) flush();
            if (!picture) picture = next;
        }
        else if (picture && (type == 6 || type == 7 || type == 8 || type == 9)) flush();
        if (type == 7) { const auto s = readSps(nal); sps[s.id] = s; }
        if (type == 8) { const auto p = readPps(nal); pps[p.id] = p; }
        if ((type == 3 || type == 4) && !picture) invalid();
        current.push_back(nal);
        if (type == 10 || type == 11) flush();
    }
    flush();
    if (out.empty()) invalid();
    return out;
}
