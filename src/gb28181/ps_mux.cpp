#include "ps_mux.h"

#include <cstring>

#include "log/log.h"

// PES 载荷上限：PES_packet_length 是 16 位，参考实现取 0xFF80（留出头部余量，
// 也是海康等设备沿用的大小），超过则拆成多个 PES 包，只有第一个带时间戳。
static const std::size_t MAX_PES_PACKET = 0xFF80;

namespace
{
inline void write16(std::uint8_t *p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline void write32(std::uint8_t *p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v & 0xFF);
}

// MPEG-2 的 CRC32：多项式 0x04C11DB7，非反射，初值 0xFFFFFFFF，无最终异或
// （ISO/IEC 13818-1 Annex B；校验值 "123456789" -> 0x0376E6E7）
std::uint32_t mpegCrc32(std::uint32_t crc, const std::uint8_t *data, std::size_t len)
{
    for (std::size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
        }
    }
    return crc;
}
}

// ---------------------------------------------------------------------------
// PS 包头（14 字节，ISO/IEC 13818-1 2.5.3.3）
//   '01' + SCR[32..30] + marker + SCR[29..15] + marker + SCR[14..0] + marker
//   + SCR_ext(9) + marker + program_mux_rate(22) + marker + 保留(5) + stuffing(3)
// ---------------------------------------------------------------------------
void PsMuxer::writePackHeader(std::vector<std::uint8_t> &out, std::int64_t scr90k) const
{
    std::uint8_t d[14] = {0};
    write32(d, 0x000001BA);

    const std::uint64_t base = static_cast<std::uint64_t>(scr90k < 0 ? 0 : scr90k) & 0x1FFFFFFFFull;
    const std::uint32_t ext = 0;                    // SCR 扩展位，90kHz 下取 0
    const std::uint32_t muxRate = m_cfg.muxRate & 0x3FFFFF;

    d[4] = static_cast<std::uint8_t>(0x44 | (((base >> 30) & 0x07) << 3) | ((base >> 28) & 0x03));
    d[5] = static_cast<std::uint8_t>((base >> 20) & 0xFF);
    d[6] = static_cast<std::uint8_t>(0x04 | (((base >> 15) & 0x1F) << 3) | ((base >> 13) & 0x03));
    d[7] = static_cast<std::uint8_t>((base >> 5) & 0xFF);
    d[8] = static_cast<std::uint8_t>(0x04 | ((base & 0x1F) << 3) | ((ext >> 7) & 0x03));
    d[9] = static_cast<std::uint8_t>(0x01 | ((ext & 0x7F) << 1));

    d[10] = static_cast<std::uint8_t>(muxRate >> 14);
    d[11] = static_cast<std::uint8_t>((muxRate >> 6) & 0xFF);
    d[12] = static_cast<std::uint8_t>(0x03 | ((muxRate & 0x3F) << 2));

    d[13] = 0xF8;   // 保留 5 位 = 11111，stuffing_length = 0

    out.insert(out.end(), d, d + sizeof(d));
}

// ---------------------------------------------------------------------------
// 系统头（15 字节 = 6 + 3×1 路流，ISO/IEC 13818-1 2.5.3.4）
// ---------------------------------------------------------------------------
void PsMuxer::writeSystemHeader(std::vector<std::uint8_t> &out) const
{
    std::uint8_t d[15] = {0};
    write32(d, 0x000001BB);
    write16(d + 4, 9);                              // 头部长度 = 6 + 3×流数(1)

    const std::uint32_t rb = m_cfg.rateBound & 0x7FFFFF;
    d[6] = static_cast<std::uint8_t>(0x80 | ((rb >> 15) & 0x7F));
    d[7] = static_cast<std::uint8_t>((rb >> 7) & 0xFF);
    d[8] = static_cast<std::uint8_t>(0x01 | ((rb & 0x7F) << 1));

    d[9]  = 0x00;                                   // audio_bound=0, fixed_flag=0, CSPS_flag=0
    d[10] = static_cast<std::uint8_t>(0x20 | 0x01); // marker + video_bound=1
    d[11] = 0x7F;                                   // packet_rate_restriction=0 + 保留 7 位

    d[12] = streamId();                             // 0xE0
    d[13] = static_cast<std::uint8_t>(0xC0 | (1 << 5) | ((400 >> 8) & 0x1F)); // '11'+scale=1+size_bound 高 5 位
    d[14] = static_cast<std::uint8_t>(400 & 0xFF);

    out.insert(out.end(), d, d + sizeof(d));
}

