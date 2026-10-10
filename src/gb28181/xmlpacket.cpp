#include "xmlpacket.h"

#include <cstdio>
#include <cstring>

#include "log/log.h"

// ---------------------------------------------------------------------------
// 非标扩展指令的 CmdType 字符串（平台约定不同时只需修改此处）
// ---------------------------------------------------------------------------
#define GB_CMD_UPGRADE     "Upgrade"      // 远程升级
#define GB_CMD_LIGHT       "LightControl" // 灯光/开关控制
#define GB_CMD_ALG_CONFIG  "AlgConfig"    // 算法配置
#define GB_CMD_ALARM_UPLOAD "AlarmUpload" // 报警图片/视频上传

// ---------------------------------------------------------------------------
// 内部通用辅助
// ---------------------------------------------------------------------------
xmlDocPtr XmlPacketBuilder::create_doc(xmlNodePtr &root, const char *root_name)
{
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    if (doc == nullptr)
    {
        UAV_LOG_WARN << "xml_pack create_doc xmlNewDoc failed!\n";
        return nullptr;
    }
    xmlNewDocProp(doc, BAD_CAST "encoding", BAD_CAST "UTF-8");

    root = xmlNewNode(NULL, BAD_CAST root_name);
    if (root == nullptr)
    {
        UAV_LOG_WARN << "xml_pack create_doc xmlNewNode failed!\n";
        xmlFreeDoc(doc);
        return nullptr;
    }
    xmlDocSetRootElement(doc, root);

    return doc;
}

int XmlPacketBuilder::dump_to_buffer(xmlDocPtr doc, char *out_buff)
{
    xmlChar *xml_body = NULL;
    int xml_len = 0;

    xmlDocDumpFormatMemoryEnc(doc, &xml_body, &xml_len, NULL, 1);
    if (xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN << "xml_pack dump fail!\n";
        xmlFreeDoc(doc);
        return -1;
    }

    memcpy(out_buff, xml_body, xml_len);
    out_buff[xml_len] = '\0';

    xmlFree(xml_body);
    xmlFreeDoc(doc);

    return xml_len;
}

xmlNodePtr XmlPacketBuilder::add_child(xmlNodePtr parent, const char *name, const char *content)
{
    if (parent == nullptr || name == nullptr || content == nullptr)
    {
        return nullptr;
    }

    // 注意：不能用 xmlNewChild(parent, NULL, name, content) 一步到位——
    // 部分 libxml2 版本(如 Ubuntu 打包的 2.9.13)会把 content 当作可含实体引用的
    // 标记文本解析，遇到 '&' 解析失败时静默丢弃文本节点，导致节点内容为空。
    // 用 xmlNewDocText 创建纯文本节点再由序列化器统一转义，跨版本行为一致。
    xmlNodePtr node = xmlNewChild(parent, NULL, BAD_CAST name, NULL);
    if (node == nullptr)
    {
        return nullptr;
    }

    xmlNodePtr text = xmlNewDocText(parent->doc, BAD_CAST content);
    if (text != nullptr)
    {
        xmlAddChild(node, text);
    }

    return node;
}

void XmlPacketBuilder::add_mobile_position_nodes(xmlNodePtr root, const gb_mobile_position_t *position)
{
    if (root == nullptr || position == nullptr)
    {
        return;
    }

    char value[GB_STR_LEN_64] = {0};

    add_child(root, "Time", position->time);

    snprintf(value, sizeof(value), "%.6f", position->longitude);
    add_child(root, "Longitude", value);
    snprintf(value, sizeof(value), "%.6f", position->latitude);
    add_child(root, "Latitude", value);
    snprintf(value, sizeof(value), "%.6f", position->speed);
    add_child(root, "Speed", value);
    snprintf(value, sizeof(value), "%.6f", position->direction);
    add_child(root, "Direction", value);
    snprintf(value, sizeof(value), "%.6f", position->altitude);
    add_child(root, "Altitude", value);
}

