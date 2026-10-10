#include "xmlpacketparser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log/log.h"

namespace
{
// 十六进制字符转数值，非法返回 -1
int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

XmlPacketParser::XmlPacketParser()
{
}

XmlPacketParser::~XmlPacketParser()
{
    release_doc();
}

void XmlPacketParser::release_doc()
{
    if (m_doc != nullptr)
    {
        xmlFreeDoc(m_doc);
        m_doc = nullptr;
    }
    // 节点指针依附于doc，doc释放后节点自动失效，置空防止野指针
    m_root_node = nullptr;
    m_root_type = GB_XML_ROOT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// 内部通用辅助
// ---------------------------------------------------------------------------
xmlNodePtr XmlPacketParser::find_child(xmlNodePtr parent, const char *name)
{
    if (parent == nullptr || name == nullptr)
    {
        return nullptr;
    }

    for (xmlNodePtr node = parent->children; node != nullptr; node = node->next)
    {
        if (node->type == XML_ELEMENT_NODE && node->name != nullptr &&
            xmlStrEqual(node->name, BAD_CAST name))
        {
            return node;
        }
    }
    return nullptr;
}

const char* XmlPacketParser::node_content(xmlNodePtr node)
{
    if (node == nullptr || node->children == nullptr)
    {
        return nullptr;
    }
    // GB28181 报文字段均为简单文本节点，直接取第一个文本子节点的内容
    xmlNodePtr child = node->children;
    if (child->type == XML_TEXT_NODE && child->content != nullptr)
    {
        return (const char*)child->content;
    }
    return nullptr;
}

const char* XmlPacketParser::child_content(xmlNodePtr parent, const char *name)
{
    return node_content(find_child(parent, name));
}

int XmlPacketParser::parse_int(xmlNodePtr parent, const char *name, int default_val)
{
    const char *content = child_content(parent, name);
    if (content == nullptr || *content == '\0')
    {
        return default_val;
    }
    return (int)strtol(content, nullptr, 10);
}

int XmlPacketParser::copy_str(char *dst, int size, const char *src)
{
    if (dst == nullptr || size <= 0)
    {
        return -1;
    }
    if (src == nullptr)
    {
        dst[0] = '\0';
        return -1;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
    return 0;
}

// ---------------------------------------------------------------------------
// 公共字段: CmdType / SN / DeviceID
// ---------------------------------------------------------------------------
void XmlPacketParser::parse_common(xmlNodePtr root, gb28181_message_param_t *msg_param)
{
    copy_str(msg_param->cmd_type, sizeof(msg_param->cmd_type), child_content(root, "CmdType"));
    copy_str(msg_param->sn, sizeof(msg_param->sn), child_content(root, "SN"));
    copy_str(msg_param->device_id, sizeof(msg_param->device_id), child_content(root, "DeviceID"));
}

// ---------------------------------------------------------------------------
// DeviceControl 的 PTZCmd 解码
//
// PTZCmd 为十六进制字符串，标准 8 字节(如停止 "A50F010000000001")：
//   字节1   A5H 命令头(部分实现会去掉，按字节数自动兼容)
//   字节2   命令类型: 低4位 0停止 1方向 2变倍 3变焦 4光圈 8预置位 F全部停止
//          (少数实现放高4位，低4位为0时按高4位兜底)
//   字节3   动作码: 方向 1上 2下 3左 4右 5左上 6右上 7左下 8右下
//                  变倍 1放大 2缩小; 变焦 1聚焦+ 2聚焦-
//                  预置位 1设置 2调用 3删除 4查询 5修改
//   字节4   速度(0x00-0xFF) 或 预置位编号(预置位命令)
// ---------------------------------------------------------------------------
int XmlPacketParser::parse_ptz_cmd(const char *ptz_cmd, gb28181_message_param_t *msg_param)
{
    msg_param->ptz_cmd = PTZ_CMD_STOP;
    msg_param->ptz_speed = 0;
    msg_param->ptz_preset_num = 0;

    if (ptz_cmd == nullptr || *ptz_cmd == '\0')
    {
        UAV_LOG_WARN << "parse_ptz_cmd empty PTZCmd!\n";
        return -1;
    }

    unsigned char bytes[16] = {0};
    int len = 0;
    while (ptz_cmd[0] != '\0' && ptz_cmd[1] != '\0' && len < (int)sizeof(bytes))
    {
        int hi = hex_val(ptz_cmd[0]);
        int lo = hex_val(ptz_cmd[1]);
        if (hi < 0 || lo < 0)
        {
            UAV_LOG_WARN << "parse_ptz_cmd invalid hex: " << ptz_cmd << "\n";
            return -1;
        }
        bytes[len++] = (unsigned char)((hi << 4) | lo);
        ptz_cmd += 2;
    }

    if (len < 8)
    {
        UAV_LOG_WARN << "parse_ptz_cmd too short, len = " << len << "\n";
        return -1;
    }

    // 命令头 A5 占第 1 字节(标准 8 字节包含 A5；去掉 A5 的 8 字节实现同样兼容)
    int idx = (bytes[0] == 0xA5 && len >= 8) ? 1 : 0;

    int cmd_byte = bytes[idx];
    int action   = bytes[idx + 1];
    int param    = bytes[idx + 2];

    int cmd = cmd_byte & 0x0F;
    if (cmd == 0 && (cmd_byte >> 4) != 0)
    {
        cmd = cmd_byte >> 4;    // 兼容命令类型放高 4 位的实现
    }
    if (cmd == 0x0F)
    {
        cmd = 0;                // 0x0F = 全部停止
    }

    msg_param->ptz_speed = (cmd != 8) ? param : 0;

    switch (cmd)
    {
    case 0: // 停止
        msg_param->ptz_cmd = PTZ_CMD_STOP;
        break;
    case 1: // 方向控制
        switch (action)
        {
        case 1: msg_param->ptz_cmd = PTZ_CMD_TOP;        break;
        case 2: msg_param->ptz_cmd = PTZ_CMD_DOWN;       break;
        case 3: msg_param->ptz_cmd = PTZ_CMD_LEFT;       break;
        case 4: msg_param->ptz_cmd = PTZ_CMD_RIGHT;      break;
        case 5: msg_param->ptz_cmd = PTZ_CMD_LEFT_TOP;   break;
        case 6: msg_param->ptz_cmd = PTZ_CMD_RIGHT_TOP;  break;
        case 7: msg_param->ptz_cmd = PTZ_CMD_LEFT_DOWN;  break;
        case 8: msg_param->ptz_cmd = PTZ_CMD_RIGHT_DOWN; break;
        default: msg_param->ptz_cmd = PTZ_CMD_STOP;      break;
        }
        break;
    case 2: // 变倍
        msg_param->ptz_cmd = (action == 2) ? PTZ_CMD_ZOOM_REDUCE : PTZ_CMD_ZOOM_ADD;
        break;
    case 3: // 变焦
        msg_param->ptz_cmd = (action == 2) ? PTZ_CMD_FOCUS_REDUCE : PTZ_CMD_FOCUS_ADD;
        break;
    case 4: // 光圈(ptz_cmd_e 未定义光圈指令，暂按停止处理)
        msg_param->ptz_cmd = PTZ_CMD_STOP;
        break;
    case 8: // 预置位
        msg_param->ptz_preset_num = param;
        switch (action)
        {
        case 1: msg_param->ptz_cmd = PTZ_CMD_SET_PRESET;    break;
        case 2: msg_param->ptz_cmd = PTZ_CMD_GOTO_PRESET;   break;
        case 3: msg_param->ptz_cmd = PTZ_CMD_DELETE_PRESET; break;
        case 4: msg_param->ptz_cmd = PTZ_CMD_GET_PRESET;    break;
        case 5: msg_param->ptz_cmd = PTZ_CMD_MODIFY_PRESET; break;
        default: msg_param->ptz_cmd = PTZ_CMD_STOP;         break;
        }
        break;
    default:
        msg_param->ptz_cmd = PTZ_CMD_STOP;
        break;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// 按 CmdType 解析各指令的附加字段
// ---------------------------------------------------------------------------
void XmlPacketParser::parse_per_cmdtype(xmlNodePtr root, gb28181_message_param_t *msg_param)
{
    const char *cmd = msg_param->cmd_type;

    if (strcmp(cmd, "DeviceControl") == 0)
    {
        // 云台控制: PTZCmd 十六进制解码
        parse_ptz_cmd(child_content(root, "PTZCmd"), msg_param);
    }
    else if (strcmp(cmd, "Guard") == 0)
    {
        // 布防/撤防: GuardCmd = SetGuard / ResetGuard
        const char *guard_cmd = child_content(root, "GuardCmd");
        if (guard_cmd != nullptr &&
            xmlStrcasecmp(BAD_CAST guard_cmd, BAD_CAST "SetGuard") == 0)
        {
            msg_param->guard_status = 1;   // 布防
        }
        else
        {
            msg_param->guard_status = 0;   // 撤防(ResetGuard/缺省)
        }
    }
    else if (strcmp(cmd, "HomePosition") == 0)
    {
        // 看守位: <HomePosition><Enabled/><ResetTime/><PresetIndex/></HomePosition>
        xmlNodePtr home = find_child(root, "HomePosition");
        if (home != nullptr)
        {
            msg_param->home_enabled = parse_int(home, "Enabled", 0);
            msg_param->home_reset_time = parse_int(home, "ResetTime", 0);
            msg_param->home_preset_id = parse_int(home, "PresetIndex", 0);
        }
    }
    else if (strcmp(cmd, "Broadcast") == 0)
    {
        // 语音广播: 源/目标设备ID
        copy_str(msg_param->broadcast_source_id, sizeof(msg_param->broadcast_source_id),
                 child_content(root, "SourceID"));
        copy_str(msg_param->broadcast_target_id, sizeof(msg_param->broadcast_target_id),
                 child_content(root, "TargetID"));
    }
    else if (strcmp(cmd, "RecordInfo") == 0)
    {
        // 录像查询条件
        copy_str(msg_param->record_start_time, sizeof(msg_param->record_start_time),
                 child_content(root, "StartTime"));
        copy_str(msg_param->record_end_time, sizeof(msg_param->record_end_time),
                 child_content(root, "EndTime"));
        copy_str(msg_param->record_type, sizeof(msg_param->record_type),
                 child_content(root, "Type"));
        msg_param->record_secrecy = parse_int(root, "Secrecy", 0);
        msg_param->indistinct_query = parse_int(root, "IndistinctQuery", 0);
    }
    else if (strcmp(cmd, "ConfigDownload") == 0)
    {
        // 配置查询类型: ConfigType = BasicParam/VideoParam/...
        copy_str(msg_param->config_type, sizeof(msg_param->config_type),
                 child_content(root, "ConfigType"));
    }
    else if (strcmp(cmd, "DeviceConfig") == 0)
    {
        // 设备配置下发: 记录携带的配置节点名(BasicParam/VideoParam/...)
        static const char *kConfigNodes[] =
        {
            "BasicParam", "VideoParam", "SVACEncodeParam", "SVACDecodeParam"
        };
        for (const char *node_name : kConfigNodes)
        {
            if (find_child(root, node_name) != nullptr)
            {
                copy_str(msg_param->config_type, sizeof(msg_param->config_type), node_name);
                break;
            }
        }
    }
    else if (strcmp(cmd, "MobilePosition") == 0)
    {
        // 移动位置查询: Interval 主动上报间隔(秒)
        msg_param->position_interval = parse_int(root, "Interval", 0);
    }
    else if (strcmp(cmd, "Upgrade") == 0)
    {
        // 非标扩展: 远程升级
        copy_str(msg_param->firmware, sizeof(msg_param->firmware),
                 child_content(root, "Firmware"));
        copy_str(msg_param->file_url, sizeof(msg_param->file_url),
                 child_content(root, "FileUrl"));
    }
    else if (strcmp(cmd, "LightControl") == 0 || strcmp(cmd, "Light") == 0)
    {
        // 非标扩展: 灯光/开关控制
        msg_param->switch_enable = (char)parse_int(root, "Enable", 0);
        copy_str(msg_param->switch_no, sizeof(msg_param->switch_no),
                 child_content(root, "SwitchNo"));
    }
    else if (strcmp(cmd, "AlgConfig") == 0)
    {
        // 非标扩展: 算法配置
        msg_param->opt_type = parse_int(root, "OptType", 0);
        msg_param->extraction_type = parse_int(root, "ExtractionType", 0);
        msg_param->alarm_interval = parse_int(root, "AlarmInterval", 0);
        const char *thresh = child_content(root, "ConfidenceThresh");
        msg_param->confidence_thresh = (thresh != nullptr) ? strtof(thresh, nullptr) : 0.0f;
        copy_str(msg_param->device_name, sizeof(msg_param->device_name),
                 child_content(root, "DeviceName"));
        copy_str(msg_param->channel_id, sizeof(msg_param->channel_id),
                 child_content(root, "ChannelID"));
        copy_str(msg_param->channel_name, sizeof(msg_param->channel_name),
                 child_content(root, "ChannelName"));
    }
    else if (strcmp(cmd, "AlarmUpload") == 0)
    {
        // 非标扩展: 报警图片/视频上传
        copy_str(msg_param->pic_url, sizeof(msg_param->pic_url),
                 child_content(root, "PicUrl"));
        copy_str(msg_param->video_url, sizeof(msg_param->video_url),
                 child_content(root, "VideoUrl"));
    }
    // 其余指令(DeviceInfo/DeviceStatus/Catalog/PresetQuery/TeleBoot/ManualRecord/
    // Keepalive/MediaStatus 等)只需公共字段，无需附加解析
}

// ---------------------------------------------------------------------------
// 入口: 解析平台发来的 SIP MESSAGE XML 消息体
// ---------------------------------------------------------------------------
int XmlPacketParser::parse(const char *xml_buff, int xml_len, gb28181_message_param_t *msg_param)
{
    if (xml_buff == nullptr || xml_len <= 0 || msg_param == nullptr)
    {
        UAV_LOG_WARN << "xml_packet_parse input null ptr!\n";
        return -1;
    }

    // 释放上一次遗留文档，并清零输出结构(避免上次报文的残留字段)
    release_doc();
    m_root_type = GB_XML_ROOT_UNKNOWN;
    memset(msg_param, 0, sizeof(*msg_param));

    m_doc = xmlReadMemory(xml_buff, xml_len, "gb28181.xml", "UTF-8",
                          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (m_doc == nullptr)
    {
        UAV_LOG_WARN << "xml_packet_parse xmlReadMemory failed!\n";
        return -1;
    }

    m_root_node = xmlDocGetRootElement(m_doc);
    if (m_root_node == nullptr || m_root_node->name == nullptr)
    {
        UAV_LOG_WARN << "xml_packet_parse no root element!\n";
        release_doc();
        return -1;
    }

    if (xmlStrEqual(m_root_node->name, BAD_CAST "Query"))
    {
        m_root_type = GB_XML_ROOT_QUERY;
    }
    else if (xmlStrEqual(m_root_node->name, BAD_CAST "Control"))
    {
        m_root_type = GB_XML_ROOT_CONTROL;
    }
    else if (xmlStrEqual(m_root_node->name, BAD_CAST "Response"))
    {
        m_root_type = GB_XML_ROOT_RESPONSE;
    }
    else if (xmlStrEqual(m_root_node->name, BAD_CAST "Notify"))
    {
        m_root_type = GB_XML_ROOT_NOTIFY;
    }
    else
    {
        UAV_LOG_WARN << "xml_packet_parse unknown root: " << (const char*)m_root_node->name << "\n";
        release_doc();
        return -1;
    }

    parse_common(m_root_node, msg_param);

    // 平台应答: 记录 Result(供业务判断平台是否受理)
    if (m_root_type == GB_XML_ROOT_RESPONSE)
    {
        const char *result = child_content(m_root_node, "Result");
        msg_param->result_status = (result != nullptr &&
                                    xmlStrEqual(BAD_CAST result, BAD_CAST "OK")) ? 0 : 1;
    }

    parse_per_cmdtype(m_root_node, msg_param);

    UAV_LOG_INFO << "------>parse ok, root=" << (const char*)m_root_node->name
                 << ", cmd=" << msg_param->cmd_type
                 << ", sn=" << msg_param->sn << "\n";

    return 0;
}
