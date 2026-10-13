#pragma once

#include"camera/airui_protocol.h"

#include<cstdint>
#include<functional>
#include<memory>
#include<mutex>
#include<string>
#include<utility>

// 热成像相机业务控制层：只负责串口控制，不负责视频采集/编码/推流。
// 控制/查询返回 1=成功、0=失败，与 RGBCamera 一致；物理量单位沿用 AiruiCore。
// 命令在本层串行化，多步操作不会与其它本层命令交错；失败时停止，不回滚已执行的指令。
// 生命周期由同一控制线程管理，disconnect 可取消等待；析构前停止其它线程访问。
// 回调在接收线程执行，只应复制并投递事件，不能同步调用控制/生命周期接口。
class ThermalCamera
{
public:
    struct Config
    {
        std::string port{airui::kDefaultPort};
        std::uint32_t baud{airui::kDefaultBaud};
        airui::DeviceType type{airui::DeviceType::Imaging};
        int responseTimeoutMs{500};
    };
    struct SensorSize { std::uint16_t width{0}, height{0}; };
    struct Environment
    {
        double reflectedTemperature{25}, atmosphericTemperature{25};
        double emissivity{0.95}, targetDistanceMeters{1}, humidity{0.5};
    };
    using ReportCallback = std::function<void(const airui::AlarmReport&)>;
    using FrameCallback = std::function<void(const airui::Frame&)>;

    // 与可见光业务方向一致；Airui 的原始变焦方向编码与此不同，内部转换。
    enum : std::uint8_t { ZOOM_DIR_STOP=0, ZOOM_DIR_IN=1, ZOOM_DIR_OUT=2 };
    enum : std::uint8_t { FOCUS_DIR_STOP=0, FOCUS_DIR_NEAR=1, FOCUS_DIR_FAR=2 };
    enum : std::uint8_t { FLIP_NONE=0, FLIP_HORIZONTAL=1, FLIP_VERTICAL=2, FLIP_BOTH=3 };

    explicit ThermalCamera(const std::string& port=airui::kDefaultPort,
                           std::uint32_t baud=airui::kDefaultBaud,
                           airui::DeviceType type=airui::DeviceType::Imaging);
    explicit ThermalCamera(const Config& config); // 非法配置抛 std::invalid_argument
    ~ThermalCamera();
    ThermalCamera(const ThermalCamera&)=delete;
    ThermalCamera& operator=(const ThermalCamera&)=delete;

    bool connect();
    void disconnect();
    bool reconnect();
    bool isConnected() const;
    // 重新创建内核并断开旧串口，保留回调；init 本身不打开串口。
    // 两参数版本保留原机芯类型和应答超时；非法配置保留旧连接。
    bool init(const Config& config);
    bool init(const std::string& port, std::uint32_t baud=airui::kDefaultBaud);
    Config config() const;
    bool setResponseTimeout(int milliseconds);
    airui::Result lastResult() const;
    void setReportCallback(ReportCallback callback);
    void setFrameCallback(FrameCallback callback);
    // 维护/特殊命令入口；绕过本层串行化和 lastResult 记录。
    // 不与本层操作或 init 并发；init 后旧指针失效。
    AiruiCore* core() { return camera_.get(); }
    const AiruiCore* core() const { return camera_.get(); }

    // speed=0 保留设备当前速度，1~100 映射到协议 1~32；停止时忽略速度及微调参数。
    std::int32_t continuousZoom(std::uint8_t direction, std::uint8_t speed=0,
                               airui::Adjustment adjustment=airui::Adjustment::Coarse);
    std::int32_t continuousFocus(std::uint8_t direction, std::uint8_t speed=0,
                                airui::Adjustment adjustment=airui::Adjustment::Coarse);
    std::int32_t zoomIn(std::uint8_t speed=0) { return continuousZoom(ZOOM_DIR_IN,speed); }
    std::int32_t zoomOut(std::uint8_t speed=0) { return continuousZoom(ZOOM_DIR_OUT,speed); }
    std::int32_t zoomStop() { return continuousZoom(ZOOM_DIR_STOP); }
    std::int32_t focusNear(std::uint8_t speed=0) { return continuousFocus(FOCUS_DIR_NEAR,speed); }
    std::int32_t focusFar(std::uint8_t speed=0) { return continuousFocus(FOCUS_DIR_FAR,speed); }
    std::int32_t focusStop() { return continuousFocus(FOCUS_DIR_STOP); }
    std::int32_t setPseudoColor(std::uint8_t color); // 0~19
    std::int32_t getPseudoColor(std::uint8_t& color);
    std::int32_t setNoiseReduction(std::uint8_t mode, std::uint8_t level); // 0=空域、1=时域，0~100
    std::int32_t setVideoFreeze(bool freeze);
    std::int32_t getSensorSize(SensorSize& size); // 宽高均查询成功后更新输出，不假定 F384/F640
    std::int32_t getEnvironment(Environment& value);
    std::int32_t setEnvironment(const Environment& value); // 先检查全部参数，再逐项写入并使其生效

