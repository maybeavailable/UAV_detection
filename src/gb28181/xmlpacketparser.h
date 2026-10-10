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
// 平台发来 XML 报文的根节点类型
// ---------------------------------------------------------------------------
enum gb_xml_root_e
{
    GB_XML_ROOT_UNKNOWN  = 0,
    GB_XML_ROOT_QUERY,      // <Query>    平台查询类指令(DeviceInfo/DeviceStatus/Catalog/...)
    GB_XML_ROOT_CONTROL,    // <Control>  平台控制类指令(DeviceControl/Guard/Broadcast/...)
    GB_XML_ROOT_RESPONSE,   // <Response> 平台应答(如对设备报警上报的确认)
    GB_XML_ROOT_NOTIFY      // <Notify>   平台通知(设备侧一般不会收到，兼容解析公共字段)
};

// ---------------------------------------------------------------------------
// GB28181 SIP MESSAGE XML 报文解析器（解析平台 → 设备方向的报文）
//
// 覆盖 GB/T 28181-2016 中平台发来的全部指令类型，以及非标扩展指令
// (Upgrade/LightControl/AlgConfig/AlarmUpload)：
//   <Query>    DeviceInfo/DeviceStatus/Catalog/RecordInfo/ConfigDownload/
//              PresetQuery/DeviceConfig/MobilePosition
//   <Control>  DeviceControl(PTZ)/Guard/HomePosition/TeleBoot/ManualRecord/
//              Broadcast + 非标扩展
//   <Response> 公共字段 + Result
//   <Notify>   公共字段(兼容)
//
// 使用方式：
//   gb28181_message_param_t msg;
//   XmlPacketParser parser;
//   if (parser.parse(body, len, &msg) == 0)
//   {
//       // 按 parser.root_type() 与 msg.cmd_type 分派处理；
//       // SIP 层的 session_id 等需在 parse 之后补写(parse 内部会先清零 msg)
//   }
// ---------------------------------------------------------------------------
class XmlPacketParser
{
public:
    XmlPacketParser();
    ~XmlPacketParser();

    void release_doc();

    // 解析平台发来的 SIP MESSAGE XML 消息体。
    // 成功返回 0，失败返回 -1；结果写入 msg_param(内部先清零，只填报文出现的字段)。
    int parse(const char *xml_buff, int xml_len, gb28181_message_param_t *msg_param);

    // parse 成功后的根节点类型
    gb_xml_root_e root_type() const { return m_root_type; }

private:
    // ---- 内部通用辅助 ----
    static xmlNodePtr find_child(xmlNodePtr parent, const char *name);              // 按名找直接子节点
    static const char* node_content(xmlNodePtr node);                               // 取节点文本(不分配内存)
    static const char* child_content(xmlNodePtr parent, const char *name);          // 直接子节点文本
    static int parse_int(xmlNodePtr parent, const char *name, int default_val);     // 子节点整数值
    static int copy_str(char *dst, int size, const char *src);                      // 安全拷贝到定长缓冲

    void parse_common(xmlNodePtr root, gb28181_message_param_t *msg_param);         // CmdType/SN/DeviceID
    int  parse_ptz_cmd(const char *ptz_cmd, gb28181_message_param_t *msg_param);    // PTZCmd 十六进制解码
    void parse_per_cmdtype(xmlNodePtr root, gb28181_message_param_t *msg_param);    // 按 CmdType 解析附加字段

    xmlDocPtr     m_doc{nullptr};
    xmlNodePtr    m_root_node{nullptr};          // 根节点 <Query>/<Control>/<Response>/<Notify>
    gb_xml_root_e m_root_type{GB_XML_ROOT_UNKNOWN};
};
