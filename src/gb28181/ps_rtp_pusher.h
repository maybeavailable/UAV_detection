#pragma once

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "ps_mux.h"

// ---------------------------------------------------------------------------
// GB28181 媒体发送：H.264/H.265 帧 → PS → RTP → 平台
//
// 报文结构：12 字节 RTP 固定头 + PS 数据（GB/T 28181-2016 附录 C 规定
// 视频用 PS 流封装在 RTP 里，负载类型 96、时钟频率 90kHz）。
//   * 每个 RTP 包只承载 PS 流的一段，按 mtu 分片（默认 1400 字节含头）
//   * 一帧的最后一个 RTP 包置 marker 位
//   * 时间戳取该帧的 PTS（90kHz），同一帧的所有分片时间戳相同
//
// 传输方式：
//   * UDP（默认）：sendto 到 SDP 协商出的平台地址/端口，一个 UDP 报文就是一个 RTP 包
//   * TCP（GB28181-2022 要求支持）：connect 后按序发送。
//     TCP 是字节流、RTP 头里没有长度字段，接收方定界方式各平台不一致，故提供两种：
//       Raw      —— 裸发 RTP（多数国标设备/平台的默认做法）
//       Rfc4571 —— 每个 RTP 包前加 2 字节大端长度前缀（IETF RFC 4571，接收端可精确定界）
//     对接具体平台时按实测结果选择。
//
// 目标地址/端口/SSRC 来自 SIP INVITE 的 SDP（c= 行地址、m=video 端口、y= 行 SSRC），
// 本类不做 SDP 解析，由调用方填 Config。
// ---------------------------------------------------------------------------
class PsRtpPusher
{
public:
    // TCP 上的 RTP 定界方式
    enum class TcpFraming
    {
        Raw = 0,     // 裸发 RTP 包
        Rfc4571      // 每包前置 2 字节大端长度
    };

    // TCP 角色：谁主动建连由 SDP 的 a=setup 决定
    enum class TcpMode
    {
        Client = 0,  // 主动连接平台（设备在 SDP 里答 a=setup:active）
        Server       // 监听本地端口等平台来连（设备答 a=setup:passive，国标常见做法）
    };

    struct Config
    {
        std::string   dstIp;              // 媒体接收方地址（SDP c= 行）
        std::uint16_t dstPort{0};         // 媒体接收方端口（SDP m=video 行）
        std::uint32_t ssrc{0};            // SSRC（SDP y= 行；为 0 时自动生成）
        std::uint8_t  payloadType{96};    // GB28181 固定 96
        std::uint16_t mtu{1400};          // 单个 RTP 包总长上限（含 12 字节头）
        bool          tcp{false};         // true=TCP，false=UDP
        TcpMode       tcpMode{TcpMode::Client};
        TcpFraming    tcpFraming{TcpFraming::Raw};
        int           localPort{0};       // UDP 本地端口 / TCP Server 的监听端口
        PsMuxer::Config ps{};             // PS 封装参数（H.264/H.265、是否补 AUD 等）
    };

    struct Stats
    {
        std::uint64_t frames{0};          // 已发送帧数
        std::uint64_t psBytes{0};         // 已发送 PS 字节数
        std::uint64_t rtpPackets{0};      // 已发送 RTP 包数
        std::uint64_t rtpBytes{0};        // 已发送字节数（含 RTP 头）
        std::uint64_t errors{0};          // 发送失败次数
        std::uint64_t skipped{0};         // 连接尚未建立而丢弃的帧数（仅 TCP Server）
    };

    PsRtpPusher() = default;
    ~PsRtpPusher();

    PsRtpPusher(const PsRtpPusher &) = delete;
    PsRtpPusher &operator=(const PsRtpPusher &) = delete;

    // 建立发送通道：UDP 建 socket；TCP Client 直接连接；TCP Server 绑定监听等平台连入
    int open(const Config &config);
    void close();
    // UDP / TCP Client：socket 已就绪；TCP Server：监听已就绪（对端可能还没连上）
    bool isOpen() const { return m_fd >= 0 || m_listenFd >= 0; }
    // TCP Server 模式下对端是否已连接
    bool isConnected() const { return m_fd >= 0; }

    // 发送一帧（Annex-B H.264/H.265，通常一帧一次调用）。
    // ptsUs：微秒；keyFrame：IDR 帧（会重发 PS 头/系统头/PSM）。
    // 返回本次写入的 PS 字节数，失败返回 -1。
    int pushFrame(const std::uint8_t *annexB, std::size_t size,
                  std::uint64_t ptsUs, bool keyFrame);

    const Config &config() const { return m_cfg; }
    const Stats  &stats() const { return m_stats; }
    void resetStats() { m_stats = Stats{}; }
    std::uint16_t lastSequence() const { return m_seq; }
    std::uint32_t ssrc() const { return m_ssrc; }

private:
    // 把一个 PS 缓冲按 mtu 切成 RTP 包发出；最后一包置 marker
    int sendPackets(const std::uint8_t *ps, std::size_t len, std::uint32_t timestamp);
    int sendAll(const std::uint8_t *data, std::size_t len) const;
    // TCP Server：非阻塞 accept 平台连入
    void tryAccept();

    Config  m_cfg{};
    PsMuxer m_muxer{};

    int           m_fd{-1};          // 实际发送用的 socket（UDP / TCP Client / 已接受的连接）
    int           m_listenFd{-1};    // TCP Server 的监听 socket
    sockaddr_in   m_dst{};
    std::uint32_t m_ssrc{0};
    std::uint16_t m_seq{0};

    std::vector<std::uint8_t> m_psBuf;    // 复用缓冲，避免每帧分配
    std::vector<std::uint8_t> m_rtpBuf;

    Stats m_stats{};
};
