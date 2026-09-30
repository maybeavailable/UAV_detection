#pragma once

#include"common/common.h"

#include<cstdint>
#include<memory>
#include<vector>

class RtpTransport;

// Annex-B NAL 片段（指向外部缓冲，不持有内存）
struct NalUnit
{
    const std::uint8_t* data{nullptr};
    std::size_t         size{0};
};

// 按 00 00 01 / 00 00 00 01 起启码切分 Annex-B 裸流
std::vector<NalUnit> splitAnnexB(const std::uint8_t* data, std::size_t size);

// 一路媒体源：encode 模块产出 H.264 后调用 pushFrame，
// 由本类完成 RTP 打包（RFC 6184 单包/FU-A 分片），
// 再把同一份 RTP 包 fan-out 转发给所有订阅的拉流客户端（"握手成功后直接转发"）。
//
// 线程约定：pushFrame 允许从单一生产者线程（encode 线程）调用；
// 订阅/退订（addSink/removeSink）可从任意线程调用。
class RtpSource
{
public:
    explicit RtpSource(int payloadType = 96, std::uint32_t clockRate = 90000);
    virtual ~RtpSource() = default;

    // ---- 描述信息（DESCRIBE 回复用）----
    virtual std::string path() const = 0;
    virtual std::string makeSdp(const std::string& host, int port) const = 0;

    std::uint32_t ssrc() const { return m_ssrc_; }

    // ---- 订阅管理（PLAY / PAUSE / TEARDOWN / 断连）----
    void addSink(const std::shared_ptr<RtpTransport>& sink);
    void removeSink(const RtpTransport* sink);

    // ---- 推流入口：一帧 H.264（Annex-B，可含多个 NAL）----
    // ptsUs：微秒；传 0 则按 25fps 自增
    void pushFrame(const std::uint8_t* annexB, std::size_t size, std::uint64_t ptsUs);

private:
    void packetizeNal(const NalUnit& nal, std::uint32_t timestamp, bool lastNal);
    void emitPacket(std::uint32_t timestamp, bool marker,
                    const std::uint8_t* payload, std::size_t size);
    void sendToSinks(const std::shared_ptr<const std::vector<std::uint8_t>>& packet);
    std::uint32_t nextTimestamp(std::uint64_t ptsUs);

    std::mutex m_sinksMutex_;
    std::vector<std::weak_ptr<RtpTransport>> m_sinks_;

    const int          m_payloadType_;
    const std::uint32_t m_clockRate_;
    std::uint32_t m_ssrc_;
    std::uint16_t m_seq_;
    std::uint32_t m_timestamp_;
};
