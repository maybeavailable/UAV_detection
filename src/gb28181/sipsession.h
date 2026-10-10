#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

// eXosip2 头文件自带 __cplusplus 的 extern "C" 保护，可直接被 C++ 包含
// （头文件路径由 cmake/FindEXosip2.cmake 提供）
#include <eXosip2/eXosip.h>

#include "gb28181type.h"
#include "xmlpacket.h"
#include "xmlpacketparser.h"

// ---------------------------------------------------------------------------
// GB28181 SIP 信令会话（设备侧 UAC）
//
// 基于 eXosip2，负责与平台之间的 SIP 信令：
//   * REGISTER 注册/注销与 401/407 摘要认证重放（eXosip 自动处理）
//   * 接收平台 MESSAGE（查询/控制指令）→ XmlPacketParser 解析 → 回调业务
//   * 发送设备侧 MESSAGE（心跳/目录/报警/位置/各类应答）→ XmlPacketBuilder 组包
//   * 接收 INVITE 点播请求，应答 200 OK + SDP（PS/90000），ACK/BYE 管理媒体会话
//
// 线程模型：
//   * 内部只有一个信令线程跑 eXosip 事件循环；注册刷新、心跳、重传都在该线程完成；
//   * send_xml() 允许从任意业务线程调用（内部用 eXosip_lock/unlock 保护）；
//   * set_command_handler() 的回调**在信令线程中执行**，耗时操作（推流、写盘、
//     查数据库）请自行投递到业务线程，避免阻塞 SIP 收发。
//
// 用法：
//   SipSession sip;
//   sip.set_command_handler([&](gb_xml_root_e root, const gb28181_message_param_t &msg) { ... });
//   sip.init(&sys_param);
//   sip.start();          // 注册 + 心跳
//   ...
//   sip.stop();
//   sip.uninit();
// ---------------------------------------------------------------------------
class SipSession
{
public:
    // 平台指令回调：root_type 区分 Query/Control/Response，msg 为解析结果
    using command_handler_t = std::function<void(gb_xml_root_e root_type, const gb28181_message_param_t &msg)>;
    // 媒体会话回调：
    //   start=true  平台点播已应答 200 OK 并收到 ACK，可以开始推流
    //   start=false 会话结束（收到 BYE 或会话释放），应停止推流
    // channelId ：平台点播的目标通道 ID（取自 INVITE 的 Request-URI/To 用户名）
    // remoteSdp ：平台下发的 SDP 原文（地址/端口/SSRC/传输方式都在里面）
    // did       ：eXosip 会话 ID，会话内唯一
    using media_handler_t = std::function<void(int did, const char *channelId,
                                               const char *remoteSdp, bool start)>;
    // 媒体端口提供者：INVITE 应答前调用，返回要写进 SDP 应答的本地媒体端口。
    // 由媒体层按会话分配（多路并发才不会抢端口）；返回 <=0 表示端口不足，
    // 此时设备直接回 503，避免在 SDP 里声明一个兑现不了的端口。
    using media_port_provider_t = std::function<int(int did, const char *channelId)>;

    SipSession();
    ~SipSession();

    SipSession(const SipSession &) = delete;
    SipSession &operator=(const SipSession &) = delete;

    // 初始化 SIP 上下文（监听本地端口、登记摘要认证信息），不发起注册
    int init(const gb28181_system_param *sys_param);
    // 释放 SIP 上下文（内部会先 stop）
    void uninit();

    // 启动信令线程：发起注册 + 周期心跳 + 事件循环
    int start();
    // 停止信令线程并注销注册
    void stop();

    bool is_registered() const { return m_registered.load(); }

    void set_command_handler(command_handler_t handler);
    void set_media_handler(media_handler_t handler);

    // SDP 应答里声明的本地媒体端口（未设置 provider 时使用，默认 LOCAL_MEDIA_PORT）
    void set_local_media_port(int port) { m_local_media_port = port; }
    int  local_media_port() const { return m_local_media_port; }
    // 设置媒体端口提供者（推荐：由 Gb28181Media 的端口池按会话分配）
    void set_media_port_provider(media_port_provider_t provider);

    // 发送一条 GB28181 XML MESSAGE（线程安全）
    int send_xml(const char *xml_body, int xml_len);
    // 发送心跳（信令线程也会按 heartbeatInterval 自动发送）
    int send_keepalive();

private:
    // 平台点播的会话信息（did → 通道 + SDP），ACK/BYE 时回传给业务层
    struct MediaDialog
    {
        std::string channel_id;
        std::string remote_sdp;
        int         local_port{0};        // 该会话在 SDP 应答里声明的媒体端口
    };

    void event_loop();
    void handle_event(eXosip_event_t *ev);
    void handle_message(eXosip_event_t *ev);
    void handle_invite(eXosip_event_t *ev);
    int  register_to_platform();
    int  unregister_from_platform();
    int  build_sdp_answer(char *sdp, int sdp_size, const char *remote_sdp, int local_rtp_port) const;
    void build_local_uris(char *from, int from_size, char *to, int to_size) const;
    // 取 INVITE 的目标通道 ID（Request-URI 用户名，回退到 To 头）
    static std::string extract_channel_id(const osip_message_t *request);

    struct eXosip_t *m_ctx{nullptr};
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_registered{false};
    int               m_reg_id{-1};
    bool              m_inited{false};
    int               m_local_media_port{LOCAL_MEDIA_PORT};

    gb28181_system_param m_sys_param{};

    command_handler_t     m_command_handler;
    media_handler_t       m_media_handler;
    media_port_provider_t m_media_port_provider;
    std::mutex        m_handler_mutex;
    // did → 点播会话信息（INVITE 时记录，ACK 后交给媒体层，会话结束清除）
    std::map<int, MediaDialog> m_dialogs;
    std::mutex                 m_dialog_mutex;
};
