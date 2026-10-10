#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "gb28181_media.h"
#include "gb28181type.h"
#include "sipsession.h"
#include "xmlpacket.h"
#include "xmlpacketparser.h"

// ---------------------------------------------------------------------------
// GB28181 设备侧门面：把 SIP 信令、XML 组包/解包、PS/RTP 推流串成一个整体。
//
// 本版本按“视频 + 云台”范围实现（报警上报、回放、语音对讲、目录订阅暂不做）：
//
//   平台查询 → 设备应答（自动）
//     Catalog       设备目录（通道列表，状态由 channel_status_provider 提供）
//     DeviceInfo    设备信息
//     DeviceStatus  设备状态（由 status_provider 提供，未设置则用默认值）
//     PresetQuery   预置位列表（由 preset_provider 提供）
//     DeviceConfig / ConfigDownload / RecordInfo(空列表)
//
//   平台控制 → 设备执行 + 应答
//     DeviceControl  方向/停止/变倍(ZOOM_*)/变焦(FOCUS_*)/预置位 → ptz_handler
//     其余指令(Guard/TeleBoot/Broadcast/非标扩展...)：交给 command_hook，
//     未注册 hook 时如实回 Result=ERROR（而不是假装成功）
//
//   视频：平台 INVITE 点播 → 200 OK(SDP) → ACK → 会话建立 → pushFrame 喂帧即推流；
//         BYE 自动停流。多路并发点播由端口池分配各自端口。
//
// 线程约定：
//   * init/start/stop/uninit 与各 set_*_handler 由控制线程调用；
//   * pushFrame 由编码（采集）线程调用；
//   * ptz_handler / *_provider 在信令线程被回调，请勿阻塞（投递到业务线程即可）。
// ---------------------------------------------------------------------------
class Gb28181Device
{
public:
    struct Config
    {
        gb28181_system_param sys{};      // 平台/本机/通道配置（sipdev_reg_id、channel_para 等）
        Gb28181Media::Config media{};    // 媒体端口池、编码类型、RTP 参数
    };

    // 云台回调：channelIndex 为匹配到的通道序号（-1 表示未匹配），
    // cmd 含 PTZ_CMD_LEFT/TOP/... 与变倍 PTZ_CMD_ZOOM_ADD/ZOOM_REDUCE、
    // 变焦 PTZ_CMD_FOCUS_ADD/FOCUS_REDUCE、预置位动作等；
    // speed 为 0x00-0xFF 原始速度（业务侧自行映射到相机档位）；
    // presetNum 仅预置位动作用。返回 true → 应答 Result=OK
    using ptz_handler_t = std::function<bool(int channelIndex, ptz_cmd_e cmd, int speed, int presetNum)>;

    // 设备状态提供者（DeviceStatus 查询用）；未设置则用默认值（在线/编码中/不录像）
    using status_provider_t = std::function<void(gb_device_status_t &status)>;
    // 预置位提供者（PresetQuery 用）：填 msg_param 的 preset_num/preset_id/preset_name，返回数量
    using preset_provider_t = std::function<int(gb28181_message_param_t &msg_param)>;
    // 通道在线状态提供者（Catalog 用）：填 channel_status[GB_CAMERA_MAX]，1 在线 0 离线
    using channel_status_provider_t = std::function<void(int *channel_status)>;
    // 兜底指令钩子：本类未内置处理的指令会走到这里；
    // 返回 true 表示已处理（回 Result=OK），false 或未注册则回 Result=ERROR
    using command_hook_t = std::function<bool(gb_xml_root_e root, const gb28181_message_param_t &msg)>;

    Gb28181Device() = default;
    ~Gb28181Device();

    Gb28181Device(const Gb28181Device &) = delete;
    Gb28181Device &operator=(const Gb28181Device &) = delete;

    // 初始化（不发起注册）。成功返回 0
    int  init(const Config &config);
    // 启动信令线程：注册 + 心跳 + 事件循环（媒体随点播自动启停）
    int  start();
    // 停止信令并关闭所有推流会话
    void stop();
    // 释放（内部会先 stop）
    void uninit();

    bool is_registered() const { return m_sip.is_registered(); }
    std::size_t session_count() const { return m_media.sessionCount(); }

    // 视频帧入口（编码线程调用）
    int  pushFrame(int channelIndex, const std::uint8_t *annexB, std::size_t size,
                   std::uint64_t ptsUs, bool keyFrame);

    void set_ptz_handler(ptz_handler_t handler);
    void set_status_provider(status_provider_t provider);
    void set_preset_provider(preset_provider_t provider);
    void set_channel_status_provider(channel_status_provider_t provider);
    void set_command_hook(command_hook_t hook);

    SipSession   &sip() { return m_sip; }
    Gb28181Media &media() { return m_media; }

private:
    // 平台指令分发（在信令线程被回调）
    void on_command(gb_xml_root_e root, const gb28181_message_param_t &msg);
    // 通道国标ID → 通道序号（找不到返回 -1）
    int  channel_index_of(const std::string &channelId) const;
    // 云台指令：交给 ptz_handler 执行并回应答
    void handle_device_control(const gb28181_message_param_t &msg, char *out_buff);
    // 未内置处理的指令：走 hook，未处理则如实回 ERROR
    void handle_fallback(gb_xml_root_e root, const gb28181_message_param_t &msg,
                         char *out_buff);
    void fill_default_status(gb_device_status_t &status) const;

    Config       m_cfg{};
    SipSession   m_sip;
    Gb28181Media m_media;
    bool         m_inited{false};

    ptz_handler_t               m_ptz;
    status_provider_t           m_status;
    preset_provider_t           m_preset;
    channel_status_provider_t   m_channel_status;
    command_hook_t              m_hook;
    mutable std::mutex          m_handler_mutex;
};
