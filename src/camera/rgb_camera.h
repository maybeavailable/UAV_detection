#pragma once

#include"common/common.h"
#include"camera/shihui_protocol.h"

static const std::string DEFAULT_CAMERA_PORT = "/dev/ttyCH9344USB6";
static const uint64_t DEFAULT_CAMERA_BAUD = 9600;

// ---------------------------------------------------------------------------
// RGBCamera —— 可见光相机控制（当前只接世辉 ShihuiCore，其它型号后续在此扩展）
//
// 说明：
//   * 本类由 CameraNode 的 SHIHUI 分支整理而来：方法名、入参含义、数值换算
//     与原实现保持一致，便于上层代码直接迁移；区别是不再有多型号 type 分发。
//   * 数值约定（与 CameraNode 一致）：
//       - 亮度/对比度/饱和度：0~100（内部换算成相机协议的 0~255）
//       - 锐化/伽马：0~100（内部换算成 0x00~0x0F）
//       - 连续变倍/聚焦速度：0~100（内部换算成协议档位 0x21~0x27 / 0x31~0x37，
//         传 0 表示用默认档位）
//       - 所有 set/get 返回 1=成功、0=失败（与 CameraNode 的 int32_t 约定一致）
//   * 连续运动方向（direction）：
//       变焦：0=停止 1=放大(TELE) 2=缩小(WIDE)
//       聚焦：0=停止 1=近焦(FOCUS+) 2=远焦(FOCUS-)
//     与 GB28181 的 ZOOM_ADD/ZOOM_REDUCE、FOCUS_ADD/FOCUS_REDUCE 语义一致，
//     可直接对应（GB28181 速度 0x00~0xFF 归一化到 0~100 后传入）。
//   * 线程约定：串口收发本身线程安全；但 connect/disconnect/init 属生命周期操作，
//     请由同一个控制线程调用，不要与其它线程的 connect/disconnect 并发。
//
// 相比原 CameraNode 修正的问题：
//   1. getGamma()：原来调用的是 setGamma()（把查询写成了设置，且用的是未初始化的入参）；
//   2. getExposureMode()：原来对未初始化的入参 mode 做 switch 再查询，多余且是未定义行为；
//   3. getAutoICRMode()：原来先把未初始化的局部变量赋给输出参数（未定义行为）；
//   4. setExposureControl()/setFocusLimit()/setAutoFocusMode()/setHeatHaze()：
//      原来存在无 return 的分支（函数末尾缺返回值）；
//   5. getVersionInfo()：原实现对世辉直接返回 0，而 ShihuiCore 有 getVersion()，这里接上了。
// ---------------------------------------------------------------------------
class RGBCamera
{
public:
    // 主动上报回调（变焦/聚焦位置、初始化完成等）；data 内存仅在回调期间有效
    using ReportCallback = std::function<void(ShihuiReportType type, const uint8_t *data, int32_t len)>;

    // 连续运动方向（与 CameraNode 的 direction 取值一致）
    enum : uint8_t
    {
        ZOOM_DIR_STOP = 0,  // 停止变焦
        ZOOM_DIR_IN = 1,    // 放大（TELE）
        ZOOM_DIR_OUT = 2    // 缩小（WIDE）
    };
    enum : uint8_t
    {
        FOCUS_DIR_STOP = 0, // 停止聚焦
        FOCUS_DIR_NEAR = 1, // 近焦（FOCUS+）
        FOCUS_DIR_FAR = 2   // 远焦（FOCUS-）
    };

    // 图像翻转模式（业务侧约定，与 CameraNode 一致）
    enum : uint8_t
    {
        FLIP_NONE = 0,       // 不翻转
        FLIP_HORIZONTAL = 1, // 水平翻转
        FLIP_VERTICAL = 2,   // 垂直翻转
        FLIP_BOTH = 3        // 水平+垂直
    };

    RGBCamera(const std::string &port = DEFAULT_CAMERA_PORT, uint64_t baud = DEFAULT_CAMERA_BAUD);
    ~RGBCamera();

    RGBCamera(const RGBCamera &) = delete;
    RGBCamera &operator=(const RGBCamera &) = delete;

