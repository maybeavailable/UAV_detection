#include "gb28181device.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "log/log.h"

// 平台查询/控制报文的应答缓冲：Catalog 含全部通道，ConfigDownload 可能较大
static const int GB_XML_BUFF_SIZE = 16 * 1024;

Gb28181Device::~Gb28181Device()
{
    uninit();
}

// ---------------------------------------------------------------------------
// 初始化 / 启停
// ---------------------------------------------------------------------------
int Gb28181Device::init(const Config &config)
{
    if (m_inited)
    {
        UAV_LOG_WARN << "Gb28181Device::init 重复调用\n";
        return -1;
    }

    m_cfg = config;
    m_media.setConfig(m_cfg.media);
    m_media.setChannels(&m_cfg.sys);

    // SDP 应答里声明的端口由媒体层按会话分配（多路并发各自独立）；
    // 端口池满时 SipSession 会直接回 503，不会声明兑现不了的端口
    m_sip.set_local_media_port(m_cfg.media.portBase);
    m_sip.set_media_port_provider([this](int did, const char *channelId) {
        return m_media.allocPort(did, channelId);
    });

    // 平台指令 → 分发
    m_sip.set_command_handler([this](gb_xml_root_e root, const gb28181_message_param_t &msg) {
        on_command(root, msg);
    });

    // 点播会话建立/结束 → 启停推流
    m_sip.set_media_handler([this](int did, const char *channelId, const char *remoteSdp, bool start) {
        if (start)
        {
            m_media.startSession(did, channelId, remoteSdp);
        }
        else
        {
            m_media.stopSession(did);
        }
    });

    if (m_sip.init(&m_cfg.sys) != 0)
    {
        UAV_LOG_WARN << "Gb28181Device::init SIP 初始化失败\n";
        return -1;
    }

    m_inited = true;
    UAV_LOG_INFO << "GB28181 设备初始化完成: 设备ID=" << m_cfg.sys.sipdev_reg_id
                 << ", 通道数=" << m_cfg.sys.channel_size
                 << ", 媒体端口池=" << m_cfg.media.portBase
                 << "-" << (m_cfg.media.portBase + m_cfg.media.portCount - 1) << "\n";
    return 0;
}

int Gb28181Device::start()
{
    if (!m_inited)
    {
        UAV_LOG_WARN << "Gb28181Device::start 前必须先 init\n";
        return -1;
    }
    return m_sip.start();
}

void Gb28181Device::stop()
{
    m_sip.stop();
    m_media.stopAll();
}

void Gb28181Device::uninit()
{
    if (!m_inited)
    {
        return;
    }
    m_sip.stop();
    m_media.stopAll();
    m_sip.uninit();
    m_inited = false;
}

int Gb28181Device::pushFrame(int channelIndex, const std::uint8_t *annexB, std::size_t size,
                             std::uint64_t ptsUs, bool keyFrame)
{
    return m_media.pushFrame(channelIndex, annexB, size, ptsUs, keyFrame);
}

// ---------------------------------------------------------------------------
// 回调注册
// ---------------------------------------------------------------------------
void Gb28181Device::set_ptz_handler(ptz_handler_t handler)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_ptz = std::move(handler);
}

void Gb28181Device::set_status_provider(status_provider_t provider)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_status = std::move(provider);
}

void Gb28181Device::set_preset_provider(preset_provider_t provider)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_preset = std::move(provider);
}

void Gb28181Device::set_channel_status_provider(channel_status_provider_t provider)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_channel_status = std::move(provider);
}

void Gb28181Device::set_command_hook(command_hook_t hook)
{
    std::lock_guard<std::mutex> lock(m_handler_mutex);
    m_hook = std::move(hook);
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------
int Gb28181Device::channel_index_of(const std::string &channelId) const
{
    if (channelId.empty())
    {
        return -1;
    }
    for (int i = 0; i < m_cfg.sys.channel_size && i < GB_CAMERA_MAX; ++i)
    {
        if (channelId == m_cfg.sys.channel_para[i].channel_id)
        {
            return m_cfg.sys.channel_para[i].channel_index;
        }
    }
    return -1;
}

void Gb28181Device::fill_default_status(gb_device_status_t &status) const
{
    memset(&status, 0, sizeof(status));
    snprintf(status.online, sizeof(status.online), "%s", "ONLINE");
    snprintf(status.status, sizeof(status.status), "%s", "OK");
    snprintf(status.encode, sizeof(status.encode), "%s", "ON");
    snprintf(status.record, sizeof(status.record), "%s", "OFF");

    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    strftime(status.device_time, sizeof(status.device_time), "%Y-%m-%dT%H:%M:%S", &tm_buf);
}

// ---------------------------------------------------------------------------
// 云台：方向/停止/变倍/变焦/预置位
// ---------------------------------------------------------------------------
void Gb28181Device::handle_device_control(const gb28181_message_param_t &msg, char *out_buff)
{
    const int channelIndex = channel_index_of(msg.device_id);

    ptz_handler_t handler;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        handler = m_ptz;
    }

    bool ok = false;
    if (handler)
    {
        // speed 为 GB28181 的 0x00-0xFF 原始值，业务侧自行映射到相机档位
        ok = handler(channelIndex, msg.ptz_cmd, msg.ptz_speed, msg.ptz_preset_num);
    }
    else
    {
        UAV_LOG_WARN << "收到云台指令但未注册 ptz_handler，回 ERROR: "
                     << "cmd=" << static_cast<int>(msg.ptz_cmd)
                     << ", speed=" << msg.ptz_speed << "\n";
    }

    gb28181_message_param_t ack = msg;
    ack.ptz_status = ok ? 0 : 1;      // 0=OK 其余=ERROR
    XmlPacketBuilder builder;
    const int len = builder.xml_pack_ptz_ack(&ack, out_buff);
    if (len > 0)
    {
        m_sip.send_xml(out_buff, len);
    }
    UAV_LOG_INFO << "云台指令: 通道=" << channelIndex
                 << ", cmd=" << static_cast<int>(msg.ptz_cmd)
                 << ", speed=" << msg.ptz_speed
                 << ", preset=" << msg.ptz_preset_num
                 << (ok ? " → OK" : " → ERROR") << "\n";
}

