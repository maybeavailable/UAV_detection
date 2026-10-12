#pragma once

#include"common/common.h"

#include"common/urt.h"
// === 枚举定义 ===

// 图像属性类型
enum ImagePropertyType
{
    BRIGHTNESS = 0x10, // 亮度
    CONTRAST = 0x11,   // 对比度
    SATURATION = 0x12, // 饱和度
    TONE = 0x13        // 色调
};

// 白平衡模式
enum WhiteBalanceMode
{
    WB_AUTO = 0x00,       // 自动白平衡
    WB_INDOOR = 0x01,     // 室内模式
    WB_OUTDOOR = 0x02,    // 室外模式
    WB_ATW = 0x03,        // 自动跟踪白平衡
    WB_MANUAL = 0x04,     // 手动模式
    WB_SODIUM = 0x05,     // 钠灯模式
    WB_NATURAL = 0x06,    // 自然光模式
    WB_STREETLAMP = 0x07, // 路灯模式
    WB_ONEPUSH = 0x08     // 单触发模式
};

// 图像风格
enum ImageStyle
{
    STYLE_STANDARD = 0x00, // 标准风格
    STYLE_GORGEOUS = 0x01, // 华丽风格
    STYLE_GENTLE = 0x02    // 温和风格
};

// 曝光模式
enum ExposureMode
{
    EXPOSURE_AUTO = 0x00,             // 自动曝光
    EXPOSURE_MANUAL = 0x03,           // 手动曝光
    EXPOSURE_SHUTTER_PRIORITY = 0x0A, // 快门优先
    EXPOSURE_IRIS_PRIORITY = 0x0B,    // 光圈优先
    EXPOSURE_GAIN_PRIORITY = 0x0D     // 增益优先
};

// 曝光控制类型
enum ExposureControlType
{
    APERTURE_UP = 0x0B02,     // 光圈增加
    APERTURE_DOWN = 0x0B03,   // 光圈减少
    APERTURE_RESET = 0x0B00,  // 光圈复位
    APERTURE_DIRECT = 0x4B00, // 光圈直接设置
    GAIN_UP = 0x0C02,         // 增益增加
    GAIN_DOWN = 0x0C03,       // 增益减少
    GAIN_RESET = 0x0C00,      // 增益复位
    GAIN_DIRECT = 0x4C00,     // 增益直接设置
    SHUTTER_UP = 0x0A02,      // 快门增加
    SHUTTER_DOWN = 0x0A03,    // 快门减少
    SHUTTER_RESET = 0x0A00,   // 快门复位
    SHUTTER_DIRECT = 0x4A00,  // 快门直接设置
    EXP_COMP_UP = 0x0E02,     // 曝光补偿增加
    EXP_COMP_DOWN = 0x0E03,   // 曝光补偿减少
    EXP_COMP_RESET = 0x0E00,  // 曝光补偿复位
    EXP_COMP_DIRECT = 0x4E00  // 曝光补偿直接设置
};

// 锐化控制类型
enum SharpnessControlType
{
    SHARPNESS_UP = 0x0202,     // 锐化增加
    SHARPNESS_DOWN = 0x0203,   // 锐化减少
    SHARPNESS_RESET = 0x0200   // 锐化复位
};

// 变焦模式
enum ZoomMode
{
    ZOOM_OPTICAL_ONLY = 0x03,   // 仅光学变焦
    ZOOM_DIGITAL_OPTICAL = 0x02 // 数字+光学变焦
};

// 变焦控制类型
enum ZoomControlType
{
    ZOOM_STOP = 0x0700,  // 停止变焦
    ZOOM_TELE = 0x0727,  // 变倍到长焦
    ZOOM_WIDE = 0x0737,  // 变倍到广角
    ZOOM_DIRECT = 0x4700 // 直接变焦
};

// 聚焦模式
enum FocusMode
{
    FOCUS_AUTO = 0x02,  // 自动聚焦
    FOCUS_MANUAL = 0x03 // 手动聚焦
};

// 聚焦控制类型
enum FocusControlType
{
    FOCUS_STOP = 0x0800,           // 停止聚焦
    FOCUS_FAR = 0x0827,            // 聚焦到远焦
    FOCUS_NEAR = 0x0837,           // 聚焦到近焦
    FOCUS_AUTO_TRIGGER = 0x3802,   // 自动聚焦
    FOCUS_MANUAL_TRIGGER = 0x3803, // 手动聚焦
    FOCUS_ONE_PUSH = 0x1801,       // 一键聚焦
    FOCUS_DIRECT = 0x4800          // 直接聚焦
};

