#pragma once

#include"common/common.h"

#include<cstdint>
#include<vector>

// RTP 固定头（RFC 3550 §5.1），CSRC 列表一并放入
struct RtpHeader
{
    std::uint8_t  version{2};
    bool          padding{false};
    bool          extension{false};
    std::uint8_t  csrcCount{0};
    bool          marker{false};
    std::uint8_t  payloadType{0};
    std::uint16_t sequenceNumber{0};
    std::uint32_t timestamp{0};
    std::uint32_t ssrc{0};
    std::vector<std::uint32_t> csrc;
};

// RTP 包解析器（与 rtcp_packet.h 的 RtcpPacketParser 同款风格）：
// 一次性解析一个 RTP 包，校验版本/长度，跳过 CSRC 与扩展头，
// 剥掉 padding 后给出真实负载。
//
// 用法：
//   RtpPacketParser packet(data, size);
//   if (!packet.valid()) { ... 丢弃 ... }
//   packet.sequenceNumber(); packet.timestamp(); packet.ssrc();
//   packet.payload(); packet.payloadSize();
class RtpPacketParser
{
public:
    explicit RtpPacketParser(const std::uint8_t* data, std::size_t size);

    // 数据不足 12 字节、版本不是 2、或头/扩展/padding 越界时为 false
    bool valid() const { return m_valid_; }

    const RtpHeader& header() const { return m_header_; }

    std::uint8_t  version() const        { return m_header_.version; }
    bool          padding() const        { return m_header_.padding; }
    bool          extension() const      { return m_header_.extension; }
    std::uint8_t  csrcCount() const      { return m_header_.csrcCount; }
    bool          marker() const         { return m_header_.marker; }
    std::uint8_t  payloadType() const    { return m_header_.payloadType; }
    std::uint16_t sequenceNumber() const { return m_header_.sequenceNumber; }
    std::uint32_t timestamp() const      { return m_header_.timestamp; }
    std::uint32_t ssrc() const           { return m_header_.ssrc; }
    const std::vector<std::uint32_t>& csrc() const { return m_header_.csrc; }

    // 跳过 CSRC/扩展头、去掉 padding 后的真实负载
    const std::uint8_t* payload() const { return m_payload_; }
    std::size_t         payloadSize() const { return m_payloadSize_; }

private:
    static std::uint16_t read16(const std::uint8_t* p)
    {
        return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }
    static std::uint32_t read32(const std::uint8_t* p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) |
               static_cast<std::uint32_t>(p[3]);
    }

    RtpHeader m_header_;
    const std::uint8_t* m_payload_{nullptr};
    std::size_t m_payloadSize_{0};
    bool m_valid_{false};
};

inline RtpPacketParser::RtpPacketParser(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size < 12)
        return;

    const std::uint8_t* p = data;

    m_header_.version     = p[0] >> 6;
    m_header_.padding     = (p[0] & 0x20) != 0;
    m_header_.extension   = (p[0] & 0x10) != 0;
    m_header_.csrcCount   = p[0] & 0x0F;
    m_header_.marker      = (p[1] & 0x80) != 0;
    m_header_.payloadType = p[1] & 0x7F;
    m_header_.sequenceNumber = read16(p + 2);
    m_header_.timestamp      = read32(p + 4);
    m_header_.ssrc           = read32(p + 8);

    if (m_header_.version != 2)
        return;                           // 版本不是 2：非法

    // CSRC 列表
    std::size_t offset = 12;
    if (size < offset + static_cast<std::size_t>(m_header_.csrcCount) * 4)
        return;
    m_header_.csrc.reserve(m_header_.csrcCount);
    for (std::uint8_t i = 0; i < m_header_.csrcCount; ++i)
    {
        m_header_.csrc.push_back(read32(p + offset));
        offset += 4;
    }

    // 扩展头：profile(16) + length(16，按 32bit 计)，不含这 4 字节本身
    if (m_header_.extension)
    {
        if (size < offset + 4)
            return;
        const std::size_t extBytes =
            static_cast<std::size_t>(read16(p + offset + 2)) * 4;
        offset += 4;
        if (size < offset + extBytes)
            return;
        offset += extBytes;
    }

    // padding：最后一个字节是填充长度（含该字节自身）
    std::size_t payloadEnd = size;
    if (m_header_.padding)
    {
        const std::size_t paddingLen = p[size - 1];
        if (paddingLen == 0 || paddingLen > size - offset)
            return;                       // padding 长度非法（不能吃掉头）
        payloadEnd = size - paddingLen;
    }

    m_payload_     = p + offset;
    m_payloadSize_ = payloadEnd - offset;
    m_valid_       = true;
}