    std::int32_t getSerialNumber(std::string& value);
    std::int32_t getSensorWidth(std::uint16_t& value);
    std::int32_t getSensorHeight(std::uint16_t& value);
    std::int32_t backgroundCorrection();
    std::int32_t shutterCorrection();
    std::int32_t setAutoShutter(bool automatic);
    std::int32_t setShutterTimeInterval(std::uint8_t minutes);
    std::int32_t getShutterTimeInterval(std::uint8_t& minutes);
    std::int32_t setShutterFpaInterval(double celsius);
    std::int32_t getShutterFpaInterval(double& celsius);
    std::int32_t setShutterCoreInterval(double celsius);
    std::int32_t getShutterCoreInterval(double& celsius);
    std::int32_t getCoreTemperature(double& celsius);
    std::int32_t getFpaTemperature(double& celsius);
    std::int32_t saveSettings();
    std::int32_t factoryReset();

    std::int32_t setDigitalZoom(const airui::Rectangle& region);
    std::int32_t setDigitalZoom(double ratio); // 1.0~8.0，0.1 步进，自动查询画幅
    std::int32_t setImageFlip(std::uint8_t mode);
    std::int32_t setAnalogVideo(bool enabled);
    std::int32_t freezeImage(bool freeze);
    std::int32_t setBootLogo(bool enabled);
    std::int32_t setPalette(airui::Palette palette);
    std::int32_t getPalette(airui::Palette& palette);
    std::int32_t setWarningThreshold(airui::WarningColor color, std::uint8_t threshold);
    std::int32_t setDigitalVideo(airui::DigitalVideo interface);
    std::int32_t setVideoSource(airui::VideoSource parallel, airui::VideoSource serial = airui::VideoSource::Original);
    std::int32_t setImageMode(airui::ImageMode mode);
    std::int32_t getImageMode(airui::ImageMode& mode);
    std::int32_t setBrightness(std::uint8_t value);
    std::int32_t getBrightness(std::uint8_t& value);
    std::int32_t setContrast(std::uint8_t value);
    std::int32_t getContrast(std::uint8_t& value);
    std::int32_t setDetail(std::uint8_t value);
    std::int32_t getDetail(std::uint8_t& value);
    std::int32_t setSpatialFilter(std::uint8_t value);
    std::int32_t getSpatialFilter(std::uint8_t& value);
    std::int32_t setTemporalFilter(std::uint8_t value);
    std::int32_t getTemporalFilter(std::uint8_t& value);

    std::int32_t setLensType(airui::LensType type);
    std::int32_t getLensType(airui::LensType& type);
    std::int32_t setManualAlarm(bool enabled);
    std::int32_t setAutofocusAfterZoom(bool enabled);
    std::int32_t setFocusTemperatureInterval(double celsius);
    std::int32_t getFocusTemperatureInterval(double& celsius);
    std::int32_t autoFocus();
    std::int32_t driveFocus(airui::FocusDirection direction, airui::Adjustment adjustment = airui::Adjustment::Coarse);
    std::int32_t stopFocus();
    std::int32_t getFocusPosition(std::uint16_t& position);
    std::int32_t setFocusSpeed(std::uint8_t speed); // 0~32
    std::int32_t getFocusSpeed(std::uint8_t& speed);
    std::int32_t getFocusTravel(airui::MotorTravel& travel);
    std::int32_t driveZoom(airui::ZoomDirection direction, airui::Adjustment adjustment = airui::Adjustment::Coarse);
    std::int32_t stopZoom();
    std::int32_t getZoomPosition(std::uint16_t& position);
    std::int32_t setZoomSpeed(std::uint8_t speed);
    std::int32_t getZoomSpeed(std::uint8_t& speed);
    std::int32_t getZoomTravel(airui::MotorTravel& travel);
    std::int32_t writePreset(airui::Motor motor, std::uint8_t index); // 0~9
    std::int32_t readPreset(airui::Motor motor, std::uint8_t index, std::uint16_t& position);
    std::int32_t callPreset(airui::Motor motor, std::uint8_t index);
    std::int32_t getFocalLength(double& millimeters);
    std::int32_t setFocalLength(double millimeters);
    std::int32_t setFocalLengthDisplay(bool enabled);
    std::int32_t getFieldOfView(airui::Axis axis, float& degrees); // IEEE-754 float32 小端