// 聚焦灵敏度
enum FocusSensitivity
{
    FOCUS_SENSITIVITY_LOW = 0x00,    // 低灵敏度
    FOCUS_SENSITIVITY_NORMAL = 0x01, // 正常灵敏度
    FOCUS_SENSITIVITY_HIGH = 0x02    // 高灵敏度
};

// 自动对焦模式
enum AutoFocusMode
{
    AF_AUTO = 0x00,           // 自动模式
    AF_SEMI_AUTO = 0x01,      // 半自动模式
    AF_MANUAL = 0x02,         // 手动模式
    AF_FAST_SEMI_AUTO = 0x03, // 快速半自动
    AF_FAST_AUTO = 0x04       // 快速自动
};

// ICR模式
enum ICRMode
{
    ICR_OFF = 0x03, // 关闭ICR
    ICR_ON = 0x02   // 开启ICR
};

// 自动ICR模式
enum AutoICRMode
{
    AUTO_ICR_OFF = 0x03, // 关闭自动ICR
    AUTO_ICR_ON = 0x02   // 开启自动ICR
};

// 除雾模式
enum DefogMode
{
    DEFOG_OFF = 0x00,   // 关闭除雾
    DEFOG_AUTO = 0x01,  // 自动除雾
    DEFOG_MANUAL = 0x02 // 手动除雾
};

// 除雾级别
enum DefogLevel
{
    DEFOG_LEVEL_LOW = 0x00,    // 低级别
    DEFOG_LEVEL_MIDDLE = 0x01, // 中级别
    DEFOG_LEVEL_HIGH = 0x02    // 高级别
};

// 热浪减少模式
enum HeatHazeMode
{
    HEAT_HAZE_OFF = 0x00,   // 关闭热浪减少
    HEAT_HAZE_AUTO = 0x01,  // 自动热浪减少
    HEAT_HAZE_MANUAL = 0x02 // 手动热浪减少
};

// 热浪减少级别
enum HeatHazeLevel
{
    HEAT_HAZE_LEVEL_LOW = 0x00,    // 低级别
    HEAT_HAZE_LEVEL_MIDDLE = 0x01, // 中级别
    HEAT_HAZE_LEVEL_HIGH = 0x02    // 高级别
};

// 防抖模式
enum StabilizerMode
{
    STABILIZER_OFF = 0x00,    // 关闭防抖
    STABILIZER_EIS = 0x01,    // 电子防抖
    STABILIZER_OIS = 0x02,    // 光学防抖
    STABILIZER_EIS_OIS = 0x03 // 电子+光学防抖
};

// WDR模式
enum WDRMode
{
    WDR_OFF = 0x03, // 关闭WDR
    WDR_ON = 0x02   // 开启WDR
};

// 防闪烁模式
enum AntiFlickerMode
{
    ANTI_FLICKER_OUTDOOR = 0x00, // 户外模式(不防闪烁)
    ANTI_FLICKER_50HZ = 0x01,    // 50Hz防闪烁
    ANTI_FLICKER_60HZ = 0x02     // 60Hz防闪烁
};

// OSD功能
enum OSDFunction
{
    OSD_TIME_TITLE = 0x00,    // 时间标题
    OSD_CHANNEL_TITLE = 0x01, // 通道标题
    OSD_GPS_INFO = 0x02,      // GPS信息
    OSD_ZOOM_VALUE = 0x03,    // 变倍值
    OSD_CROSSHAIR = 0x04      // 十字准心
};

// OSD区域
enum OSDRegion
{
    OSD_NETWORK = 0x00, // 网络OSD
    OSD_DIGITAL = 0x01  // 数字OSD
};

// 视频格式
enum VideoFormat
{
    FORMAT_1080_30P = 0x01, // 1080P@30fps
    FORMAT_1080_25P = 0x02, // 1080P@25fps
    FORMAT_1080_60I = 0x03, // 1080i@60fps
    FORMAT_1080_50I = 0x04, // 1080i@50fps
    FORMAT_720_60P = 0x05,  // 720P@60fps
    FORMAT_720_50P = 0x06,  // 720P@50fps
    FORMAT_1080_60P = 0x07, // 1080P@60fps
    FORMAT_1080_50P = 0x08,  // 1080P@50fps
    FORMAT_1520_60P = 0x0D,  // 2688×1520@60fps
    FORMAT_1520_50P = 0x0E,  // 2688×1520@50fps
    FORMAT_1520_30P = 0x0F, // 2688×1520@30fps
    FORMAT_1520_25P = 0x10  // 12688×1520@25fps
};

