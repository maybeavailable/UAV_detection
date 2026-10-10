#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// GB28181 的 PS 封装：把 H.264 / H.265 的 Annex-B 码流打成 MPEG-2 PS
//
// 结构依据 GB/T 28181-2016 附录 C（即 ISO/IEC 13818-1 的 program stream）：
//   PS 包头 00 00 01 BA  +  系统头 00 00 01 BB  +  PSM 00 00 01 BC  +  PES 00 00 01 E0
//
// 关键帧：PS 包头 + 系统头 + PSM + PES（每个 IDR 前重发 PS 头，平台中途接入也能解）
// 非关键帧：只发 PES，省开销
//
// 时间戳单位：入参 µs，内部换算到 90kHz（GB28181 视频时钟频率）
//
// 用法：
//   PsMuxer mux;
//   mux.setConfig({PsMuxer::Codec::H264, true, ...});
//   std::vector<uint8_t> ps;
//   mux.packFrame(annexB, size, ptsUs, isKeyFrame, ps);   // ps 即一帧的 PS 数据
// ---------------------------------------------------------------------------
class PsMuxer
{
public:
    enum class Codec
    {
        H264 = 0,
        H265
    };

    struct Config
    {
        Codec    codec{Codec::H264};
        // 帧内没有 AUD 时自动补一个访问单元分隔符（H.222.0 2.14 要求每个访问单元含 AUD）
        bool     insertAud{true};
        // program_mux_rate，单位 50 字节/秒（仅提示，接收端一般忽略）
        uint32_t muxRate{6106};
        // 系统头 rate_bound，单位 50 字节/秒
        uint32_t rateBound{26234};
        // 是否写 DTS（pts==dts 时通常只写 PTS）
        bool     withDts{false};
    };

    PsMuxer() = default;
    ~PsMuxer() = default;

    PsMuxer(const PsMuxer &) = delete;
    PsMuxer &operator=(const PsMuxer &) = delete;

    void setConfig(const Config &config) { m_cfg = config; }
    const Config &config() const { return m_cfg; }

    // 输入一帧 Annex-B 码流（可含 SPS/PPS/SEI/切片，通常一帧一次调用），
    // 输出该帧的 PS 数据（追加到 out，调用方负责清空/复用）。
    // ptsUs：微秒；keyFrame：是否 IDR 关键帧（决定是否重发 PS/系统头/PSM）。
    // 返回写入的字节数，失败返回 -1。
    int packFrame(const std::uint8_t *annexB, std::size_t size,
                  std::uint64_t ptsUs, bool keyFrame,
                  std::vector<std::uint8_t> &out);

    // 复位内部状态（重新从关键帧开始）
    void reset() { m_firstFrame = true; }

    // 是否已经发过 PS 头（用于判断平台是否已拿到 PSI）
    bool psiSent() const { return !m_firstFrame; }

    // µs → 90kHz 时间戳
    static std::uint32_t toTimestamp90k(std::uint64_t ptsUs)
    {
        return static_cast<std::uint32_t>((ptsUs * 9ull) / 100ull);
    }

private:
    // PS 包头（14 字节）
    void writePackHeader(std::vector<std::uint8_t> &out, std::int64_t scr90k) const;
    // 系统头（15 字节：1 路视频）
    void writeSystemHeader(std::vector<std::uint8_t> &out) const;
    // PSM（20 字节：1 路视频 + CRC32）
    void writePsm(std::vector<std::uint8_t> &out) const;
    // PES 头（9 字节 + 可选 5/10 字节时间戳），返回写入长度
    std::size_t writePesHeader(std::uint8_t *dst, std::size_t dstSize,
                               std::int64_t pts90k, std::int64_t dts90k,
                               bool hasPts, bool hasDts, bool keyFrame) const;
    std::uint8_t streamId() const { return 0xE0; }        // 视频流 PES
    std::uint8_t streamType() const                        // PSM 中的 stream_type
    {
        return (m_cfg.codec == Codec::H265) ? 0x24 : 0x1B; // H.265 / H.264
    }
    bool hasAud(const std::uint8_t *data, std::size_t size) const;
    void appendAud(std::vector<std::uint8_t> &out) const;

    Config m_cfg{};
    bool   m_firstFrame{true};     // 首帧必须带 PS 头/系统头/PSM
};