// ---------------------------------------------------------------------------
// 通用应答
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_common_ack(const char *cmd_type, const char *device_id, int status, const char *sn, char *out_buff)
{
    if (cmd_type == nullptr || device_id == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_common_ack input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", cmd_type);
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_child(root, "Result", (status == 0) ? "OK" : "ERROR");

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>" << cmd_type << " ack xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// DeviceStatus 设备状态查询应答
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_devicestatus(const char *device_id, const gb_device_status_t *status, const char *sn, char *out_buff)
{
    if (device_id == nullptr || status == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_devicestatus input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "DeviceStatus");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_child(root, "Result", "OK");
    add_child(root, "Online", status->online);
    add_child(root, "Status", status->status);
    add_child(root, "Encode", status->encode);
    add_child(root, "Record", status->record);
    add_child(root, "DeviceTime", status->device_time);

    int alarm_num = status->alarm_num;
    if (alarm_num > GB_CAMERA_MAX)
    {
        alarm_num = GB_CAMERA_MAX;
    }
    if (alarm_num < 0)
    {
        alarm_num = 0;
    }

    char num_str[12] = {0};
    snprintf(num_str, sizeof(num_str), "%d", alarm_num);

    xmlNodePtr alarm_status_node = xmlNewChild(root, NULL, BAD_CAST "Alarmstatus", NULL);
    xmlNewProp(alarm_status_node, BAD_CAST "Num", BAD_CAST num_str);
    for (int i = 0; i < alarm_num; i++)
    {
        xmlNodePtr item = xmlNewChild(alarm_status_node, NULL, BAD_CAST "Item", NULL);
        add_child(item, "DeviceID", status->alarm_status[i].device_id);
        add_child(item, "DutyStatus", status->alarm_status[i].duty_status);
    }

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>devicestatus xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// RecordInfo 录像文件查询应答
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_recordinfo(const char *device_id, const gb_record_info_t *records, int record_num, const char *sn, char *out_buff)
{
    // records 允许为空指针——表示"查无录像"，此时 record_num 必须为 0
    if (device_id == nullptr || sn == nullptr || out_buff == nullptr ||
        (records == nullptr && record_num > 0))
    {
        UAV_LOG_WARN << "xml_pack_recordinfo input null ptr!\n";
        return -1;
    }
    if (record_num < 0)
    {
        record_num = 0;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "RecordInfo");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);

    char num_str[12] = {0};
    snprintf(num_str, sizeof(num_str), "%d", record_num);
    add_child(root, "SumNum", num_str);

    xmlNodePtr record_list = xmlNewChild(root, NULL, BAD_CAST "RecordList", NULL);
    xmlNewProp(record_list, BAD_CAST "Num", BAD_CAST num_str);

    for (int i = 0; i < record_num; i++)
    {
        const gb_record_info_t *rec = &records[i];
        xmlNodePtr item = xmlNewChild(record_list, NULL, BAD_CAST "Item", NULL);

        add_child(item, "DeviceID", rec->device_id);
        add_child(item, "Name", rec->name);
        add_child(item, "StartTime", rec->start_time);
        add_child(item, "EndTime", rec->end_time);
        snprintf(num_str, sizeof(num_str), "%d", rec->secrecy);
        add_child(item, "Secrecy", num_str);
        add_child(item, "Type", rec->type);
        snprintf(num_str, sizeof(num_str), "%d", rec->file_size);
        add_child(item, "FileSize", num_str);
    }

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>recordinfo xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// ConfigDownload 配置文件查询应答
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_configdownload(const char *device_id, const char *config_name, const char *config_content, const char *sn, char *out_buff)
{
    if (device_id == nullptr || config_name == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_configdownload input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "ConfigDownload");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_child(root, "Result", "OK");
    // 配置内容按文本节点输出(libxml2 序列化时自动转义特殊字符)
    add_child(root, config_name, (config_content != nullptr) ? config_content : "");

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>configdownload xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// PresetQuery 预置位查询应答
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_preset_query(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_preset_query input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "PresetQuery");
    add_child(root, "SN", msg_param->sn);
    add_child(root, "DeviceID", msg_param->device_id);

    int preset_num = msg_param->preset_num;
    if (preset_num > GB_STR_LEN_350)
    {
        preset_num = GB_STR_LEN_350;
    }
    if (preset_num < 0)
    {
        preset_num = 0;
    }

    char num_str[12] = {0};
    snprintf(num_str, sizeof(num_str), "%d", preset_num);
    add_child(root, "SumNum", num_str);

    xmlNodePtr preset_list = xmlNewChild(root, NULL, BAD_CAST "PresetList", NULL);
    xmlNewProp(preset_list, BAD_CAST "Num", BAD_CAST num_str);

    for (int i = 0; i < preset_num; i++)
    {
        xmlNodePtr item = xmlNewChild(preset_list, NULL, BAD_CAST "Item", NULL);
        add_child(item, "PresetID", msg_param->preset_id[i]);
        add_child(item, "PresetName", msg_param->preset_name[i]);
    }

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>presetquery xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// 控制应答类
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_ptz_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_ptz_ack input null ptr!\n";
        return -1;
    }
    return xml_pack_common_ack("DeviceControl", msg_param->device_id, msg_param->ptz_status, msg_param->sn, out_buff);
}

int XmlPacketBuilder::xml_pack_guard_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_guard_ack input null ptr!\n";
        return -1;
    }
    return xml_pack_common_ack("Guard", msg_param->device_id, msg_param->guard_status, msg_param->sn, out_buff);
}