// 波特率
enum BaudRate
{
    BAUD_9600 = 0x00,  // 9600波特率
    BAUD_19200 = 0x01, // 19200波特率
    BAUD_38400 = 0x02, // 38400波特率
    BAUD_115200 = 0x03 // 115200波特率
};

// 主动上报数据类型
enum ShihuiReportType
{
    SHIHUI_ZOOM_POSITION = 0x07,     // 变焦位置上报
    SHIHUI_FOCUS_POSITION = 0x08,    // 聚焦位置上报
    SHIHUI_INITIALIZE_COMPLETE = 0x38,  // 初始化完成上报
    SHIHUI_OTHER = 0xFF              // 其他上报数据
};

// === 协议返回码处理 ===
enum ResponseCode
{
    RESPONSE_COMMAND_RECEIVED = 0x41,  // 命令接收成功
    RESPONSE_COMMAND_COMPLETED = 0x51, // 命令执行完成
    RESPONSE_POWER_ON = 0x38,          // 上电启动完成
    RESPONSE_LENGTH_ERROR = 0x60,      // 信息长度错误
    RESPONSE_SYNTAX_ERROR = 0x61,      // 语法错误
    RESPONSE_BUFFER_FULL = 0x62,       // 命令缓冲区已满
    RESPONSE_COMMAND_CANCELLED = 0x63, // 命令已取消
    RESPONSE_NO_SOCKET = 0x64,         // 无插座
    RESPONSE_NOT_EXECUTABLE = 0x65     // 命令不可执行
};




// === 相机控制类 ===

class ShihuiCore
{
private:
    std::string port_;       // 串口设备路径
    uint64_t baud_; // 波特率

    // 添加UART对象指针
    std::unique_ptr<Urt> uart_obj_;     // UART对象指针（内部自带线程安全的接收线程）

    // 连接状态监控
    bool auto_reconnect_;        // 自动重连标志
    int32_t max_reconnect_attempts_; // 最大重连尝试次数
    int32_t reconnect_delay_ms_;     // 重连延迟时间(毫秒)

    // === 异步数据接收 ===
    // 接收线程由 Urt 内部管理（UartObj->start_receive/stop_receive），本类不再自建线程；
    // frame_buffer_ 只在接收回调所在的线程里访问，因此无需加锁
    std::vector<uint8_t> frame_buffer_;   // 半帧累积缓冲

    // === 改进的异步处理相关成员变量 ===
    struct PendingCommand
    {
        uint8_t expected_final_response_socket; // 期望的最终响应Socket (如 0x51 for set, 0x50 for inquiry)
        std::vector<uint8_t> response_data;     // 存储收到的最终响应数据
        bool response_received;                  // 最终响应是否已收到
        std::condition_variable *cv;            // 指向主线程条件变量的指针，用于通知
    };

    std::queue<PendingCommand *> pending_commands_queue_; // 待确认命令的FIFO队列
    std::mutex command_mutex_;                       // 保护命令队列和响应的互斥锁
    // 串口写不需要额外加锁：Urt::write 内部已串行化
    std::condition_variable command_cv_;             // 用于通知主线程响应已到达

    // 主动上报数据回调函数
    std::function<void(ShihuiReportType, const uint8_t *, int32_t)> active_report_callback_;
    // === 异步接收处理相关函数 ===
    void onReceive(const uint8_t *data, std::size_t len);   // Urt 接收回调：累积字节
    void processFrameBuffer(std::vector<uint8_t> &frame_buffer);
    void processReceivedFrame(const uint8_t *frame, int32_t frame_len);
    bool waitForResponse(uint8_t expected_socket, std::vector<uint8_t> &response, int32_t timeout_ms);
    bool sendQueryCommandAndWaitForResponse(const uint8_t *cmd, int32_t cmd_len, uint8_t *response, int32_t &resp_len,
                                            int32_t timeout_ms, int32_t max_response_len);
    uint8_t calculateChecksum(const uint8_t *data, int32_t length);

public:
    // === 连接管理函数 ===
    bool connect();
    void disconnect();
    bool isConnected() const;
    bool reconnect(); // 重新连接函数