    // 以下测温接口仅允许 DeviceType::Radiometric；火点报警、强光保护两类均可使用。
    // 温度使用当前设备单位；单独设置环境字段后需显式 applyEnvironment()。
    std::int32_t setTemperatureRange(airui::TemperatureRange range);
    std::int32_t setTemperatureDisplay(bool enabled);
    std::int32_t getTemperatureUnit(airui::TemperatureUnit& unit);
    std::int32_t setTemperatureUnit(airui::TemperatureUnit unit);
    std::int32_t getReflectedTemperature(double& value);
    std::int32_t setReflectedTemperature(double value);
    std::int32_t getAtmosphericTemperature(double& value);
    std::int32_t setAtmosphericTemperature(double value);
    std::int32_t getEmissivity(double& value); // 0~1
    std::int32_t setEmissivity(double value);
    std::int32_t getTargetDistance(double& meters);
    std::int32_t setTargetDistance(double meters);
    std::int32_t getHumidity(double& value); // 比例 0~1，不是百分数 0~100
    std::int32_t setHumidity(double value);
    std::int32_t applyEnvironment();
    std::int32_t getPointTemperature(airui::Point point, double& value);
    std::int32_t setCenterTemperatureDisplay(bool enabled);
    std::int32_t getCenterTemperature(airui::CenterTemperature& value);
    std::int32_t setFireAlarm(bool enabled);
    std::int32_t setFireThreshold(std::uint16_t threshold); // 1~16383，原始灰度阈值
    std::int32_t getFireThreshold(std::uint16_t& threshold);
    std::int32_t setTemperatureStretch(bool enabled);
    std::int32_t setStretchLow(double value);
    std::int32_t getStretchLow(double& value);
    std::int32_t setStretchHigh(double value);
    std::int32_t getStretchHigh(double& value);
    std::int32_t setTemperatureAlarm(airui::TemperatureAlarm mode);
    std::int32_t getTemperatureAlarm(airui::TemperatureAlarm& mode);
    std::int32_t setIsotherm(bool enabled);
    std::int32_t getIsotherm(bool& enabled);
    std::int32_t setAlarmLow(double value);
    std::int32_t getAlarmLow(double& value);
    std::int32_t setAlarmHigh(double value);
    std::int32_t getAlarmHigh(double& value);
    std::int32_t setLensTemperatureCorrection(bool enabled);
    std::int32_t hasLensCorrectionData(bool& saved);
    std::int32_t eraseLensCorrection();
    std::int32_t calibrateLensTemperature(std::int16_t targetTemperature); // 整数温度，不乘 10/10000
    std::int32_t saveLensCorrection();
    std::int32_t getStrongLightProtection(airui::StrongLightProtection& value);
    std::int32_t setStrongLightProtection(const airui::StrongLightProtection& value);

    std::int32_t showBlindPixelCursor(bool show);
    std::int32_t moveBlindPixelCursor(airui::CursorMove move);
    std::int32_t editBlindPixel(airui::BlindPixelEdit action);
    std::int32_t saveBlindPixels();
    std::int32_t calibrateLensK(airui::LensKAction action);
    std::int32_t compensateImage(airui::CompensationAction action);
    std::int32_t getSynchronization(airui::Synchronization& value);
    std::int32_t setSynchronization(const airui::Synchronization& value);

private:
    template<class F> std::int32_t call(F&& operation)
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        return completeLocked(operation(*camera_));
    }
    std::int32_t completeLocked(bool success);
    std::int32_t failLocked(airui::Error error);
    bool querySensorSizeLocked(SensorSize& size);

    Config config_;
    std::unique_ptr<AiruiCore> camera_;
    mutable std::mutex commandMutex_;
    airui::Result last_;
    ReportCallback reportCallback_;
    FrameCallback frameCallback_;
};