    // 线性映射：把工程值映射到相机协议值，并把输入夹在 [in_min, in_max]
    template <typename T>
    static typename std::enable_if<std::is_arithmetic<T>::value, int32_t>::type
    map_value(T value, T in_min, T in_max, int32_t out_min, int32_t out_max)
    {
        if (value < in_min)
        {
            value = in_min;
        }
        if (value > in_max)
        {
            value = in_max;
        }
        return static_cast<int32_t>((value - in_min) * static_cast<double>(out_max - out_min) /
                                    (in_max - in_min) + out_min + 0.5);
    }

    // === 生命周期 ===
    bool connect();
    void disconnect();
    bool isConnected() const;
    bool reconnect();
    // 重新创建内核（换串口/换波特率用），旧连接会被断开
    bool init(const std::string &port, uint64_t baud);

    // 链路异常后的自动重连参数（透传 ShihuiCore，实际重连由上层调用 reconnect() 触发）
    void setAutoReconnect(bool enable);
    void setMaxReconnectAttempts(int32_t attempts);
    void setReconnectDelay(int32_t delay_ms);
    bool getAutoReconnect() const;
    int32_t getMaxReconnectAttempts() const;
    int32_t getReconnectDelay() const;

    // 主动上报回调
    void setReportCallback(ReportCallback callback);

    // 底层内核（需要访问本类未包装的接口时使用，例如查询类函数）
    ShihuiCore *core() { return shihui_camera_.get(); }

    // === 变倍（Zoom）===
    // 连续变倍：direction 见 ZOOM_DIR_*，speed 0~100（0 用默认速度）
    int32_t continuousZoom(uint8_t direction, uint8_t speed);
    // 变倍到绝对位置（可见光 0x0000~0x4000）
    int32_t zoomToPosition(uint16_t position);
    int32_t setCustomZoom(uint32_t position);
    int32_t setOriginalZoom(uint32_t position);
    // 变焦+对焦联动
    int32_t setZoomFocus(uint32_t zoomPosition, uint32_t focusPosition, bool isThermal = false);
    // 热成像数字变焦（0x10~0x80 对应 1~8 倍）
    int32_t setThermalDZoom(uint8_t position);
    // 变倍模式：0x03 仅光学 / 0x02 数字+光学
    int32_t setZoomMode(uint8_t mode);
    int32_t getZoomMode(uint8_t &mode);
    int32_t getMaximumZoomRatio(uint8_t &opticalZoom, uint8_t &digitalZoom);
    int32_t getZoomPosition(uint16_t &position);
    // 当前变倍信息（目标位置 / 原始倍率 / 缩放位置）
    int32_t getCurrentZoomPos(uint32_t &targetPosition, uint32_t &originalZoomRatio, uint16_t &zoomPosition);
    // 连续变焦位置主动上报开关：mode 2=启用 3=禁用（沿用原 CameraNode 的函数名）
    int32_t getContinuousZoomPos(uint8_t &mode);

