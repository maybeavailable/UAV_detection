#pragma once

#include <iostream>

// libxml2 头文件自带 __cplusplus 的 extern "C" 保护，无需再包一层；
// 包上反而会在系统 libxml2(启用 ICU) 时把 ICU 的 C++ 头文件一并纳入 C 链接导致编译失败
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlsave.h>

#include "gb28181type.h"

// ---------------------------------------------------------------------------
// GB28181 XML 报文载荷结构体（供各指令的应答 / 上报报文组装使用）
// ---------------------------------------------------------------------------

/* 报警状态条目（DeviceStatus 应答中 <Alarmstatus> 的 <Item>） */
struct gb_alarm_status_item_t
{
    char device_id[GB_STR_LEN_32];   /* 报警设备ID */
    char duty_status[GB_STR_LEN_16]; /* 布防状态: ONDUTY / OFFDUTY / ALARM */
};

/* 设备状态（DeviceStatus 查询应答） */
struct gb_device_status_t
{
    char online[GB_STR_LEN_16];      /* ONLINE / OFFLINE */
    char status[GB_STR_LEN_16];      /* OK */
    char encode[GB_STR_LEN_16];      /* 是否编码: ON / OFF */
    char record[GB_STR_LEN_16];      /* 是否录像: ON / OFF */
    char device_time[GB_STR_LEN_64]; /* 设备时间, 格式 yyyy-MM-ddTHH:mm:ss */
    int  alarm_num;                  /* 报警状态条数 */
    gb_alarm_status_item_t alarm_status[GB_CAMERA_MAX];
};

/* 录像文件信息（RecordInfo 查询应答中 <RecordList> 的 <Item>） */
struct gb_record_info_t
{
    char device_id[GB_STR_LEN_32];   /* 通道设备ID */
    char name[GB_STR_LEN_64];        /* 录像名称 */
    char start_time[GB_STR_LEN_64];  /* 开始时间 yyyy-MM-ddTHH:mm:ss */
    char end_time[GB_STR_LEN_64];    /* 结束时间 yyyy-MM-ddTHH:mm:ss */
    int  secrecy;                    /* 保密属性: 0不涉密 1涉密 */
    char type[GB_STR_LEN_16];        /* time/alarm/manual/all */
    int  file_size;                  /* 文件大小(Byte) */
};

/* 移动位置信息（MobilePosition 查询应答 / 通知上报） */
struct gb_mobile_position_t
{
    char   time[GB_STR_LEN_64];      /* 时间 yyyy-MM-ddTHH:mm:ss */
    double longitude;                /* 经度 */
    double latitude;                 /* 纬度 */
    double speed;                    /* 速度 */
    double direction;                /* 方向 */
    double altitude;                 /* 海拔 */
};

/* 报警信息（Alarm 通知上报） */
struct gb_alarm_info_t
{
    char alarm_priority[GB_STR_LEN_16];    /* 报警级别 1-4 */
    char alarm_method[GB_STR_LEN_16];      /* 报警方式: 1电话报警 2设备报警 3短信报警 4GPS报警 5视频报警 6设备故障 7其他报警 */
    char alarm_time[GB_STR_LEN_64];        /* 报警时间 yyyy-MM-ddTHH:mm:ss */
    char alarm_description[GB_STR_LEN_256];/* 报警描述 */
    double longitude;                      /* 经度 */
    double latitude;                       /* 纬度 */
    int  alarm_type;                       /* <Info><AlarmType> 报警类型 */
    char alarm_type_param[GB_STR_LEN_256]; /* <Info><AlarmTypeParam> 报警类型扩展参数 */
};

// ---------------------------------------------------------------------------
// GB28181 SIP MESSAGE XML 报文组装器
//
// 覆盖 GB/T 28181-2016 附录 D 中前端设备需要处理 / 上报的全部 SIP MESSAGE 指令，
// 以及平台约定的非标扩展指令（远程升级、灯光控制、算法配置、报警图片/视频上传）。
// 语音对讲（Talk）走 SIP INVITE 会话，无 XML 消息体，不在本类中处理。
//
// 说明:
//  - 返回值为报文长度(不含结尾 '\0')，失败返回 -1；
//  - out_buff 由调用方保证足够大（建议不小于 4KB，Catalog/PresetQuery 按数据量放大）；
//  - status == 0 表示成功 -> <Result>OK</Result>，其余为 <Result>ERROR</Result>；
//  - 非标扩展指令的 CmdType 字符串集中在 .cpp 内定义，平台约定不同时只需改字符串。
// ---------------------------------------------------------------------------
class XmlPacketBuilder
{
public:
    XmlPacketBuilder();
    ~XmlPacketBuilder();

    void release_doc();

