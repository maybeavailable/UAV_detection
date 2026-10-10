#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gb28181type.h"
#include "ps_mux.h"
#include "ps_rtp_pusher.h"

// ---------------------------------------------------------------------------
// GB28181 媒体会话管理：把 SIP 协商结果接到 PS/RTP 推流上
//
// 职责：
//   1. 解析平台 INVITE 里的 SDP（c= 地址、m=video 端口与传输协议、y= SSRC、
//      a=setup 主被动），换算成 PsRtpPusher 的配置；
//   2. 按会话（did）管理推流器生命周期：ACK 后 startSession、BYE 后 stopSession；
//   3. 把编码线程产出的 H.264/H.265 帧按通道分发给对应会话，
//      一路帧可同时推给多个点播该通道的平台会话。
//
// 线程约定：
//   startSession/stopSession/sessions  由信令线程调用；
//   pushFrame                          由编码（采集）线程调用；
//   两者之间用互斥锁保护会话表，实际发送在锁外进行（避免阻塞信令）。
//
// 典型用法：
//   Gb28181Media media;
//   media.setChannels(&sys_param);
//   sip.set_media_handler([&](int did, const char *ch, const char *sdp, bool start) {
//       start ? media.startSession(did, ch, sdp) : media.stopSession(did);
//   });
//   // 编码线程：
//   media.pushFrame(channelIndex, annexB, size, ptsUs, isKey);
// ---------------------------------------------------------------------------
class Gb28181Media
{
public:
    struct Config
    {
        // 媒体端口池：每路点播会话独占一个端口，SDP 应答里声明的端口就是该会话真实使用的端口。
        // 固定用单个端口在多路并发时必然冲突（TCP 被动模式第二路直接失败），故改为池化分配。
        int             portBase{LOCAL_MEDIA_PORT};       // 池起始端口
        int             portCount{16};                    // 池大小（可支持的同时点播路数）
        int             mtu{1400};                        // RTP 包大小上限
        PsMuxer::Config ps{};                             // 编码类型/是否补 AUD 等
        // TCP 上的 RTP 定界方式：对端按 RFC4571 解则选 Rfc4571，国标设备默认裸发
        PsRtpPusher::TcpFraming tcpFraming{PsRtpPusher::TcpFraming::Raw};
    };

    // 平台 SDP 解析结果
    struct RemoteMedia
    {
        bool          valid{false};
        std::string   ip;             // c= 行地址
        std::uint16_t port{0};        // m=video 端口
        std::uint32_t ssrc{0};        // y= 行 SSRC（十进制）
        bool          tcp{false};     // m= 行协议是否 TCP
        bool          setupPassive{false};  // 平台声明 a=setup:passive（设备需主动连）
        bool          playback{false};      // s=Playback/Download（回放/下载）
        std::string   sessionName;    // s= 行原文
    };

    struct SessionSnapshot
    {
        int                  did{0};
        std::string          channelId;
        RemoteMedia          media;
        bool                 connected{false};
        PsRtpPusher::Stats   stats;
    };

    Gb28181Media() = default;
    ~Gb28181Media();

    Gb28181Media(const Gb28181Media &) = delete;
    Gb28181Media &operator=(const Gb28181Media &) = delete;

    void setConfig(const Config &config) { m_cfg = config; }
    const Config &config() const { return m_cfg; }

    // 登记通道：把 system_param 里「通道序号 → 通道国标ID」的对应关系记下来，
    // pushFrame 靠它把某一路视频路由到点播该通道的会话
    void setChannels(const gb28181_system_param *sys_param);

    // 解析平台 SDP（静态方法，便于单独测试）
    static RemoteMedia parseSdp(const char *sdp);

    // ---- 媒体端口分配（信令线程调用）----
    // INVITE 应答前调用：为该会话分配一个媒体端口，用于写进 SDP 应答；
    // 池满时返回 -1（调用方应回 503/488 或退回默认端口）
    int  allocPort(int did, const char *channelId);
    // 释放该会话占用的端口（会话结束/应答失败时调用）
    void freePort(int did);
    // 查询已分配的端口，未分配返回 -1
    int  portOf(int did) const;

    // ---- 会话生命周期（信令线程调用）----
    // 平台点播已建立（ACK）：按 SDP + 已分配端口创建推流通道。成功返回 0
    int  startSession(int did, const char *channelId, const char *remoteSdp);
    // 会话结束：关闭推流通道
    void stopSession(int did);
    void stopAll();

    // ---- 视频帧入口（编码线程调用）----
    // channelIndex：通道序号（与 camera_param_t::channel_index 一致）
    // 返回实际发送成功的会话数（0 表示当前无人点播该通道，属正常情况）
    int  pushFrame(int channelIndex, const std::uint8_t *annexB, std::size_t size,
                   std::uint64_t ptsUs, bool keyFrame);

    // 当前所有会话快照（调试/日志用）
    std::vector<SessionSnapshot> sessions() const;
    std::size_t sessionCount() const;

private:
    struct Session
    {
        int                          did{0};
        std::string                  channelId;
        RemoteMedia                  media;
        std::shared_ptr<PsRtpPusher> pusher;
    };

    struct Config m_cfg{};
    // 通道序号 → 通道国标ID
    std::map<int, std::string> m_channelIds;
    std::map<int, std::shared_ptr<Session>> m_sessions;
    // 会话端口分配表：did → 端口
    std::map<int, int> m_ports;
    int m_nextPortOffset{0};
    mutable std::mutex m_mutex;
};
