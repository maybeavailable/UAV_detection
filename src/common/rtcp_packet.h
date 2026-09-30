#pragma once

#include"common/common.h"

#include<cstdint>
#include<ctime>
#include<functional>
#include<vector>

// RTCP 包类型（RFC 3550）
enum RtcpPacketType : std::uint8_t
{
    RTCP_SR   = 200,   // Sender Report
    RTCP_RR   = 201,   // Receiver Report
    RTCP_SDES = 202,   // 源描述
    RTCP_BYE  = 203,   // 离开会话
    RTCP_APP  = 204,   // 应用自定义
};

// RR/SR 里的报告块（RFC 3550 §6.4.1）
struct RtcpReportBlock
{
    std::uint32_t ssrc{0};
    std::uint8_t  fractionLost{0};      // 定点 8 位：丢失比例 = n / 256
    std::int32_t  cumulativeLost{0};    // 24 位有符号
    std::uint32_t highestSeq{0};        // 扩展最高序号
    std::uint32_t jitter{0};            // 到达间隔抖动（RTP 时间戳单位）
    std::uint32_t lsr{0};               // 最近收到的 SR 的 NTP 中间 32 位
    std::uint32_t dlsr{0};              // 自 LSR 起的延迟（1/65536 秒）
};

// 客户端对一条流的反馈聚合（RR 中匹配我方 SSRC 的报告块）
struct RtcpFeedback
{
    std::uint32_t ssrc{0};
    std::uint8_t  fractionLost{0};
    std::int32_t  cumulativeLost{0};
    std::uint32_t highestSeq{0};
    std::uint32_t jitter{0};
    double        rttMs{0.0};

    double lossPercent() const { return fractionLost * 100.0 / 256.0; }
    double jitterMs(std::uint32_t clockRate = 90000) const
    {
        return jitter * 1000.0 / clockRate;
    }
};

// 客户端反馈回调：解析到匹配我方 SSRC 的 RR 报告块时触发
using RtcpFeedbackCallback = std::function<void(const RtcpFeedback&)>;

// RTCP 复合包迭代解析：一个 UDP 载荷（或一个 TCP interleaved 帧）里
// 通常装着一串子包，例如 RR + SDES。
// 用法：
//   for (RtcpPacketParser parser(data, size); parser.next(); ) {
//       if (parser.type() == RTCP_RR) {
//           for (auto& block : parser.reportBlocks()) { ... }
//       }
//   }
class RtcpPacketParser
{
public:
    explicit RtcpPacketParser(const std::uint8_t* data, std::size_t size)
        : m_data_(data), m_size_(size)
    {
    }

    // 迭代到下一个完整子包；数据不足/版本非法/截断返回 false
    bool next()
    {
        while (m_offset_ + 4 <= m_size_)
        {
            const std::uint8_t* h = m_data_ + m_offset_;
            if ((h[0] >> 6) != 2)
                return false;                     // 版本不是 2

            m_type_        = h[1];
            m_reportCount_ = h[0] & 0x1Fu;
            const std::size_t bytes =
                (static_cast<std::size_t>((h[2] << 8) | h[3]) + 1) * 4;
            if (m_offset_ + bytes > m_size_)
                return false;                     // 截断

            m_payload_     = h + 4;
            m_payloadSize_ = bytes - 4;
            m_offset_ += bytes;

            if (m_type_ == 0)
                continue;                         // 空包（填充用）
            return true;
        }
        return false;
    }

    std::uint8_t  type() const { return m_type_; }
    std::uint8_t  reportCount() const { return m_reportCount_; }
    const std::uint8_t* payload() const { return m_payload_; }
    std::size_t   payloadSize() const { return m_payloadSize_; }

    // RR：报告块从 payload 开头；SR：跳过 20 字节发送者信息
    std::vector<RtcpReportBlock> reportBlocks() const
    {
        std::vector<RtcpReportBlock> blocks;
        std::size_t offset = (m_type_ == RTCP_SR) ? 20u : 0u;
        while (offset + 24 <= m_payloadSize_)
        {
            const std::uint8_t* p = m_payload_ + offset;

            RtcpReportBlock block;
            block.ssrc         = read32(p);
            block.fractionLost = p[4];
            const std::uint32_t lost24 =
                (static_cast<std::uint32_t>(p[5]) << 16) |
                (static_cast<std::uint32_t>(p[6]) << 8) |
                static_cast<std::uint32_t>(p[7]);
            block.cumulativeLost = static_cast<std::int32_t>(lost24 << 8) >> 8;  // 24 位符号扩展
            block.highestSeq     = read32(p + 8);
            block.jitter         = read32(p + 12);
            block.lsr            = read32(p + 16);
            block.dlsr           = read32(p + 20);
            blocks.push_back(block);
            offset += 24;
        }
        return blocks;
    }

private:
    static std::uint32_t read32(const std::uint8_t* p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) |
               static_cast<std::uint32_t>(p[3]);
    }

    const std::uint8_t* m_data_{nullptr};
    std::size_t m_size_{0};
    std::size_t m_offset_{0};
    std::uint8_t m_type_{0};
    std::uint8_t m_reportCount_{0};
    const std::uint8_t* m_payload_{nullptr};
    std::size_t m_payloadSize_{0};
};

// 当前 NTP 时间的中间 32 位（1/65536 秒单位，用于 RTT 计算）
inline std::uint32_t rtcpNtpMiddle32Now()
{
    const std::uint64_t seconds =
        static_cast<std::uint64_t>(std::time(nullptr)) + 2208988800ULL;
    return static_cast<std::uint32_t>(seconds << 16);
}

// LSR/DLSR → RTT（毫秒）。RFC 3550 §A.8：RTT = now - LSR - DLSR
// 注意：LSR == 0 表示客户端还没收到过我们的 SR，无法计算，返回 0。
inline double rtcpRttMs(std::uint32_t lsr, std::uint32_t dlsr)
{
    if (lsr == 0)
        return 0.0;
    const std::int32_t delta =
        static_cast<std::int32_t>(rtcpNtpMiddle32Now() - lsr - dlsr);
    return delta * (1000.0 / 65536.0);
}