int XmlPacketBuilder::xml_pack_home_position_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_home_position_ack input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "HomePosition");
    add_child(root, "SN", msg_param->sn);
    add_child(root, "DeviceID", msg_param->device_id);
    add_child(root, "Result", (msg_param->home_status == 0) ? "OK" : "ERROR");

    xmlNodePtr home_node = xmlNewChild(root, NULL, BAD_CAST "HomePosition", NULL);
    char value[12] = {0};
    snprintf(value, sizeof(value), "%d", msg_param->home_enabled);
    add_child(home_node, "Enabled", value);
    snprintf(value, sizeof(value), "%d", msg_param->home_reset_time);
    add_child(home_node, "ResetTime", value);
    snprintf(value, sizeof(value), "%d", msg_param->home_preset_id);
    add_child(home_node, "PresetIndex", value);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>homeposition ack xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_teleboot_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_teleboot_ack input null ptr!\n";
        return -1;
    }
    return xml_pack_common_ack("TeleBoot", msg_param->device_id, msg_param->reboot_status, msg_param->sn, out_buff);
}

int XmlPacketBuilder::xml_pack_manual_record_ack(const char *device_id, int status, const char *sn, char *out_buff)
{
    return xml_pack_common_ack("ManualRecord", device_id, status, sn, out_buff);
}