// ---------------------------------------------------------------------------
// 未内置处理的指令
// ---------------------------------------------------------------------------
void Gb28181Device::handle_fallback(gb_xml_root_e root, const gb28181_message_param_t &msg,
                                    char *out_buff)
{
    command_hook_t hook;
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        hook = m_hook;
    }

    bool handled = false;
    if (hook)
    {
        handled = hook(root, msg);
    }

    if (!handled)
    {
        UAV_LOG_INFO << "指令未实现，按不支持应答(Result=ERROR): cmd=" << msg.cmd_type << "\n";
    }

    XmlPacketBuilder builder;
    const int len = builder.xml_pack_common_ack(msg.cmd_type, msg.device_id,
                                                handled ? 0 : 1, msg.sn, out_buff);
    if (len > 0)
    {
        m_sip.send_xml(out_buff, len);
    }
}

// ---------------------------------------------------------------------------
// 指令分发（信令线程）
// ---------------------------------------------------------------------------
void Gb28181Device::on_command(gb_xml_root_e root, const gb28181_message_param_t &msg)
{
    const std::string cmd = msg.cmd_type;
    char xml[GB_XML_BUFF_SIZE] = {0};
    XmlPacketBuilder builder;
    int len = -1;

    // ---------------- 查询类：设备应答 ----------------
    if (cmd == "Catalog")
    {
        int channelStatus[GB_CAMERA_MAX] = {0};
        channel_status_provider_t provider;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            provider = m_channel_status;
        }
        if (provider)
        {
            provider(channelStatus);
        }
        else
        {
            for (int i = 0; i < GB_CAMERA_MAX; ++i)
            {
                channelStatus[i] = 1;    // 默认全部在线
            }
        }
        len = builder.xml_pack_catalog(&m_cfg.sys, channelStatus, msg.sn, xml);
    }
    else if (cmd == "DeviceInfo")
    {
        len = builder.xml_pack_deviceinfo(&m_cfg.sys, msg.sn, xml);
    }
    else if (cmd == "DeviceStatus")
    {
        gb_device_status_t status{};
        status_provider_t provider;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            provider = m_status;
        }
        if (provider)
        {
            provider(status);
        }
        else
        {
            fill_default_status(status);
        }
        len = builder.xml_pack_devicestatus(msg.device_id, &status, msg.sn, xml);
    }
    else if (cmd == "PresetQuery")
    {
        gb28181_message_param_t out = msg;
        preset_provider_t provider;
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            provider = m_preset;
        }
        out.preset_num = provider ? provider(out) : 0;
        if (out.preset_num < 0)
        {
            out.preset_num = 0;
        }
        len = builder.xml_pack_preset_query(&out, xml);
    }
    else if (cmd == "DeviceConfig")
    {
        len = builder.xml_pack_deviceconfig(&m_cfg.sys, msg.sn, xml);
    }
    else if (cmd == "ConfigDownload")
    {
        // 未做具体配置项，回一个空的配置节点（平台一般只关心 Result）
        const char *configName = (msg.config_type[0] != '\0') ? msg.config_type : "BasicParam";
        len = builder.xml_pack_configdownload(msg.device_id, configName, "", msg.sn, xml);
    }
    else if (cmd == "RecordInfo")
    {
        // 本设备不做本地录像，回空列表（SumNum=0）
        len = builder.xml_pack_recordinfo(msg.device_id, nullptr, 0, msg.sn, xml);
    }

    // ---------------- 控制类：云台 ----------------
    else if (cmd == "DeviceControl")
    {
        handle_device_control(msg, xml);
        return;
    }

    // ---------------- 其余指令 ----------------
    else
    {
        handle_fallback(root, msg, xml);
        return;
    }

    if (len > 0)
    {
        m_sip.send_xml(xml, len);
    }
    else
    {
        UAV_LOG_WARN << "指令应答组包失败: cmd=" << cmd << "\n";
    }
}
