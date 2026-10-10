#include "sipsession.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "generat_sn.h"
#include "log/log.h"

// GB28181 报文的消息体类型（GB/T 28181-2016 规定 MANSCDP XML）
#define GB_SIP_CONTENT_TYPE "Application/MANSCDP+xml"

SipSession::SipSession()
{
}

SipSession::~SipSession()
{
    uninit();
}

// ---------------------------------------------------------------------------
// 初始化 / 释放
// ---------------------------------------------------------------------------
int SipSession::init(const gb28181_system_param *sys_param)
{
    if (sys_param == nullptr)
    {
        UAV_LOG_WARN << "SipSession::init 传入空参数!\n";
        return -1;
    }
    if (m_inited)
    {
        UAV_LOG_WARN << "SipSession::init 重复调用!\n";
        return -1;
    }

    m_sys_param = *sys_param;

    m_ctx = eXosip_malloc();
    if (m_ctx == nullptr)
    {
        UAV_LOG_WARN << "eXosip_malloc 失败!\n";
        return -1;
    }

    if (eXosip_init(m_ctx) != 0)
    {
        UAV_LOG_WARN << "eXosip_init 失败!\n";
        osip_free(m_ctx);
        m_ctx = nullptr;
        return -1;
    }

    eXosip_set_user_agent(m_ctx, "UAV-Detection-GB28181/1.0");

    // 监听本地 SIP 端口：sip_transport 0=UDP，1=TCP（GB28181-2022 起 TCP 为必选，
    // 平台上若要求 TCP 就把 sip_transport 配成 1）
    int proto = (m_sys_param.sip_transport == 1) ? IPPROTO_TCP : IPPROTO_UDP;
    if (eXosip_listen_addr(m_ctx, proto, nullptr, m_sys_param.sip_localport, AF_INET, 0) != 0)
    {
        UAV_LOG_WARN << "eXosip_listen_addr 失败, port=" << m_sys_param.sip_localport << "\n";
        eXosip_quit(m_ctx);
        osip_free(m_ctx);
        m_ctx = nullptr;
        return -1;
    }

    // 摘要认证信息：GB28181 用设备ID作用户名，密码为注册密码。
    // realm 传 nullptr 时 eXosip 会把它当“任意 realm”的回退项（见 jauth.c
    // eXosip_find_authentication_info），因此平台用平台域做 realm 也能匹配上。
    if (eXosip_add_authentication_info(m_ctx, m_sys_param.sipdev_reg_id, m_sys_param.sipdev_reg_id,
                                       m_sys_param.sipdev_password, nullptr, nullptr) != 0)
    {
        // 不致命：也可能是平台不要求认证，打成警告继续跑
        UAV_LOG_WARN << "eXosip_add_authentication_info 失败(将无法应对 401/407 挑战)\n";
    }

    m_inited = true;
    UAV_LOG_INFO << "SipSession::init 成功, 本地端口=" << m_sys_param.sip_localport
                 << ", 传输=" << (proto == IPPROTO_TCP ? "TCP" : "UDP")
                 << ", 平台=" << m_sys_param.sipserver_ip << ":" << m_sys_param.sipserver_port << "\n";
    return 0;
}

void SipSession::uninit()
{
    if (!m_inited)
    {
        return;
    }

    stop();

    if (m_ctx != nullptr)
    {
        // eXosip_quit 释放内部资源，context 本身要用 osip_free 释放
        eXosip_quit(m_ctx);
        osip_free(m_ctx);
        m_ctx = nullptr;
    }

    m_inited = false;
    m_registered = false;
    m_reg_id = -1;
}

// ---------------------------------------------------------------------------
// 启动 / 停止
// ---------------------------------------------------------------------------
int SipSession::start()
{
    if (!m_inited)
    {
        UAV_LOG_WARN << "SipSession::start 前必须先 init!\n";
        return -1;
    }
    if (m_running.load())
    {
        return 0;
    }

    m_running = true;
    m_thread = std::thread(&SipSession::event_loop, this);
    return 0;
}

void SipSession::stop()
{
    if (m_running.exchange(false))
    {
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }
    else if (m_thread.joinable())
    {
        m_thread.join();
    }

    if (m_inited && m_ctx != nullptr)
    {
        unregister_from_platform();
    }
}

void SipSession::set_command_handler(command_handler_t handler)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_command_handler = std::move(handler);
}

void SipSession::set_media_handler(media_handler_t handler)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_media_handler = std::move(handler);
}

void SipSession::set_media_port_provider(media_port_provider_t provider)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_media_port_provider = std::move(provider);
}