// ---------------------------------------------------------------------------
// PSM（节目流映射，20 字节：1 路视频、无描述符、尾部 CRC32）
// ---------------------------------------------------------------------------
void PsMuxer::writePsm(std::vector<std::uint8_t> &out) const
{
    std::uint8_t d[20] = {0};
    write32(d, 0x000001BC);

    d[6] = 0xC1;        // current_next_indicator=1 + 保留 2 位 + version=1
    d[7] = 0x01;        // 保留 7 位 + marker_bit
    write16(d + 8, 0);  // program_stream_info_length = 0
    write16(d + 10, 4); // elementary_stream_map_length = 1 路 × 4 字节
    d[12] = streamType();
    d[13] = streamId();
    write16(d + 14, 0); // elementary_stream_info_length = 0

    write16(d + 4, 14); // program_stream_map_length = 后面 14 字节(不含 CRC 之外的自身)

    const std::uint32_t crc = mpegCrc32(0xFFFFFFFFu, d, 16);
    write32(d + 16, crc);

    out.insert(out.end(), d, d + sizeof(d));
}

// ---------------------------------------------------------------------------
// PES 头（9 字节 + 可选 PTS/DTS）
// ---------------------------------------------------------------------------
std::size_t PsMuxer::writePesHeader(std::uint8_t *dst, std::size_t dstSize,
                                    std::int64_t pts90k, std::int64_t dts90k,
                                    bool hasPts, bool hasDts, bool keyFrame) const
{
    if (dstSize < 9)
    {
        return 0;
    }

    dst[0] = 0x00;
    dst[1] = 0x00;
    dst[2] = 0x01;
    dst[3] = streamId();
    dst[4] = 0x00;   // PES_packet_length 稍后回填
    dst[5] = 0x00;

    // '10' + PES_scrambling_control(00) + PES_priority(0) + data_alignment_indicator + 版权(00)
    dst[6] = static_cast<std::uint8_t>(0x80 | (keyFrame ? 0x04 : 0x00));

    std::uint8_t flags = 0x00;
    std::uint8_t headerDataLen = 0;
    if (hasPts)
    {
        flags |= 0x80;
        headerDataLen = static_cast<std::uint8_t>(headerDataLen + 5);
    }
    if (hasDts)
    {
        flags |= 0x40;
        headerDataLen = static_cast<std::uint8_t>(headerDataLen + 5);
    }
    dst[7] = flags;
    dst[8] = headerDataLen;

    std::uint8_t *p = dst + 9;

    if (hasPts)
    {
        const std::uint64_t pts = static_cast<std::uint64_t>(pts90k < 0 ? 0 : pts90k) & 0x1FFFFFFFFull;
        // 前缀 '0010'(仅 PTS) / '0011'(PTS+DTS)：由 flags 推导
        p[0] = static_cast<std::uint8_t>(((flags >> 2) & 0x30) | (((pts >> 30) & 0x07) << 1) | 0x01);
        p[1] = static_cast<std::uint8_t>((pts >> 22) & 0xFF);
        p[2] = static_cast<std::uint8_t>((((pts >> 14) & 0xFE)) | 0x01);
        p[3] = static_cast<std::uint8_t>((pts >> 7) & 0xFF);
        p[4] = static_cast<std::uint8_t>(((pts << 1) & 0xFE) | 0x01);
        p += 5;
    }

    if (hasDts)
    {
        const std::uint64_t dts = static_cast<std::uint64_t>(dts90k < 0 ? 0 : dts90k) & 0x1FFFFFFFFull;
        p[0] = static_cast<std::uint8_t>(0x10 | (((dts >> 30) & 0x07) << 1) | 0x01);
        p[1] = static_cast<std::uint8_t>((dts >> 22) & 0xFF);
        p[2] = static_cast<std::uint8_t>((((dts >> 14) & 0xFE)) | 0x01);
        p[3] = static_cast<std::uint8_t>((dts >> 7) & 0xFF);
        p[4] = static_cast<std::uint8_t>(((dts << 1) & 0xFE) | 0x01);
        p += 5;
    }

    return static_cast<std::size_t>(p - dst);
}

