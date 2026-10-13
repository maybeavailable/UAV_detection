#pragma once

#include "camera/mipi_ctl.h"
#include "camera/thermal_camera.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

// 艾睿 F640：MIPI YUV422 → VI → VPSS chn0 → 640×512 NV21。
// 本类负责采集和串口控制；编码、检测、推流由帧的使用方负责。
// 媒体运行时和全局 MIPI lane 分组由应用在启动各相机前统一初始化。
// init/deinit/reconnectCamera 由同一控制线程调用，不与抓帧和控制命令并发。
// 建议仅一个线程调用 getFrame/saveFrame；析构前必须停止其它线程访问。
class ThermalCameraCtl
{
public:
    static constexpr std::uint32_t kWidth = 640;
    static constexpr std::uint32_t kHeight = 512;
    static constexpr hi_vpss_chn kVpssChn = 0; // 仅创建这一条输出通道

    struct Config
    {
        // 参考 8.11/VT 的 __THERMAL_AIRUI__ 接线；反接 lane 时可显式调整。
        std::uint32_t width = kWidth;
        std::uint32_t height = kHeight;
        combo_dev_t mipiDev = 2;
        std::array<short, 2> laneIds{{4, 5}};
        hi_vi_dev viDev = 2;
        hi_vi_pipe viPipe = 3;
        hi_vi_chn viChn = 0;
        hi_vpss_grp vpssGrp = 257;
        hi_u32 pipeDepth = 3;
        hi_u32 chnDepth = 0;

        // 默认 /dev/ttyCH9344USB0、115200、8N1。
        ThermalCamera::Config camera;
        bool cameraRequired = false; // 串口离线时仍允许采集；true 则初始化失败
        int getFrameTimeoutMs = 1000;
    };
    struct Stats
    {
        std::uint64_t frames = 0;
        std::uint64_t errors = 0;
        std::uint64_t lastPtsUs = 0;
    };

    // 帧仅在回调期间有效，回调返回即归还 VPSS；异步处理前须拷贝/完成设备操作。
    // 回调中不能调用 init/deinit，也不能递归抓帧。
    using FrameHandler = std::function<void(const hi_video_frame_info&)>;

    ThermalCameraCtl() = default;
    ~ThermalCameraCtl();
    ThermalCameraCtl(const ThermalCameraCtl&) = delete;
    ThermalCameraCtl& operator=(const ThermalCameraCtl&) = delete;

    bool init() { return init(Config{}); }
    bool init(const Config& config); // 已初始化时返回 false；改配置前先 deinit
    void deinit();                  // 逆序拆除链路、关闭串口，可重复调用
    bool isInited() const { return inited_; }
    const Config& config() const { return config_; }

    // timeoutMs < 0 使用配置值，0 非阻塞；取帧、回调或还帧失败均返回 false。
    // 捕获回调异常并记录错误，异常时也会归还帧。
    bool getFrame(FrameHandler handler, int timeoutMs = -1);
    // 原始 NV21/NV12，逐行去除 stride 填充；文件写入和关闭失败返回 false。
    bool saveFrame(const std::string& path, int timeoutMs = -1);
    Stats stats() const;

    ThermalCamera* camera() { return &camera_; }
    const ThermalCamera* camera() const { return &camera_; }
    bool cameraConnected() const { return camera_.isConnected(); }
    bool reconnectCamera(); // 只重连串口，不重建采集链路
    void setReportCallback(ThermalCamera::ReportCallback callback)
        { camera_.setReportCallback(std::move(callback)); }
    void setProtocolFrameCallback(ThermalCamera::FrameCallback callback)
        { camera_.setFrameCallback(std::move(callback)); }

    // 常用串口控制返回 bool；完整接口和详细错误使用 camera()。
    bool zoomIn(std::uint8_t speed = 0)
        { return camCall("zoomIn", [this, speed] { return camera_.zoomIn(speed); }); }
    bool zoomOut(std::uint8_t speed = 0)
        { return camCall("zoomOut", [this, speed] { return camera_.zoomOut(speed); }); }
    bool zoomStop() { return camCall("zoomStop", [this] { return camera_.zoomStop(); }); }
    bool focusNear(std::uint8_t speed = 0)
        { return camCall("focusNear", [this, speed] { return camera_.focusNear(speed); }); }
    bool focusFar(std::uint8_t speed = 0)
        { return camCall("focusFar", [this, speed] { return camera_.focusFar(speed); }); }
    bool focusStop() { return camCall("focusStop", [this] { return camera_.focusStop(); }); }
    bool autoFocus() { return camCall("autoFocus", [this] { return camera_.autoFocus(); }); }
    bool setPseudoColor(std::uint8_t color)
        { return camCall("setPseudoColor", [this, color] { return camera_.setPseudoColor(color); }); }
    bool setBrightness(std::uint8_t value)
        { return camCall("setBrightness", [this, value] { return camera_.setBrightness(value); }); }
    bool setContrast(std::uint8_t value)
        { return camCall("setContrast", [this, value] { return camera_.setContrast(value); }); }
    bool setDetail(std::uint8_t value)
        { return camCall("setDetail", [this, value] { return camera_.setDetail(value); }); }
    bool setNoiseReduction(std::uint8_t mode, std::uint8_t level)
        { return camCall("setNoiseReduction", [this, mode, level] { return camera_.setNoiseReduction(mode, level); }); }
    bool setDigitalZoom(double ratio)
        { return camCall("setDigitalZoom", [this, ratio] { return camera_.setDigitalZoom(ratio); }); }
    bool setImageFlip(std::uint8_t mode)
        { return camCall("setImageFlip", [this, mode] { return camera_.setImageFlip(mode); }); }
    bool setVideoFreeze(bool freeze)
        { return camCall("setVideoFreeze", [this, freeze] { return camera_.setVideoFreeze(freeze); }); }
    bool shutterCorrection()
        { return camCall("shutterCorrection", [this] { return camera_.shutterCorrection(); }); }
    bool backgroundCorrection()
        { return camCall("backgroundCorrection", [this] { return camera_.backgroundCorrection(); }); }
    bool setAutoShutter(bool automatic)
        { return camCall("setAutoShutter", [this, automatic] { return camera_.setAutoShutter(automatic); }); }
    bool saveSettings() { return camCall("saveSettings", [this] { return camera_.saveSettings(); }); }

private:
    bool buildMipi();
    bool buildVi();
    bool buildVpss();
    bool camCall(const char* name, const std::function<std::int32_t()>& operation);

    Config config_;
    ThermalCamera camera_;
    mipi_ctl::vi_cfg viConfig_{};
    mipi_ctl::sample_venc_vpss_chn_attr vpssConfig_{};
    bool inited_ = false;
    bool mipiClockEnabled_ = false;
    bool viStarted_ = false;
    bool vpssStarted_ = false;
    bool bound_ = false;
    std::atomic<std::uint64_t> frames_{0}, errors_{0}, lastPtsUs_{0};
};