    // === 连接状态监控函数 ===
    void setAutoReconnect(bool enable);
    void setMaxReconnectAttempts(int32_t attempts);
    void setReconnectDelay(int32_t delay_ms);
    bool getAutoReconnect() const;
    int32_t getMaxReconnectAttempts() const;
    int32_t getReconnectDelay() const;

    // === 主动上报数据处理函数 ===
    void setShihuiReportCallback(std::function<void(ShihuiReportType, const uint8_t *, int32_t)> callback);

    ShihuiCore(std::string port, uint64_t baud = 9600);
    ~ShihuiCore();

    // === 底层通信 ===
    bool sendCommand(const uint8_t *data, int32_t length);
    bool receiveResponse(uint8_t *response, int32_t &length);
    bool parseResponseCode(const uint8_t *response, int32_t length, ResponseCode &code);

    // === 图像参数设置 ===
    bool setImageProperty(ImagePropertyType type, uint8_t value);
    bool setWhiteBalance(WhiteBalanceMode mode);
    bool setRBgain(uint8_t rGain, uint8_t bGain);
    bool setGamma(uint8_t value);
    bool setSharpness(uint8_t value);
    bool setImageStyle(ImageStyle style);
    bool setNoiseReduction2D(uint8_t level);
    bool setNoiseReduction3D(uint8_t level);
    bool setAINR(uint8_t mode, uint8_t level);
    bool setPseudoColor(uint8_t color);
    bool freezeImage(bool freeze);
    bool setHighlightCompensation(bool on);
    bool setWDRLevel(uint8_t mode, uint8_t level);

    // === 曝光控制 ===
    bool setExposureMode(ExposureMode mode);
    bool setExposure(uint8_t mode, uint8_t apertureMin, uint8_t apertureMax,
                     uint32_t shutterMin, uint32_t shutterMax, uint8_t gainMin,
                     uint8_t gainMax);
    bool setExposureControl(ExposureControlType type, uint8_t value = 0);
    bool setSharpnessControl(SharpnessControlType type);
    bool setAntiFlickerMode(AntiFlickerMode mode);

    // === 变焦控制 ===
    bool setZoomControl(ZoomControlType type, uint16_t position = 0);
    bool setThermalDZoom(uint8_t position);
    bool setDirectZoom(uint32_t position, bool isThermal = false);
    bool setCustomZoom(uint32_t position, bool isThermal = false);  // 添加自定义变焦命令
    bool setOriginalZoom(uint32_t position, bool isThermal = false);  // 添加原始变倍比变焦命令
    bool setZoomFocus(uint32_t zoomPosition, uint32_t focusPosition, bool isThermal = false);  // 添加变焦对焦联动命令
    bool setZoomMode(ZoomMode mode);