// ---------------------------------------------------------------------------
// 事件循环（信令线程）
// ---------------------------------------------------------------------------
void SipSession::event_loop()
{
    register_to_platform();

    auto last_keepalive = std::chrono::steady_clock::now();

    while (m_running.load())
    {
        eXosip_event_t *ev = eXosip_event_wait(m_ctx, 0, 200);
        if (ev != nullptr)
        {
            // 注册刷新、摘要认证重放、超时重传都由它自动完成
            eXosip_automatic_action(m_ctx);
            handle_event(ev);
            eXosip_event_free(ev);
        }
        else
        {
            eXosip_automatic_action(m_ctx);
        }

        // 心跳：GB28181 约定注册成功后按 heartbeatInterval 周期发 Keepalive MESSAGE
        if (m_registered.load() && m_sys_param.heartbeatInterval > 0)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_keepalive).count();
            if (elapsed >= m_sys_param.heartbeatInterval)
            {
                send_keepalive();
                last_keepalive = now;
            }
        }
    }

    UAV_LOG_INFO << "SipSession 信令线程退出\n";
}

void SipSession::handle_event(eXosip_event_t *ev)
{
    switch (ev->type)
    {
    case EXOSIP_REGISTRATION_SUCCESS:
        m_registered = true;
        UAV_LOG_INFO << "SIP 注册成功, rid=" << ev->rid << "\n";
        break;

    case EXOSIP_REGISTRATION_FAILURE:
        m_registered = false;
        // eXosip_automatic_action 会按退避策略自动重试注册
        UAV_LOG_WARN << "SIP 注册失败(将自动重试), rid=" << ev->rid << "\n";
        break;

    case EXOSIP_MESSAGE_NEW:
        handle_message(ev);
        break;

    case EXOSIP_CALL_INVITE:
    case EXOSIP_CALL_REINVITE:
        handle_invite(ev);
        break;

    case EXOSIP_CALL_ACK:
    {
        // 200 OK 收到 ACK，会话正式建立，可以开始推流
        MediaDialog dialog;
        {
            std::lock_guard<std::mutex> lock(m_dialog_mutex);
            auto it = m_dialogs.find(ev->did);
            if (it != m_dialogs.end())
            {
                dialog = it->second;
            }
        }
        media_handler_t handler;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            handler = m_media_handler;
        }
        if (handler)
        {
            handler(ev->did, dialog.channel_id.c_str(), dialog.remote_sdp.c_str(), true);
        }
        UAV_LOG_INFO << "媒体会话建立(ACK), did=" << ev->did
                     << ", 通道=" << dialog.channel_id << "\n";
        break;
    }

    case EXOSIP_CALL_CLOSED:
    case EXOSIP_CALL_RELEASED:
    {
        MediaDialog dialog;
        {
            std::lock_guard<std::mutex> lock(m_dialog_mutex);
            auto it = m_dialogs.find(ev->did);
            if (it != m_dialogs.end())
            {
                dialog = it->second;
                m_dialogs.erase(it);
            }
        }
        media_handler_t handler;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            handler = m_media_handler;
        }
        if (handler)
        {
            handler(ev->did, dialog.channel_id.c_str(), dialog.remote_sdp.c_str(), false);
        }
        UAV_LOG_INFO << "媒体会话结束, did=" << ev->did << "\n";
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 平台 MESSAGE → XmlPacketParser → 业务回调
// ---------------------------------------------------------------------------
void SipSession::handle_message(eXosip_event_t *ev)
{
    // 先回 200 OK：eXosip 不会自动应答 MESSAGE，平台在等这个响应才会继续
    eXosip_message_send_answer(m_ctx, ev->tid, 200, nullptr);

    osip_body_t *body = nullptr;
    if (ev->request == nullptr ||
        osip_message_get_body(ev->request, 0, &body) != 0 ||
        body == nullptr || body->body == nullptr)
    {
        UAV_LOG_WARN << "收到无消息体的 MESSAGE，已忽略\n";
        return;
    }

    gb28181_message_param_t msg{};
    XmlPacketParser parser;
    if (parser.parse(body->body, (int)strlen(body->body), &msg) != 0)
    {
        UAV_LOG_WARN << "MESSAGE 的 XML 解析失败，已忽略\n";
        return;
    }

    UAV_LOG_INFO << "平台指令: cmd=" << msg.cmd_type << ", sn=" << msg.sn
                 << ", device=" << msg.device_id << "\n";

    command_handler_t handler;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        handler = m_command_handler;
    }
    if (handler)
    {
        // 注意：在信令线程中回调，业务侧耗时应投递到自己的线程
        handler(parser.root_type(), msg);
    }
}