    // === 聚焦（Focus）===
    int32_t continuousFocus(uint8_t direction, uint8_t speed);
    int32_t focusToPosition(uint16_t position);
    // 一键自动聚焦
    int32_t autoFocus();
    // 聚焦灵敏度：0 低 / 1 正常 / 2 高
    int32_t setFocusSensitivity(uint8_t sensitivity);
    int32_t getAFSensitivity(uint8_t &sensitivity);
    // 最近聚焦距离限制：0~7（0 最远，7 最近）
    int32_t setFocusLimit(uint16_t limit);
    int32_t getFocusNearLimit(uint16_t &limit);
    // ROI 区域对焦
    int32_t setRoiFocus(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    // 自动对焦模式：0 自动 / 1 半自动 / 2 手动 / 3 快速半自动 / 4 快速自动
    int32_t setAutoFocusMode(uint8_t mode);
    int32_t getAFMode(uint8_t &mode);
    int32_t getFocusMode(uint8_t &mode);
    int32_t getFocusPosition(uint16_t &position);

    // === 图像参数 ===
    // 亮度：value 0~100
    int32_t setBrightness(uint8_t mode, uint8_t value);
    int32_t getBrightness(uint8_t &value);
    // 对比度：value 0~100
    int32_t setContrast(uint8_t mode, uint8_t value);
    int32_t getContrast(uint8_t &value);
    // 饱和度：value 0~100
    int32_t setSaturation(uint8_t value);
    int32_t getSaturation(uint8_t &value);
    // 锐化：value 0~100
    int32_t setSharpness(uint8_t mode, uint8_t value);
    int32_t getSharpness(uint8_t &value);
    // 伽马：value 0~100
    int32_t setGamma(uint8_t value);
    int32_t getGamma(uint8_t &value);
    // 按协议属性类型直接设置：0x10 亮度 / 0x11 对比度 / 0x12 饱和度 / 0x13 色调
    int32_t setImageProperty(uint8_t type, uint8_t value);
    // 图像风格：0 标准 / 1 华丽 / 2 温和
    int32_t setImageStyle(uint8_t style);
    int32_t getStyle(uint8_t &style);
    // 图像翻转，mode 见 FLIP_*
    int32_t setImageFlip(uint8_t mode);
    int32_t setImageFlipMode(uint8_t mode);
    int32_t getImageFlip(uint8_t &flip_mode);
    int32_t getPictureFlipMode(uint8_t &mode);
    int32_t getLRReverseMode(uint8_t &mode);
    // 图像冻结
    int32_t setVideoFreeze(bool freeze);
    // 降噪：mode 0=2D(level 0~100) / 1=3D(level 0~100)
    int32_t setNoiseReduction(uint8_t mode, uint8_t level);
    int32_t getNR2D(uint8_t &level);
    int32_t getNR3D(uint8_t &level);
    int32_t setAINR(uint8_t mode, uint8_t level);
    // 伪彩色（热成像）
    int32_t setPseudoColor(uint8_t color);

    // === 白平衡与曝光 ===
    // 白平衡模式见 WhiteBalanceMode：0 自动 / 1 室内 / 2 室外 / 3 ATW / 4 手动 ...
    int32_t setWhiteBalanceMode(uint8_t mode);
    int32_t getWhiteBalance(WhiteBalanceMode &mode, uint8_t &r_gain, uint8_t &b_gain);
    // 手动白平衡增益（世辉只有 R/B 两路，g 参数被忽略）
    int32_t setRGBGain(uint8_t r, uint8_t g, uint8_t b);
    int32_t setRBgain(uint8_t rGain, uint8_t bGain);
    // 曝光模式（业务约定）：0x00 自动 / 0x01 手动 / 0x02 光圈优先 / 0x03 快门优先
    int32_t setExposureMode(uint8_t mode);
    int32_t getExposureMode(uint8_t &mode);
    int32_t getAEMode(uint8_t &mode);
    // 曝光参数（模式 + 光圈/快门/增益上下限）
    int32_t setExposure(uint8_t mode, uint8_t apertureMin, uint8_t apertureMax,
                        uint32_t shutterMin, uint32_t shutterMax, uint8_t gainMin, uint8_t gainMax);
    // 曝光控制：type 0~15 依次为 光圈±/复位/直接、增益±/复位/直接、快门±/复位/直接、补偿±/复位/直接
    int32_t setExposureControl(uint8_t type, uint8_t value);
    int32_t setAperture(uint8_t aperture);
    int32_t setShutterSpeed(uint8_t speed);
    int32_t setGain(uint8_t gain);
    int32_t getShutterPosition(uint8_t &value);
    int32_t getGainPosition(uint8_t &position);
    int32_t getExposureCompensationPosition(uint8_t &position);
    // 防闪烁：0 户外(不防闪) / 1 50Hz / 2 60Hz
    int32_t setAntiFlickerMode(uint8_t mode);

    // === 宽动态 / 透雾 / 防抖 / 强光抑制 ===
    // WDR：0 关 / 1 开
    int32_t setWDR(uint8_t mode);
    int32_t setWDRLevel(uint8_t mode, uint8_t level);
    int32_t getWDRMode(uint8_t &mode);
    int32_t getWD(uint8_t &level);
    // 透雾：mode 0 自动 / 1 手动 / 2 关闭；level 0~100
    int32_t setDefog(uint8_t mode, uint8_t level);
    int32_t getElectronicFogMode(uint8_t &mode);
    int32_t getFogPenetrationLevel(uint8_t &level);
    // 防抖：0 关 / 1 电子防抖（世辉仅这两档）
    int32_t setEIS(uint8_t mode);
    int32_t getStabilizerMode(uint8_t &mode);
    int32_t getImageStabilizer(uint8_t &mode);
    // 强光抑制
    int32_t setHLC(uint8_t mode);
    int32_t getHighlightCompensationMode(uint8_t &mode);
    int32_t getHighlightCompensationStrength(uint8_t &strength);
    // 热浪减少：mode 0 关 / 1 自动 / 2 手动；level 0 低 / 1 中 / 2 高
    int32_t setHeatHaze(uint8_t mode, uint8_t level);
    int32_t getHeatHaze(uint8_t &mode, uint8_t &level);

    // === 日夜（ICR）===
    int32_t setICR(uint8_t mode);     // 0 关 / 其它 开
    int32_t getICRMode(uint8_t &mode);
    int32_t getICREnableMode(uint8_t &mode);
    int32_t setAutoICR(uint8_t mode); // 0x02 开 / 0x03 关
    int32_t getAutoICRMode(uint8_t &mode);
    int32_t setAutoICRThreshold(uint8_t threshold);
    int32_t getAutoICRThreshold(uint8_t &threshold);
    // 日夜切换延迟：type 0x22 白天→夜晚 / 0x23 夜晚→白天；delay 单位秒
    int32_t setDayNightSwitchDelay(uint8_t type, uint8_t delay);
    int32_t getDayNightSwitchDelay(uint8_t type, uint8_t &delay);
    int32_t getIrcGain(uint8_t &value);

    // === OSD ===
    // 网络 OSD 时间标题开关（对应原 CameraNode 的 setOSD）
    int32_t setOSD(uint8_t mode);
    int32_t setOSDDisplay(uint8_t channel, uint8_t region, uint8_t function, bool enable);
    int32_t setCustomOSD(const int8_t *text);
    int32_t setThermalGPSOSD(bool enable);
    int32_t setZoomOSD(bool enable);
    int32_t setThermalTimeTitleOSD(bool enable);

    // === 视频格式 / 分辨率 ===
    int32_t setVideoFormat(uint8_t type);
    int32_t setVideoFormat(uint8_t visibleFormat, uint8_t thermalFormatHigh, uint8_t thermalFormatLow);
    int32_t getVisibleLightVideoFormat(uint8_t &format);
    int32_t getThermalVideoFormat(uint8_t &format);
    int32_t getMipiFormat(uint8_t &formatHigh, uint8_t &formatLow);
    int32_t setFormat(uint8_t format);   // 0 LVDS/BT1120 / 1 MIPI
    int32_t getFormat(uint8_t &format);
    int32_t setHorizontalResolution(uint32_t resolution);
    int32_t setVerticalResolution(uint32_t resolution);
    int32_t getHorizontalResolution(uint16_t &resolution);
    int32_t getVerticalResolution(uint16_t &resolution);
    int32_t getMipiAcquisition(uint8_t &mode);

    // === 抓拍 / 录像 ===
    int32_t setSnap(uint8_t count, uint8_t delay);
    int32_t setSnapOnePush();
    int32_t setRecord(bool start);
    int32_t getSlowShutterMode(uint8_t &mode);
    int32_t getDZoomMode(uint8_t &mode);

    // === 系统 ===
    int32_t setInitialize();
    int32_t setImageInitialize();
    int32_t setPowerOff();
    int32_t setBaudRate(uint8_t rate);   // 0 9600 / 1 19200 / 2 38400 / 3 115200

    // === 查询 ===
    int32_t getCoreTemperature(float &temperature);
    int32_t getProgramVersion(uint16_t &year, uint8_t &month, uint8_t &day, uint8_t &serialNumber);
    int32_t getVersionInfo(uint16_t &vendorId, uint16_t &modelId, uint8_t &majorVersion,
                           uint8_t &minorVersion, uint8_t &fixVersion);
    int32_t getLensZoomInfo(uint32_t &zoomTimes, uint32_t &zoomDirectionTimes);
    int32_t getLensFocusInfo(uint32_t &focusTimes, uint32_t &focusDirectionTimes);
    int32_t getLensIrisInfo(uint32_t &irisTimes, uint32_t &icrTimes);
    int32_t getRegisterValue(uint16_t regNum, uint8_t &value);
    int32_t getLockMode(uint8_t &mode);

    // === 兼容原 CameraNode 的参数通道 ===
    // 世辉：subCommand 0 → 仅光学变倍；其它 → 数字+光学变倍（与原实现一致）
    int32_t setAdcGainParam(uint8_t subCommand, uint16_t data);

private:
    std::unique_ptr<ShihuiCore> shihui_camera_;
};