    // === 聚焦控制 ===
    bool setFocusControl(FocusControlType type, uint16_t position = 0);
    bool setFocusSensitivity(FocusSensitivity sensitivity);
    bool setFocusLimit(uint16_t limit);
    bool setRoiFocus(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    bool setAutoFocusMode(AutoFocusMode mode);

    // === 功能设置 ===
    bool setICR(ICRMode mode);
    bool setAutoICR(AutoICRMode mode);
    bool setAutoICRThreshold(uint8_t threshold);
    bool setDayNightSwitchDelay(uint8_t type, uint8_t delay);
    bool setDefogLevel(DefogMode mode, uint8_t level);
    bool setHeatHaze(HeatHazeMode mode, HeatHazeLevel level);
    bool setStabilizerMode(StabilizerMode mode);
    bool setOSDDisplay(uint8_t channel, OSDRegion region, OSDFunction function,
                       bool enable);
    bool setCustomOSD(const char *text);
    bool setVideoFormat(VideoFormat visibleFormat, uint8_t thermalFormatHigh = 0,
                        uint8_t thermalFormatLow = 0);
    bool setFormat(uint8_t format);
    bool setHorizontalResolution(uint32_t resolution);
    bool setVerticalResolution(uint32_t resolution);
    bool setRegisterValue(uint8_t regNum, uint8_t value);
    bool setLockMode(bool lock);
    bool setImageFlip(uint8_t mode);

    // === 快照和录像 ===
    bool setSnap(uint8_t count, uint8_t delay);
    bool setSnapOnePush();
    bool setRecord(bool start);

    // === OSD功能 ===
    bool setThermalGPSOSD(bool enable);
    bool setZoomOSD(bool enable);
    bool setThermalTimeTitleOSD(bool enable);

    // === 系统功能 ===
    bool setInitialize();
    bool setImageInitialize();
    bool setPowerOff();
    bool setBaudRate(BaudRate rate);

    // === 查询命令 ===
    bool getImageProperty(ImagePropertyType type, uint8_t &value);
    bool getWhiteBalance(WhiteBalanceMode &mode, uint8_t &rGain, uint8_t &bGain);
    bool getGamma(uint8_t &value);
    bool getSharpness(uint8_t &value);
    bool getCameraInhibition(uint8_t &value);
    bool getNR2D(uint8_t &value);
    bool getNR3D(uint8_t &value);
    bool getAdvancedNR(uint8_t &mode, uint8_t &level);
    bool getImageBrightness(uint8_t &value);
    bool getImageContrast(uint8_t &value);
    bool getImageSaturation(uint8_t &value);
    bool getImageTone(uint8_t &value);
    bool getShutterPosition(uint8_t &value);
    bool getGainPosition(uint8_t &value);
    bool getIrcGain(uint8_t &value);
    bool getExposureCompensationPosition(uint8_t &value);
    bool getAEMode(uint8_t &mode);
    bool getZoomPosition(uint16_t &position);
    bool getZoomMode(uint8_t &mode);
    bool getMaximumZoomRatio(uint8_t &opticalZoom, uint8_t &digitalZoom);
    bool getCurrentZoomPosition(uint32_t &targetPosition, uint32_t &originalZoomRatio, uint16_t &zoomPosition);
    bool getContinuousZoomPos(uint8_t &mode);
    bool getFocusPosition(uint16_t &position);
    bool getFocusMode(uint8_t &mode);
    bool getFocusSensitivity(uint8_t &sensitivity);
    bool getAFMode(uint8_t &mode);
    bool getFocusNearLimit(uint16_t &limit);
    bool getAFSensitivity(uint8_t &sensitivity);
    bool getPictureFlipMode(uint8_t &mode);
    bool getICRMode(uint8_t &mode);
    bool getVisibleLightVideoFormat(uint8_t &format);
    bool getThermalVideoFormat(uint8_t &format);
    bool getAutoICRMode(uint8_t &mode);
    bool getStabilizerMode(uint8_t &mode);
    bool getWDRMode(uint8_t &mode);
    bool getWD(uint8_t &level);
    bool getImageStabilizer(uint8_t &mode);
    bool getICREnableMode(uint8_t &mode);
    bool getAutoICRThreshold(uint8_t &threshold);
    bool getDayNightSwitchDelay(uint8_t type, uint8_t &delay);
    bool getElectronicFogMode(uint8_t &mode);
    bool getFogPenetrationLevel(uint8_t &level);
    bool getMipiAcquisition(uint8_t &mode);
    bool getFormat(uint8_t &format);
    bool getHorizontalResolution(uint16_t &resolution);
    bool getVerticalResolution(uint16_t &resolution);
    bool getMipiFormat(uint8_t &formatHigh, uint8_t &formatLow);
    bool getLRReverseMode(uint8_t &mode);
    bool getSlowShutterMode(uint8_t &mode);
    bool getHighlightCompensationMode(uint8_t &mode);
    bool getHighlightCompensationStrength(uint8_t &strength);
    bool getStyle(uint8_t &style);
    bool getDZoomMode(uint8_t &mode);
    bool getVersion(uint16_t &vendorId, uint16_t &modelId, uint8_t &majorVersion,
                    uint8_t &minorVersion, uint8_t &fixVersion);
    bool getProgramVersion(uint16_t &year, uint8_t &month, uint8_t &day,
                           uint8_t &serialNumber);
    bool getLensZoomInfo(uint32_t &zoomTimes, uint32_t &zoomDirectionTimes);
    bool getLensFocusInfo(uint32_t &focusTimes, uint32_t &focusDirectionTimes);
    bool getLensIrisInfo(uint32_t &irisTimes, uint32_t &icrTimes);
    bool getRegisterValue(uint16_t regNum, uint8_t &value);
    bool getLockMode(uint8_t &mode);
    bool getHeatHaze(HeatHazeMode &mode, HeatHazeLevel &level);
    bool getTemperature(float &temperature);
};
