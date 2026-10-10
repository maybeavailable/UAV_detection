#pragma once


#define GB_SUCCESS          0   /**< return successful */
#define GB_FAILURE          -1  /**< return failure */
#define GB_FMT_MAX_NUM      4
#define GB_CAMERA_MAX       4
#define GB_PUSHER_MAX       4  //单个摄像机码流推流最大数
#define LOCAL_MEDIA_PORT    20000

#define GB_STR_LEN_12       12
#define GB_STR_LEN_16       16
#define GB_STR_LEN_32       32
#define GB_STR_LEN_64       64
#define GB_STR_LEN_128      128
#define GB_STR_LEN_256      256
#define GB_STR_LEN_350      350
#define GB_STR_LEN_512      512


struct camera_param_t
{
    int  channel_index; //从0开始
    char channel_id[GB_STR_LEN_32]; //GB ID
    char channel_ip[GB_STR_LEN_32];
    char channel_name[GB_STR_LEN_32];
    char username[GB_STR_LEN_32];
    char password[GB_STR_LEN_32];
    char rtsp_url[GB_STR_LEN_128];

};


struct gb28181_system_param
{
    //国标平台信息
    //bool enable;                                    /* 使能开关 */
    char sipserver_id[GB_STR_LEN_32];               /**< SIP服务器ID >**/
    char sipserver_region[GB_STR_LEN_32];           /**< SIP服务器域 >**/
    char sipserver_ip[GB_STR_LEN_32];               /**< SIP服务器IP>**/
    int  sipserver_port;                            /**< SIP服务器端口>**/
    
    //本机国标信息
    char sipdev_reg_id[GB_STR_LEN_32];       /**< 本机SIP设备ID >**/
    char sipdev_password[GB_STR_LEN_32];     /**< SIPͨ注册密码 >**/
    int  heartbeatInterval;                         /**< SIP心跳周期间隔 >**/
    int  deadline;                                  /**< SIP注册有效期 >**/
    int  register_interval;                         /**< SIP 注册间隔 >**/
    int  sip_localport;                             /**< SIP本地端口 >**/
    int  sip_transport;                             /**< SIPͨ传输协议(0:UDP;1:TCP) >**/
    int  heartbeat_timeout;                         /**< 心跳最大超时次数 >**/
    int  upload_type;                              //报警上传类型 1-只需要图片上传；2-只需要短视频；3-图片上传和短视频上传
    int  pre_rec_time;                             //预录时长，单位秒，最大10
    int  delay_rec_time;                           //延录时长，单位秒，最大10
    
    int  enable;                                   // 国标功能是否启用（0：不启用，1：启用）    

    //本机设备信息
    char local_ip[GB_STR_LEN_32];	            /**< 本机设备IP >**/
    char dev_model[GB_STR_LEN_32];                  /**< 设备型号 >**/
    char dev_version[GB_STR_LEN_64];                /**< 设备版本 >**/
    char dev_manufacturer[GB_STR_LEN_64];           /**< 设备厂商 >**/
    char dev_name[GB_STR_LEN_128];                   /**< 设备名称 >**/
    
    //摄像机通道信息
    int channel_size;
    camera_param_t channel_para[GB_CAMERA_MAX];        /**< 视频通道ID >**/
    
};



enum ptz_cmd_e
{
    PTZ_CMD_LEFT = 0,
    PTZ_CMD_LEFT_TOP,
    PTZ_CMD_TOP,
    PTZ_CMD_RIGHT_TOP,
    PTZ_CMD_RIGHT,
    PTZ_CMD_RIGHT_DOWN,
    PTZ_CMD_DOWN,
    PTZ_CMD_LEFT_DOWN,
    PTZ_CMD_ZOOM_ADD,
    PTZ_CMD_ZOOM_REDUCE,
    PTZ_CMD_FOCUS_ADD,
    PTZ_CMD_FOCUS_REDUCE,
    PTZ_CMD_STOP,
    PTZ_CMD_GET_PRESET,
    PTZ_CMD_SET_PRESET,
    PTZ_CMD_MODIFY_PRESET,
    PTZ_CMD_GOTO_PRESET,
    PTZ_CMD_DELETE_PRESET
};





struct gb28181_message_param_t
{
    char  cmd_type[GB_STR_LEN_32];
    char  sn[GB_STR_LEN_64];
    char  device_id[GB_STR_LEN_32];
    
    //reboot
    char   reboot_status;
    
    //guard
    char   guard_status;
    
    //upgrade
    char   upgrade_status;
    char   file_url[GB_STR_LEN_256];
    char   firmware[GB_STR_LEN_64];
    
    //light
    char  light_status;
    char  switch_enable;
    char  switch_no[GB_STR_LEN_32];
    
    //ptz
    char  ptz_status;
    ptz_cmd_e ptz_cmd;
    char  session_id[GB_STR_LEN_64];
    int   ptz_speed; //00-FF
    
    int preset_num;
    char preset_name[GB_STR_LEN_350][GB_STR_LEN_64];
    char preset_id[GB_STR_LEN_350][GB_STR_LEN_32]; 
    
    char home_status;
    int  home_enabled;
    int  home_reset_time;
    int  home_preset_id;
    
    //alarm upload pic and video
    char  upload_status;
    char  pic_url[GB_STR_LEN_256];
    char  pic_path[GB_STR_LEN_256];
    char  video_url[GB_STR_LEN_256];
    char  video_path[GB_STR_LEN_256];
     
    //ALG CONFIG
    char  alg_status;
    char  device_name[GB_STR_LEN_32];
    char  channel_id[GB_STR_LEN_32];
    char  channel_name[GB_STR_LEN_32];
    int   opt_type;
    int   extraction_type;
    float confidence_thresh;
    int   alarm_interval;

    //平台应答 <Response> 的 Result: 0=OK, 非0=ERROR(解析平台回应时使用)
    int   result_status;

    //DeviceControl(PTZCmd) 预置位操作携带的预置位编号
    int   ptz_preset_num;

    //RecordInfo 录像查询条件(平台查询时携带)
    char  record_start_time[GB_STR_LEN_64];
    char  record_end_time[GB_STR_LEN_64];
    char  record_type[GB_STR_LEN_16];
    int   record_secrecy;
    int   indistinct_query;

    //ConfigDownload/DeviceConfig 携带的配置类型节点名(如 BasicParam/VideoParam)
    char  config_type[GB_STR_LEN_64];

    //MobilePosition 查询的主动上报间隔(秒)
    int   position_interval;

    //Broadcast 语音广播的源/目标设备ID
    char  broadcast_source_id[GB_STR_LEN_32];
    char  broadcast_target_id[GB_STR_LEN_32];

};