// ---------------------------------------------------------------------------
// 通知上报类
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_mobile_position_response(const char *device_id, const gb_mobile_position_t *position, const char *sn, char *out_buff)
{
    if (device_id == nullptr || position == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_mobile_position_response input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "MobilePosition");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_child(root, "Result", "OK");
    add_mobile_position_nodes(root, position);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>mobileposition response xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_mobile_position_notify(const char *device_id, const gb_mobile_position_t *position, const char *sn, char *out_buff)
{
    if (device_id == nullptr || position == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_mobile_position_notify input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Notify");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "MobilePosition");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_mobile_position_nodes(root, position);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>mobileposition notify xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_alarm_notify(const char *device_id, const gb_alarm_info_t *alarm, const char *sn, char *out_buff)
{
    if (device_id == nullptr || alarm == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_alarm_notify input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Notify");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "Alarm");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_child(root, "AlarmPriority", alarm->alarm_priority);
    add_child(root, "AlarmMethod", alarm->alarm_method);
    add_child(root, "AlarmTime", alarm->alarm_time);
    add_child(root, "AlarmDescription", alarm->alarm_description);

    char value[GB_STR_LEN_64] = {0};
    snprintf(value, sizeof(value), "%.6f", alarm->longitude);
    add_child(root, "Longitude", value);
    snprintf(value, sizeof(value), "%.6f", alarm->latitude);
    add_child(root, "Latitude", value);

    xmlNodePtr info_node = xmlNewChild(root, NULL, BAD_CAST "Info", NULL);
    snprintf(value, sizeof(value), "%d", alarm->alarm_type);
    add_child(info_node, "AlarmType", value);
    add_child(info_node, "AlarmTypeParam", alarm->alarm_type_param);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>alarm notify xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_media_status_notify(const char *device_id, const char *notify_type, const char *sn, char *out_buff)
{
    if (device_id == nullptr || notify_type == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_media_status_notify input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Notify");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", "MediaStatus");
    add_child(root, "SN", sn);
    add_child(root, "DeviceID", device_id);
    add_child(root, "NotifyType", notify_type);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>mediastatus notify xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// 非标扩展指令应答
// ---------------------------------------------------------------------------
int XmlPacketBuilder::xml_pack_upgrade_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_upgrade_ack input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", GB_CMD_UPGRADE);
    add_child(root, "SN", msg_param->sn);
    add_child(root, "DeviceID", msg_param->device_id);
    add_child(root, "Result", (msg_param->upgrade_status == 0) ? "OK" : "ERROR");
    add_child(root, "Firmware", msg_param->firmware);
    add_child(root, "FileUrl", msg_param->file_url);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>upgrade ack xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_light_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_light_ack input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", GB_CMD_LIGHT);
    add_child(root, "SN", msg_param->sn);
    add_child(root, "DeviceID", msg_param->device_id);
    add_child(root, "Result", (msg_param->light_status == 0) ? "OK" : "ERROR");

    char value[12] = {0};
    snprintf(value, sizeof(value), "%d", (int)msg_param->switch_enable);
    add_child(root, "Enable", value);
    add_child(root, "SwitchNo", msg_param->switch_no);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>light ack xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_alg_config_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_alg_config_ack input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", GB_CMD_ALG_CONFIG);
    add_child(root, "SN", msg_param->sn);
    add_child(root, "DeviceID", msg_param->device_id);
    add_child(root, "Result", (msg_param->alg_status == 0) ? "OK" : "ERROR");

    char value[GB_STR_LEN_32] = {0};
    snprintf(value, sizeof(value), "%d", msg_param->opt_type);
    add_child(root, "OptType", value);
    snprintf(value, sizeof(value), "%d", msg_param->extraction_type);
    add_child(root, "ExtractionType", value);
    snprintf(value, sizeof(value), "%.2f", msg_param->confidence_thresh);
    add_child(root, "ConfidenceThresh", value);
    snprintf(value, sizeof(value), "%d", msg_param->alarm_interval);
    add_child(root, "AlarmInterval", value);
    add_child(root, "ChannelID", msg_param->channel_id);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>algconfig ack xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

int XmlPacketBuilder::xml_pack_alarm_upload_ack(gb28181_message_param_t *msg_param, char *out_buff)
{
    if (msg_param == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN << "xml_pack_alarm_upload_ack input null ptr!\n";
        return -1;
    }

    xmlNodePtr root = NULL;
    xmlDocPtr doc = create_doc(root, "Response");
    if (doc == nullptr)
    {
        return -1;
    }

    add_child(root, "CmdType", GB_CMD_ALARM_UPLOAD);
    add_child(root, "SN", msg_param->sn);
    add_child(root, "DeviceID", msg_param->device_id);
    add_child(root, "Result", (msg_param->upload_status == 0) ? "OK" : "ERROR");
    add_child(root, "PicUrl", msg_param->pic_url);
    add_child(root, "VideoUrl", msg_param->video_url);

    int xml_len = dump_to_buffer(doc, out_buff);
    UAV_LOG_INFO << "------>alarmupload ack xml_len = " << xml_len << "-----------\n";
    return xml_len;
}

// ---------------------------------------------------------------------------
// 既有指令实现（保持不变）
// ---------------------------------------------------------------------------
XmlPacketBuilder::XmlPacketBuilder():m_doc(nullptr),
    m_root_node(nullptr),
    m_device_list_node(nullptr),
    m_item_node(nullptr)
{

}

XmlPacketBuilder::~XmlPacketBuilder()
{
    release_doc();
}

void XmlPacketBuilder::release_doc()
{
    if (m_doc != nullptr)
    {
        xmlFreeDoc(m_doc);
        m_doc = nullptr;
    }
    // 节点指针依附于doc，doc释放后节点自动失效，置空防止野指针
    m_root_node = nullptr;
    m_device_list_node = nullptr;
    m_item_node = nullptr;
}

int XmlPacketBuilder::xml_pack_catalog(gb28181_system_param *sys_param, int *channel_status, const char *sn, char *out_buff)
{

    if (sys_param == nullptr || channel_status == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_catalog input null ptr!\n";
        return -1;
    }

    xmlChar *xml_body = NULL;
    int xml_len = 0;

    // 【关键】先释放上一次遗留文档
    release_doc();

    // 使用类成员 m_doc，不再是局部变量
    m_doc = xmlNewDoc(BAD_CAST "1.0");
    if (m_doc == nullptr)
    {
        UAV_LOG_WARN<<"xmlNewDoc failed\n";
        return -1;
    }
    xmlNewDocProp(m_doc, BAD_CAST "encoding", BAD_CAST "UTF-8");

    m_root_node = xmlNewNode(NULL, BAD_CAST "Response");
    xmlDocSetRootElement(m_doc, m_root_node);

    add_child(m_root_node, "CmdType", "Catalog");
    add_child(m_root_node, "SN", sn);
    add_child(m_root_node, "DeviceID", sys_param->sipdev_reg_id);

    UAV_LOG_INFO<<"--------------test sys_param->channel_size: "<< sys_param->channel_size <<"\n";
    char camera_size[12] = {0};
    sprintf(camera_size, "%d", sys_param->channel_size);
    add_child(m_root_node, "SumNum", camera_size);

    m_device_list_node = xmlNewChild(m_root_node, NULL, BAD_CAST "DeviceList", NULL);
    xmlNewProp(m_device_list_node, BAD_CAST "Num", BAD_CAST camera_size);


    for(int idx = 0; idx < GB_CAMERA_MAX; idx++)
    {
        for(int i = 0; i < sys_param->channel_size; i++)
        {
            if(idx != sys_param->channel_para[i].channel_index)
                continue;

            m_item_node = xmlNewChild(m_device_list_node, NULL, BAD_CAST "Item", NULL);
            add_child(m_item_node, "DeviceID", sys_param->channel_para[i].channel_id);
            char channelName[24] = {0};
            sprintf(channelName, "channel%02d", sys_param->channel_para[i].channel_index + 1);
            add_child(m_item_node, "Name", channelName);

            add_child(m_item_node, "Manufacturer", sys_param->dev_manufacturer);
            add_child(m_item_node, "Model", sys_param->dev_model);
            add_child(m_item_node, "Owner", sys_param->dev_manufacturer);
            add_child(m_item_node, "CivilCode", sys_param->sipserver_region);
            add_child(m_item_node, "Address", sys_param->channel_para[i].channel_ip);
            add_child(m_item_node, "Parental", "0");
            add_child(m_item_node, "ParentID", sys_param->sipdev_reg_id);
            add_child(m_item_node, "SafetyWay", "0");
            add_child(m_item_node, "RegisterWay", "2"); //网关代理改为2
            add_child(m_item_node, "Secrecy", "0");
            add_child(m_item_node, "IPAddress", sys_param->channel_para[i].channel_ip);
            add_child(m_item_node, "Port", "80");
            // add_child(m_item_node, "Password", sys_param->channel_para[i].password); //建议注释掉密码

            int status = channel_status[idx];
            add_child(m_item_node, "Status", (status?"ON":"OFF"));
        }
    }

    xmlDocDumpFormatMemoryEnc(m_doc, &xml_body, &xml_len, NULL, 1);
    if(xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN<<"xmlDocDumpFormatMemoryEnc fail\n";
        release_doc();
        return -1;
    }

    memcpy((char *)out_buff,(char *)xml_body,xml_len);
    out_buff[xml_len] = '\0';
    UAV_LOG_INFO<<"------>catalog xml_len = "<< xml_len <<"-----------\n";

    xmlFree(xml_body);
    // 重点！这里不再调用 xmlFreeDoc(m_doc)，交给下一次release_doc或者析构释放
    return xml_len;
}

int XmlPacketBuilder::xml_pack_keepalive(const char *device_id, const char *sn, char *out_buff)
{
    if (device_id == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_keepalive input null ptr!\n";
        return -1;
    }

    xmlDocPtr keepalive_body_ptr = NULL;
    xmlChar *xml_body = NULL;
    int xml_len = 0;
    xmlNodePtr root_node = NULL;

    /*
     * Creates a new document, a node and set it as a root node
     */
    keepalive_body_ptr = xmlNewDoc(BAD_CAST "1.0");
    if (keepalive_body_ptr == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_keepalive xmlNewDoc failed\n";
        return -1;
    }
    // 加上UTF-8编码，和catalog保持一致
    xmlNewDocProp(keepalive_body_ptr, BAD_CAST "encoding", BAD_CAST "UTF-8");

    root_node = xmlNewNode(NULL, BAD_CAST "Notify");
    xmlDocSetRootElement(keepalive_body_ptr, root_node);

    add_child(root_node, "CmdType", "Keepalive");
    add_child(root_node, "SN", sn);
    add_child(root_node, "DeviceID", device_id);
    add_child(root_node, "Status", "OK");

    xmlDocDumpFormatMemoryEnc(keepalive_body_ptr, &xml_body, &xml_len, NULL, 1);
    if(xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN<<"xml_pack_keepalive dump fail\n";
        xmlFreeDoc(keepalive_body_ptr);
        return -1;
    }

    memcpy((char *)out_buff, (char *)xml_body, xml_len);
    out_buff[xml_len] = '\0';

    UAV_LOG_INFO<<"------>keepalive xml_len = "<< xml_len <<"-----------\n";

    xmlFree(xml_body);
    xmlFreeDoc(keepalive_body_ptr);

    return xml_len;
}

int XmlPacketBuilder::xml_pack_deviceinfo(gb28181_system_param *sys_param,const char *sn, char *out_buff)
{
    if (sys_param == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_deviceinfo input null ptr!\n";
        return -1;
    }

    xmlDocPtr deviceinfo_body_ptr = NULL;
    xmlChar *xml_body = NULL;
    int xml_len = 0;
    xmlNodePtr root_node = NULL;

    /*
     * Creates a new document, a node and set it as a root node
     */
    deviceinfo_body_ptr = xmlNewDoc(BAD_CAST "1.0");
    if (deviceinfo_body_ptr == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_deviceinfo xmlNewDoc failed\n";
        return -1;
    }
    xmlNewDocProp(deviceinfo_body_ptr, BAD_CAST "encoding", BAD_CAST "UTF-8");

    root_node = xmlNewNode(NULL, BAD_CAST "Response");
    xmlDocSetRootElement(deviceinfo_body_ptr, root_node);

    add_child(root_node, "CmdType", "DeviceInfo");
    add_child(root_node, "SN", sn);
    add_child(root_node, "DeviceID", sys_param->sipdev_reg_id);
    add_child(root_node, "DeviceName", sys_param->dev_name);
    add_child(root_node, "Result", "OK");

    add_child(root_node, "Manufacturer", sys_param->dev_manufacturer);
    add_child(root_node, "Model", sys_param->dev_model);
    add_child(root_node, "Firmware", sys_param->dev_version);

    char cam_num[12] = {0};
    sprintf(cam_num, "%d", GB_CAMERA_MAX);
    add_child(root_node, "MaxCamera", cam_num);
    add_child(root_node, "MaxAlarm", cam_num);

    sprintf(cam_num, "%d", sys_param->channel_size);
    add_child(root_node, "Channel", cam_num);


    xmlDocDumpFormatMemoryEnc(deviceinfo_body_ptr, &xml_body, &xml_len, NULL,1);
    if(xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN<<"xml_pack_deviceinfo dump fail\n";
        xmlFreeDoc(deviceinfo_body_ptr);
        return -1;
    }

    memcpy((char *)out_buff, (char *)xml_body, xml_len);
    out_buff[xml_len] = '\0';

    UAV_LOG_INFO<<"------>deviceinfo xml_len = "<< xml_len <<"-----------\n";

    xmlFree(xml_body);
    xmlFreeDoc(deviceinfo_body_ptr);

    return xml_len;
}


int XmlPacketBuilder::xml_pack_deviceconfig(gb28181_system_param *sys_param, const char *sn, char *out_buff)
{
    if (sys_param == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_deviceconfig input null ptr!\n";
        return -1;
    }

    xmlDocPtr doc_ptr = NULL;
    xmlChar *xml_body = NULL;
    int xml_len = 0;
    xmlNodePtr root_node = NULL;

    /*
     * Creates a new document, a node and set it as a root node
     */
    doc_ptr = xmlNewDoc(BAD_CAST "1.0");
    if (doc_ptr == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_deviceconfig xmlNewDoc failed\n";
        return -1;
    }
    xmlNewDocProp(doc_ptr, BAD_CAST "encoding", BAD_CAST "UTF-8");

    root_node = xmlNewNode(NULL, BAD_CAST "Response");
    xmlDocSetRootElement(doc_ptr, root_node);

    add_child(root_node, "CmdType", "DeviceConfig");
    add_child(root_node, "SN", sn);
    add_child(root_node, "DeviceID", sys_param->sipdev_reg_id);
    add_child(root_node, "Result", "OK");

    xmlDocDumpFormatMemoryEnc(doc_ptr, &xml_body, &xml_len, NULL,1);
    if(xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN<<"xml_pack_deviceconfig dump fail\n";
        xmlFreeDoc(doc_ptr);
        return -1;
    }

    memcpy((char *)out_buff, (char *)xml_body, xml_len);
    out_buff[xml_len] = '\0';

    UAV_LOG_INFO<<"------>deviceconfig xml_len = "<< xml_len <<"-----------\n";

    xmlFree(xml_body);
    xmlFreeDoc(doc_ptr);

    return xml_len;
}


int XmlPacketBuilder::xml_pack_device_config_ack(const char *device_id,int status,const char *sn,char *out_buff)
{
    if (device_id == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_device_config_ack input null ptr!\n";
        return -1;
    }

    xmlDocPtr device_config_ack_ptr = NULL;
    xmlChar *xml_body = NULL;
    int xml_len = 0;
    xmlNodePtr root_node = NULL;

    device_config_ack_ptr = xmlNewDoc(BAD_CAST "1.0");
    if (device_config_ack_ptr == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_device_config_ack xmlNewDoc failed\n";
        return -1;
    }
    xmlNewDocProp(device_config_ack_ptr, BAD_CAST "encoding", BAD_CAST "UTF-8");

    root_node = xmlNewNode(NULL, BAD_CAST "Response");
    xmlDocSetRootElement(device_config_ack_ptr, root_node);

    add_child(root_node, "CmdType", "DeviceConfig");
    add_child(root_node, "SN", sn);
    add_child(root_node, "DeviceID", device_id);
    add_child(root_node, "Result", (status == 0) ? "OK" : "ERROR");

    xmlDocDumpFormatMemoryEnc(device_config_ack_ptr,&xml_body,&xml_len,NULL,1);
    if(xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN<<"xml_pack_device_config_ack dump fail\n";
        xmlFreeDoc(device_config_ack_ptr);
        return -1;
    }

    memcpy((char *)out_buff,(char *)xml_body,xml_len);
    out_buff[xml_len] = '\0';

    UAV_LOG_INFO<<"------>device_config_ack xml_len = "<< xml_len <<"-----------\n";

    xmlFree(xml_body);
    xmlFreeDoc(device_config_ack_ptr);

    return xml_len;
}



int XmlPacketBuilder::xml_pack_broadcast_ack(const char *device_id,const char *broadcast_id,const char *sn,char *out_buff)
{
    if (device_id == nullptr || broadcast_id == nullptr || sn == nullptr || out_buff == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_broadcast_ack input null ptr!\n";
        return -1;
    }

    xmlDocPtr broadcast_body_ptr = NULL;
    xmlChar *xml_body = NULL;
    int xml_len = 0;
    int  broadcast_status=1;
    xmlNodePtr root_node = NULL;

    broadcast_body_ptr = xmlNewDoc(BAD_CAST "1.0");
    if (broadcast_body_ptr == nullptr)
    {
        UAV_LOG_WARN<<"xml_pack_broadcast_ack xmlNewDoc failed\n";
        return -1;
    }
    xmlNewDocProp(broadcast_body_ptr, BAD_CAST "encoding", BAD_CAST "UTF-8");

    root_node = xmlNewNode(NULL, BAD_CAST "Response");
    xmlDocSetRootElement(broadcast_body_ptr, root_node);

    add_child(root_node, "CmdType", "Broadcast");
    add_child(root_node, "SN", sn);
    add_child(root_node, "DeviceID", broadcast_id);
    add_child(root_node, "Result", broadcast_status ? "OK" : "ERROR");

    xmlDocDumpFormatMemoryEnc(broadcast_body_ptr,&xml_body,&xml_len,NULL,1);
    if(xml_body == nullptr || xml_len <= 0)
    {
        UAV_LOG_WARN<<"xml_pack_broadcast_ack dump fail\n";
        xmlFreeDoc(broadcast_body_ptr);
        return -1;
    }

    memcpy((char *)out_buff,(char *)xml_body,xml_len);
    out_buff[xml_len] = '\0';

    UAV_LOG_INFO<<"------>broadcast_ack xml_len = "<< xml_len <<"-----------\n";

    /* free xml_body buffer */
    xmlFree(xml_body);
    /*free the document */
    xmlFreeDoc(broadcast_body_ptr);

    return xml_len;
}