// ---------------------------------------------------------------------------
// INVITE 点播：应答 200 OK + SDP，并把平台 SDP 交给媒体层
// ---------------------------------------------------------------------------
void SipSession::handle_invite(eXosip_event_t *ev)
{
    char remote_sdp[2048] = {0};
    osip_body_t *body = nullptr;
    if (ev->request != nullptr &&
        osip_message_get_body(ev->request, 0, &body) == 0 &&
        body != nullptr && body->body != nullptr)
    {
        snprintf(remote_sdp, sizeof(remote_sdp), "%s", body->body);
    }

    const std::string channelId = extract_channel_id(ev->request);

    // 向媒体层申请本会话的媒体端口（多路并发各自独立）；申请不到就拒绝点播，
    // 不在 SDP 里声明一个实际兑现不了的端口
    int localPort = m_local_media_port;
    {
        media_port_provider_t provider;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            provider = m_media_port_provider;
        }
        if (provider)
        {
            const int allocated = provider(ev->did, channelId.c_str());
            if (allocated <= 0)
            {
                UAV_LOG_WARN << "媒体端口不足，拒绝点播, did=" << ev->did << "\n";
                eXosip_call_send_answer(m_ctx, ev->tid, 503, nullptr);
                return;
            }
            localPort = allocated;
        }
    }

    char answer_sdp[1024] = {0};
    build_sdp_answer(answer_sdp, sizeof(answer_sdp), remote_sdp, localPort);

    osip_message_t *answer = nullptr;
    if (eXosip_call_build_answer(m_ctx, ev->tid, 200, &answer) != 0 || answer == nullptr)
    {
        UAV_LOG_WARN << "eXosip_call_build_answer 失败, did=" << ev->did << "\n";
        eXosip_call_send_answer(m_ctx, ev->tid, 500, nullptr);
        return;
    }

    osip_message_set_content_type(answer, "application/sdp");
    osip_message_set_body(answer, answer_sdp, strlen(answer_sdp));
    if (eXosip_call_send_answer(m_ctx, ev->tid, 200, answer) != 0)
    {
        UAV_LOG_WARN << "eXosip_call_send_answer(200) 失败, did=" << ev->did << "\n";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_dialog_mutex);
        MediaDialog &dialog = m_dialogs[ev->did];
        dialog.channel_id = channelId;
        dialog.remote_sdp = remote_sdp;
        dialog.local_port = localPort;
    }

    UAV_LOG_INFO << "已应答 INVITE(200 OK), did=" << ev->did
                 << ", 通道=" << channelId
                 << ", 媒体端口=" << localPort
                 << ", 平台SDP长度=" << strlen(remote_sdp) << "\n";
    // 真正开始推流等收到 ACK（EXOSIP_CALL_ACK）后再由业务层启动
}

// ---------------------------------------------------------------------------
// 注册 / 注销
// ---------------------------------------------------------------------------
int SipSession::register_to_platform()
{
    char from[128] = {0};
    char to[128] = {0};
    char contact[128] = {0};
    build_local_uris(from, sizeof(from), to, sizeof(to));
    snprintf(contact, sizeof(contact), "sip:%s@%s:%d",
             m_sys_param.sipdev_reg_id, m_sys_param.local_ip, m_sys_param.sip_localport);

    int expires = (m_sys_param.deadline > 0) ? m_sys_param.deadline : 3600;

    osip_message_t *reg = nullptr;
    m_reg_id = eXosip_register_build_initial_register(m_ctx, from, to, contact, expires, &reg);
    if (m_reg_id < 0 || reg == nullptr)
    {
        UAV_LOG_WARN << "eXosip_register_build_initial_register 失败, ret=" << m_reg_id << "\n";
        return -1;
    }

    if (eXosip_register_send_register(m_ctx, m_reg_id, reg) != 0)
    {
        UAV_LOG_WARN << "eXosip_register_send_register 失败\n";
        return -1;
    }

    UAV_LOG_INFO << "已发起 SIP 注册: from=" << from << ", to=" << to
                 << ", expires=" << expires << "\n";
    return 0;
}