    // ----------------------- 查询应答类 -----------------------
    /* Catalog 设备目录查询应答 */
    int xml_pack_catalog(gb28181_system_param *sys_param, int *channel_status, const char *sn, char *out_buff);
    /* DeviceInfo 设备信息查询应答 */
    int xml_pack_deviceinfo(gb28181_system_param *sys_param, const char *sn, char *out_buff);
    /* DeviceStatus 设备状态查询应答 */
    int xml_pack_devicestatus(const char *device_id, const gb_device_status_t *status, const char *sn, char *out_buff);
    /* RecordInfo 录像文件查询应答 */
    int xml_pack_recordinfo(const char *device_id, const gb_record_info_t *records, int record_num, const char *sn, char *out_buff);
    /* ConfigDownload 配置文件查询应答(config_name 如 BasicParam/VideoParam, config_content 为文本内容) */
    int xml_pack_configdownload(const char *device_id, const char *config_name, const char *config_content, const char *sn, char *out_buff);
    /* PresetQuery 预置位查询应答(取 msg_param 中 sn/device_id/preset 数组) */
    int xml_pack_preset_query(gb28181_message_param_t *msg_param, char *out_buff);
    /* DeviceConfig 设备配置查询应答 */
    int xml_pack_deviceconfig(gb28181_system_param *sys_param, const char *sn, char *out_buff);

    // ----------------------- 控制应答类 -----------------------
    /* DeviceConfig 设备配置下发应答 */
    int xml_pack_device_config_ack(const char *device_id, int status, const char *sn, char *out_buff);
    /* DeviceControl 云台控制应答(取 msg_param 中 sn/device_id/ptz_status) */
    int xml_pack_ptz_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* Guard 布防/撤防应答(取 msg_param 中 sn/device_id/guard_status) */
    int xml_pack_guard_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* HomePosition 看守位控制应答(取 msg_param 中 home_* 字段) */
    int xml_pack_home_position_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* TeleBoot 远程启动/重启应答(取 msg_param 中 sn/device_id/reboot_status) */
    int xml_pack_teleboot_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* ManualRecord 手动录像控制应答 */
    int xml_pack_manual_record_ack(const char *device_id, int status, const char *sn, char *out_buff);
    /* Broadcast 语音广播应答 */
    int xml_pack_broadcast_ack(const char *device_id, const char *broadcast_id, const char *sn, char *out_buff);

    // ----------------------- 通知上报类 -----------------------
    /* Keepalive 心跳上报 */
    int xml_pack_keepalive(const char *device_id, const char *sn, char *out_buff);
    /* MobilePosition 移动位置查询应答(Response) */
    int xml_pack_mobile_position_response(const char *device_id, const gb_mobile_position_t *position, const char *sn, char *out_buff);
    /* MobilePosition 移动位置主动上报(Notify) */
    int xml_pack_mobile_position_notify(const char *device_id, const gb_mobile_position_t *position, const char *sn, char *out_buff);
    /* Alarm 报警通知上报(Notify) */
    int xml_pack_alarm_notify(const char *device_id, const gb_alarm_info_t *alarm, const char *sn, char *out_buff);
    /* MediaStatus 媒体流状态上报(Notify, notify_type 如 "121" 媒体流发送) */
    int xml_pack_media_status_notify(const char *device_id, const char *notify_type, const char *sn, char *out_buff);

    // ----------------------- 非标扩展指令应答(按平台约定可调整 CmdType) -----------------------
    /* 远程升级应答(取 msg_param 中 upgrade_status/firmware/file_url) */
    int xml_pack_upgrade_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* 灯光/开关控制应答(取 msg_param 中 light_status/switch_enable/switch_no) */
    int xml_pack_light_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* 算法配置应答(取 msg_param 中 alg_* 字段) */
    int xml_pack_alg_config_ack(gb28181_message_param_t *msg_param, char *out_buff);
    /* 报警图片/视频上传应答(取 msg_param 中 upload_status/pic_url/video_url) */
    int xml_pack_alarm_upload_ack(gb28181_message_param_t *msg_param, char *out_buff);

    // ----------------------- 通用应答 -----------------------
    /* 通用 <Response> 应答: CmdType + SN + DeviceID + Result, 供无附加字段的指令复用 */
    int xml_pack_common_ack(const char *cmd_type, const char *device_id, int status, const char *sn, char *out_buff);

private:
    /* 创建带 UTF-8 声明的文档与根节点, 失败返回 nullptr */
    xmlDocPtr create_doc(xmlNodePtr &root, const char *root_name);
    /* 序列化文档到 out_buff, 成功返回长度(含释放 doc), 失败返回 -1 */
    int dump_to_buffer(xmlDocPtr doc, char *out_buff);
    /* 添加文本子节点, name/content 任一为空时跳过 */
    xmlNodePtr add_child(xmlNodePtr parent, const char *name, const char *content);
    /* 添加 MobilePosition 公共字段: Time/Longitude/Latitude/Speed/Direction/Altitude */
    void add_mobile_position_nodes(xmlNodePtr root, const gb_mobile_position_t *position);

    xmlDocPtr        m_doc{nullptr};
    xmlNodePtr       m_root_node{nullptr};       // 根节点 <Response>
    xmlNodePtr       m_device_list_node{nullptr};// <DeviceList>
    xmlNodePtr       m_item_node{nullptr};       // <Item> 单个通道节点
};