// ---------------------------------------------------------------------------
// AUD 检测 / 插入
// ---------------------------------------------------------------------------
bool PsMuxer::hasAud(const std::uint8_t *data, std::size_t size) const
{
    for (std::size_t i = 0; i + 4 < size; ++i)
    {
        if (data[i] != 0x00 || data[i + 1] != 0x00 || data[i + 2] != 0x01)
        {
            continue;
        }
        const std::uint8_t nal = data[i + 3];
        if (m_cfg.codec == Codec::H264)
        {
            if ((nal & 0x1F) == 9)      // H.264 AUD
            {
                return true;
            }
        }
        else
        {
            if (((nal >> 1) & 0x3F) == 35)  // H.265 AUD_NUT
            {
                return true;
            }
        }
    }
    return false;
}

void PsMuxer::appendAud(std::vector<std::uint8_t> &out) const
{
    static const std::uint8_t kAudH264[6] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};
    static const std::uint8_t kAudH265[7] = {0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50};
    if (m_cfg.codec == Codec::H264)
    {
        out.insert(out.end(), kAudH264, kAudH264 + sizeof(kAudH264));
    }
    else
    {
        out.insert(out.end(), kAudH265, kAudH265 + sizeof(kAudH265));
    }
}

// ---------------------------------------------------------------------------
// 一帧 → PS
// ---------------------------------------------------------------------------
int PsMuxer::packFrame(const std::uint8_t *annexB, std::size_t size,
                       std::uint64_t ptsUs, bool keyFrame,
                       std::vector<std::uint8_t> &out)
{
    if (annexB == nullptr || size == 0)
    {
        UAV_LOG_WARN << "PsMuxer::packFrame 空数据\n";
        return -1;
    }

    // 首帧必须带 PS 头/系统头/PSM，否则接收端拿不到 PSI
    if (m_firstFrame)
    {
        keyFrame = true;
    }

    const std::int64_t ts = static_cast<std::int64_t>(toTimestamp90k(ptsUs));
    // SCR 略早于 DTS 一个帧间隔（参考实现取 dts-3600，即 40ms@90kHz）
    const std::int64_t scr = (ts >= 3600) ? (ts - 3600) : 0;

    if (keyFrame)
    {
        writePackHeader(out, scr);
        writeSystemHeader(out);
        writePsm(out);
    }

    const bool needAud = m_cfg.insertAud && !hasAud(annexB, size);
    const std::size_t audLen = needAud ? ((m_cfg.codec == Codec::H265) ? 7u : 6u) : 0u;

    const std::uint8_t *payload = annexB;
    std::size_t remaining = size;
    bool first = true;

    while (remaining > 0 || first)
    {
        std::uint8_t pesHeader[16] = {0};
        const bool withPts = first;
        const bool withDts = first && m_cfg.withDts;
        const std::size_t pesHeaderLen =
            writePesHeader(pesHeader, sizeof(pesHeader), ts, ts, withPts, withDts, keyFrame);
        if (pesHeaderLen == 0)
        {
            return -1;
        }

        // 单个 PES 包可承载的净荷上限
        const std::size_t overhead = (pesHeaderLen - 6) + (first ? audLen : 0);
        std::size_t maxPayload = (MAX_PES_PACKET > overhead) ? (MAX_PES_PACKET - overhead) : 1;
        const std::size_t n = (remaining < maxPayload) ? remaining : maxPayload;

        const std::size_t pesStart = out.size();
        out.insert(out.end(), pesHeader, pesHeader + pesHeaderLen);
        if (first && needAud)
        {
            appendAud(out);
        }
        out.insert(out.end(), payload, payload + n);

        // 回填 PES_packet_length = 6 字节前缀之后的内容长度
        const std::size_t packetLen = out.size() - pesStart - 6;
        write16(&out[pesStart + 4], static_cast<std::uint16_t>(packetLen));

        payload += n;
        remaining -= n;
        first = false;
    }

    m_firstFrame = false;
    return static_cast<int>(out.size());
}