int SipSession::unregister_from_platform()
{
    if (m_reg_id < 0 || m_ctx == nullptr)
    {
        return 0;
    }

    osip_message_t *reg = nullptr;
    if (eXosip_register_build_register(m_ctx, m_reg_id, 0, &reg) != 0 || reg == nullptr)
    {
        return -1;
    }

    eXosip_lock(m_ctx);
    int ret = eXosip_register_send_register(m_ctx, m_reg_id, reg);
    eXosip_unlock(m_ctx);

    UAV_LOG_INFO << "已发送注销请求(expires=0)\n";
    return ret;
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------
int SipSession::send_xml(const char *xml_body, int xml_len)
{
    if (!m_inited || m_ctx == nullptr || xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN << "send_xml 参数非法\n";
        return -1;
    }

    char from[128] = {0};
    char to[128] = {0};
    build_local_uris(from, sizeof(from), to, sizeof(to));

    // eXosip 不是线程安全的：业务线程调用时用 lock/unlock 保护
    // （信令线程在 eXosip_event_wait 期间不持有该锁，因此不会自锁）
    eXosip_lock(m_ctx);

    osip_message_t *msg = nullptr;
    int ret = eXosip_message_build_request(m_ctx, &msg, "MESSAGE", to, from, nullptr);
    if (ret != 0 || msg == nullptr)
    {
        eXosip_unlock(m_ctx);
        UAV_LOG_WARN << "eXosip_message_build_request 失败, ret=" << ret << "\n";
        return -1;
    }

    osip_message_set_content_type(msg, GB_SIP_CONTENT_TYPE);
    osip_message_set_body(msg, xml_body, xml_len);
    ret = eXosip_message_send_request(m_ctx, msg);

    eXosip_unlock(m_ctx);

    if (ret < 0)
    {
        UAV_LOG_WARN << "eXosip_message_send_request 失败, ret=" << ret << "\n";
    }
    return ret;
}

int SipSession::send_keepalive()
{
    // 心跳报文很短，用局部 builder，避免与业务线程共享 XmlPacketBuilder 的成员文档
    XmlPacketBuilder builder;
    char xml[1024] = {0};

    std::string sn = GenerateSN::generate();
    int len = builder.xml_pack_keepalive(m_sys_param.sipdev_reg_id, sn.c_str(), xml);
    if (len <= 0)
    {
        UAV_LOG_WARN << "组装心跳报文失败\n";
        return -1;
    }
    return send_xml(xml, len);
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------
// 取 INVITE 的目标通道 ID：优先 Request-URI 的用户名（形如 sip:<通道ID>@...），
// 回退到 To 头；平台点播某个通道时就是用这个字段指定通道
std::string SipSession::extract_channel_id(const osip_message_t *request)
{
    if (request == nullptr)
    {
        return std::string();
    }

    const char *user = nullptr;
    if (request->req_uri != nullptr)
    {
        user = osip_uri_get_username(request->req_uri);
    }
    if ((user == nullptr || *user == '\0') &&
        request->to != nullptr && request->to->url != nullptr)
    {
        user = osip_uri_get_username(request->to->url);
    }

    return (user != nullptr) ? std::string(user) : std::string();
}

// From 用本机设备ID@SIP域；Request-URI/To 用平台ID@平台IP:端口
// （部分平台要求 To 也写成 sip:平台ID@平台域，此时把 to 改成域形式即可）
void SipSession::build_local_uris(char *from, int from_size, char *to, int to_size) const
{
    snprintf(from, from_size, "sip:%s@%s",
             m_sys_param.sipdev_reg_id, m_sys_param.sipserver_region);
    snprintf(to, to_size, "sip:%s@%s:%d",
             m_sys_param.sipserver_id, m_sys_param.sipserver_ip, m_sys_param.sipserver_port);
}

// 组装 SDP 应答：GB28181 实时点播为 PS/90000，设备侧 a=sendonly。
// TCP 传输时按平台的 setup 属性做反向应答（平台 active → 设备 passive）。
int SipSession::build_sdp_answer(char *sdp, int sdp_size, const char *remote_sdp, int local_rtp_port) const
{
    bool tcp = (remote_sdp != nullptr) && (strstr(remote_sdp, "TCP/RTP/AVP") != nullptr);
    bool peer_passive = (remote_sdp != nullptr) && (strstr(remote_sdp, "a=setup:passive") != nullptr);

    // SSRC：GB28181 约定 10 位十进制，实时流以 0 开头（回放以 1 开头）
    char ssrc[16] = {0};
    snprintf(ssrc, sizeof(ssrc), "%010u", (unsigned int)time(nullptr) % 1000000000u);

    int n = snprintf(sdp, sdp_size,
        "v=0\r\n"
        "o=%s 0 0 IN IP4 %s\r\n"
        "s=Play\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=video %d %s 96\r\n"
        "a=sendonly\r\n"
        "a=rtpmap:96 PS/90000\r\n"
        "%s"
        "y=%s\r\n"
        "f=v/2/5/25/1/4000a\r\n",
        m_sys_param.sipdev_reg_id, m_sys_param.local_ip, m_sys_param.local_ip,
        local_rtp_port,
        tcp ? "TCP/RTP/AVP" : "RTP/AVP",
        tcp ? (peer_passive ? "a=setup:active\r\na=connection:new\r\n"
                            : "a=setup:passive\r\na=connection:new\r\n") : "",
        ssrc);

    return (n > 0 && n < sdp_size) ? n : -1;
}
