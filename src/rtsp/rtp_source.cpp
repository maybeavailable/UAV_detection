#include"rtsp/rtp_source.h"

#include"rtsp/rtp_transport.h"

#include<algorithm>
#include<cstring>
#include<random>

namespace
{

// 单包最大 RTP 负载；FU-A 再扣 2 字节分片头
constexpr std::size_t kMaxRtpPayload = 1400;
constexpr std::size_t kMaxFuPayload  = kMaxRtpPayload - 2;

std::uint32_t random32()
{
    static std::mt19937 gen(std::random_device{}());
    return static_cast<std::uint32_t>(gen());
}

bool isStartCode(const std::uint8_t* p, std::size_t size, std::size_t pos, std::size_t& codeLen)
{
    if (pos + 3 > size)
        return false;
    if (p[pos] == 0 && p[pos + 1] == 0 && p[pos + 2] == 1)
    {
        codeLen = 3;
        return true;
    }
    if (pos + 4 <= size && p[pos] == 0 && p[pos + 1] == 0 && p[pos + 2] == 0 && p[pos + 3] == 1)
    {
        codeLen = 4;
        return true;
    }
    return false;
}

}  // namespace

std::vector<NalUnit> splitAnnexB(const std::uint8_t* data, std::size_t size)
{
    std::vector<NalUnit> nals;
    if (data == nullptr || size == 0)
        return nals;

    std::size_t pos = 0;
    while (pos + 3 <= size)
    {
        std::size_t codeLen = 0;
        if (!isStartCode(data, size, pos, codeLen))
        {
            ++pos;
            continue;
        }

        const std::size_t payloadStart = pos + codeLen;
        std::size_t end = size;
        for (std::size_t k = payloadStart; k + 3 <= size; ++k)
        {
            std::size_t nextLen = 0;
            if (isStartCode(data, size, k, nextLen))
            {
                end = k;
                break;
            }
        }

        // 去掉文件尾部的 trailing_zero_8bits 填充
        while (end > payloadStart && data[end - 1] == 0)
            --end;

        if (end > payloadStart)
            nals.push_back({data + payloadStart, end - payloadStart});
        pos = end;
    }
    return nals;
}

RtpSource::RtpSource(int payloadType, std::uint32_t clockRate)
    : m_payloadType_(payloadType),
      m_clockRate_(clockRate),
      m_ssrc_(random32()),
      m_seq_(static_cast<std::uint16_t>(random32())),
      m_timestamp_(random32())
{
}

void RtpSource::addSink(const std::shared_ptr<RtpTransport>& sink)
{
    if (!sink)
        return;
    std::lock_guard<std::mutex> lock(m_sinksMutex_);
    for (const auto& weak : m_sinks_)
    {
        if (weak.lock() == sink)
            return;                       // 已订阅
    }
    m_sinks_.emplace_back(sink);
}

void RtpSource::removeSink(const RtpTransport* sink)
{
    if (sink == nullptr)
        return;
    std::lock_guard<std::mutex> lock(m_sinksMutex_);
    for (auto it = m_sinks_.begin(); it != m_sinks_.end(); ++it)
    {
        if (it->lock().get() == sink)
        {
            m_sinks_.erase(it);
            return;
        }
    }
}

void RtpSource::pushFrame(const std::uint8_t* annexB, std::size_t size, std::uint64_t ptsUs)
{
    if (annexB == nullptr || size == 0)
        return;

    const auto nals = splitAnnexB(annexB, size);
    if (nals.empty())
        return;

    const std::uint32_t timestamp = nextTimestamp(ptsUs);
    for (std::size_t i = 0; i < nals.size(); ++i)
        packetizeNal(nals[i], timestamp, i == nals.size() - 1);
}

std::uint32_t RtpSource::nextTimestamp(std::uint64_t ptsUs)
{
    if (ptsUs != 0)
        return static_cast<std::uint32_t>((ptsUs * m_clockRate_) / 1000000u);

    // 无 pts：按 25fps 推进（测试源用）
    m_timestamp_ += m_clockRate_ / 25;
    return m_timestamp_;
}

void RtpSource::packetizeNal(const NalUnit& nal, std::uint32_t timestamp, bool lastNal)
{
    if (nal.size == 0 || nal.data == nullptr)
        return;

    const std::uint8_t header = nal.data[0];

    // 小 NAL 直接单包（RFC 6184 Single NAL Unit）
    if (nal.size <= kMaxRtpPayload)
    {
        emitPacket(timestamp, lastNal, nal.data, nal.size);
        return;
    }

    // 大 NAL 走 FU-A 分片
    const std::uint8_t fuIndicator = static_cast<std::uint8_t>((header & 0xE0u) | 28u);
    const std::uint8_t nalType     = header & 0x1Fu;

    const std::uint8_t* p = nal.data + 1;
    std::size_t remain = nal.size - 1;
    while (remain > 0)
    {
        const std::size_t chunk = std::min(remain, kMaxFuPayload);
        const bool first = (p == nal.data + 1);
        const bool last  = (chunk == remain);

        std::vector<std::uint8_t> payload(2 + chunk);
        payload[0] = fuIndicator;
        payload[1] = static_cast<std::uint8_t>(nalType | (first ? 0x80u : 0u) | (last ? 0x40u : 0u));
        std::memcpy(payload.data() + 2, p, chunk);

        emitPacket(timestamp, lastNal && last, payload.data(), payload.size());
        p += chunk;
        remain -= chunk;
    }
}

void RtpSource::emitPacket(std::uint32_t timestamp, bool marker,
                           const std::uint8_t* payload, std::size_t size)
{
    auto packet = std::make_shared<std::vector<std::uint8_t>>(12 + size);
    std::uint8_t* h = packet->data();

    h[0] = 0x80;                                    // V=2, P=0, X=0, CC=0
    h[1] = static_cast<std::uint8_t>((marker ? 0x80u : 0u) |
                                    (static_cast<unsigned>(m_payloadType_) & 0x7Fu));
    h[2] = static_cast<std::uint8_t>(m_seq_ >> 8);
    h[3] = static_cast<std::uint8_t>(m_seq_ & 0xFFu);
    ++m_seq_;

    h[4] = static_cast<std::uint8_t>(timestamp >> 24);
    h[5] = static_cast<std::uint8_t>(timestamp >> 16);
    h[6] = static_cast<std::uint8_t>(timestamp >> 8);
    h[7] = static_cast<std::uint8_t>(timestamp);

    h[8]  = static_cast<std::uint8_t>(m_ssrc_ >> 24);
    h[9]  = static_cast<std::uint8_t>(m_ssrc_ >> 16);
    h[10] = static_cast<std::uint8_t>(m_ssrc_ >> 8);
    h[11] = static_cast<std::uint8_t>(m_ssrc_);

    if (size > 0)
        std::memcpy(h + 12, payload, size);

    sendToSinks(packet);
}

void RtpSource::sendToSinks(const std::shared_ptr<const std::vector<std::uint8_t>>& packet)
{
    std::vector<std::shared_ptr<RtpTransport>> sinks;
    {
        std::lock_guard<std::mutex> lock(m_sinksMutex_);
        for (const auto& weak : m_sinks_)
        {
            if (const auto sink = weak.lock())
                sinks.push_back(sink);
        }
    }
    for (const auto& sink : sinks)
        sink->deliverRtp(packet);
}
