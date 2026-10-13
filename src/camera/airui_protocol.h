#pragma once

#include<array>
#include<cstddef>
#include<cstdint>
#include<functional>
#include<memory>
#include<optional>
#include<string>
#include<vector>

// F384/F640 用户操作指令说明书 V1.0.2（20251107）。
// UART 默认 /dev/ttyCH9344USB0，115200 / 8N1。多字节数值为小端；长度包含命令体和校验字节。
namespace airui
{
using Bytes = std::vector<std::uint8_t>;
inline constexpr char kDefaultPort[] = "/dev/ttyCH9344USB0";
constexpr std::uint32_t kDefaultBaud = 115200;
constexpr std::uint8_t kCommandHead = 0xAA;
constexpr std::uint8_t kResponseHead = 0x55;
constexpr std::uint8_t kResponseOperation = 0x33;
constexpr std::size_t kMaxFrameBytes = 259;

enum class CommandSet : std::uint8_t { Common = 0x01, ImageMode = 0x02, Temperature = 0x07, Lens = 0x08 };
enum class Operation : std::uint8_t { Read = 0x00, Write = 0x01, Action = 0x02 };
enum class DeviceType { Imaging, Radiometric };

// 高字节是命令字 0，低字节是命令字 1；普通应答不回显命令字 0。
enum class Command : std::uint16_t
{
    AutoShutter = 0x0101, Correction = 0x0102, ShutterTimeInterval = 0x0103,
    ShutterFpaInterval = 0x0104, StrongLightProtection = 0x0108, ShutterCoreInterval = 0x010D,
    FieldOfView = 0x0131, Brightness = 0x0136, Contrast = 0x0137, Detail = 0x0138,
    SpatialFilter = 0x0139, TemporalFilter = 0x013A, AnalogVideo = 0x013D, Freeze = 0x013E,
    DigitalZoom = 0x0140, Palette = 0x0142, BlindPixelCursor = 0x0143, BlindPixelMove = 0x0144,
    BootLogo = 0x0149, WarningThreshold = 0x014B, ImageFlip = 0x014C,
    VideoSource = 0x015C, DigitalVideo = 0x015D, SerialNumber = 0x0171,
    SensorWidth = 0x0172, SensorHeight = 0x0173, CoreTemperature = 0x017C,
    SaveSettings = 0x017F, FactoryReset = 0x0182, BlindPixelEdit = 0x0190,
    SaveBlindPixels = 0x0191, LensKCalibration = 0x01A0, ImageCompensation = 0x01A1,
    Synchronization = 0x01A3, FpaTemperature = 0x01C3, ImageMode = 0x021A,
    TemperatureDisplay = 0x0700, TemperatureRange = 0x0701, TemperatureUnit = 0x0702,
    ReflectedTemperature = 0x070F, AtmosphericTemperature = 0x0710, Humidity = 0x0711,
    Emissivity = 0x0712, TargetDistance = 0x0713, ApplyEnvironment = 0x0718,
    StretchLow = 0x071D, StretchHigh = 0x071E, PointTemperature = 0x071F, Isotherm = 0x0720,
    CenterTemperatureDisplay = 0x072B, CenterTemperature = 0x072C, TemperatureAlarm = 0x072D,
    AlarmLow = 0x072E, AlarmHigh = 0x072F, FireAlarm = 0x0730, FireThreshold = 0x0731,
    LensTemperatureCorrection = 0x0760, LensCorrectionData = 0x076A,
    EraseLensCorrection = 0x076B, CalibrateLensTemperature = 0x076F, TemperatureStretch = 0x07F0,
    ManualAlarm = 0x0800, LensType = 0x0803, AutofocusAfterZoom = 0x0804,
    FocusTemperatureInterval = 0x0808, FocusDrive = 0x0821, FocusStop = 0x0822,
    FocusPosition = 0x0823, FocusSpeed = 0x0824, FocusTravel = 0x0825, Autofocus = 0x082F,
    ZoomDrive = 0x0831, ZoomStop = 0x0832, ZoomPosition = 0x0833, ZoomSpeed = 0x0834,
    ZoomTravel = 0x0835, Preset = 0x0883, CallPreset = 0x0887, FocalLength = 0x088B,
    FocalLengthDisplay = 0x088D, SetFocalLength = 0x088E
};

enum class Palette : std::uint8_t
{
    WhiteHot, BlackHot, Rainbow, HighContrastRainbow, IronRed, Lava, Sky, MiddleGray,
    GrayRed, PurpleOrange, Special, WarningRed, IceFire, CyanRed, Special2,
    GradientRed, GradientGreen, GradientBlue, WarningGreen, WarningBlue
};
enum class ImageFlip : std::uint8_t { None = 0x01, Horizontal = 0x02, Vertical = 0x04, Diagonal = 0x08 };
enum class WarningColor : std::uint8_t { Red, Green, Blue };
enum class DigitalVideo : std::uint16_t { Off = 0x0000, LvCmos = 0x0002, Bt1120 = 0x0005, Bt601 = 0x2005, Cds2 = 0x8005, Cds3 = 0x4005 };
enum class VideoSource : std::uint8_t { Original = 0x00, Nuc = 0x01, Drc = 0x02, Dns = 0x05 };
enum class ImageMode : std::uint8_t { Classic, SeaSky, Forest, Reserved3, Reserved4 };
enum class LensType : std::uint8_t { NoneOrFixed, ContinuousZoom, MotorizedFixed, DualFieldOfView };
enum class FocusDirection : std::uint8_t { Near = 0x01, Far = 0x02 };
enum class ZoomDirection : std::uint8_t { Wide = 0x01, Tele = 0x02 };
enum class Adjustment : std::uint8_t { Coarse, Fine };
enum class Motor : std::uint8_t { Zoom, Focus };
enum class TemperatureRange : std::uint8_t { HighGain = 0x00, LowGain = 0x01, Auto = 0x03 };
enum class TemperatureUnit : std::uint8_t { Celsius, Kelvin, Fahrenheit };
enum class TemperatureAlarm : std::uint8_t { Off, Low, High, LowAndHigh };
enum class Axis : std::uint8_t { Horizontal, Vertical };
enum class CursorMove : std::uint8_t { Up1 = 1, Down1, Left1, Right1, Up20 = 0x81, Down20, Left20, Right20 };
enum class BlindPixelEdit : std::uint8_t { Add = 1, Undo = 2 };
enum class LensKAction : std::uint8_t { CaptureLow = 0x0A, CaptureHigh, Calculate, Save, Clear };
enum class CompensationAction : std::uint8_t { CaptureAndCalculate, Save, Clear };
enum class SyncMode : std::uint8_t { Self, Internal, External, Adaptive };
enum class DeviceError : std::uint8_t { CommandTimeout = 0xF1, MissingCommand = 0xFB, Checksum = 0xFD, Header = 0xFF };

struct Point { std::uint16_t x{0}, y{0}; };
struct Rectangle { std::uint16_t left{0}, top{0}, right{0}, bottom{0}; }; // 右下角含边界
struct MotorTravel { std::uint16_t minimum{0}, maximum{0}; };
struct CenterTemperature { double temperature{0}; Point position; };
struct StrongLightProtection { bool enabled{false}; std::uint16_t threshold{0}; std::uint8_t seconds{0}; };
struct Synchronization { SyncMode mode{SyncMode::Self}; std::uint8_t frequencyHz{50}; };

struct CommandInfo
{
    Command command;
    const char* name;
    int readBytes, writeBytes, actionBytes; // -1 表示该操作未在文档中定义
    std::size_t responseBytes;             // 读取数据长度；写/执行成功应答均为 01
    bool radiometricOnly;
    unsigned manualPage;                  // 文档正文页码（不含封面/目录）
};
const std::vector<CommandInfo>& commandCatalog();
const CommandInfo* commandInfo(Command command);

std::uint8_t checksum(const std::uint8_t* data, std::size_t size);
// 校验命令/操作/参数长度；不合法时抛 invalid_argument。
Bytes encodeCommand(Command command, Operation operation, const Bytes& parameters = {});
Bytes littleEndian16(std::uint16_t value);
Bytes littleEndian32(std::uint32_t value);
std::uint16_t readLittleEndian16(const std::uint8_t* data);
std::uint32_t readLittleEndian32(const std::uint8_t* data);

struct Frame
{
    bool response{false};
    std::optional<CommandSet> commandSet; // 普通 55 应答省略命令字 0，故为 nullopt
    std::uint8_t command{0}, operation{0};
    Bytes parameters;
    Bytes wire;
};
bool decodeFrame(const std::uint8_t* data, std::size_t size, Frame& out);

class FrameParser
{
public:
    std::vector<Frame> feed(const std::uint8_t* data, std::size_t size);
    void reset() { buffer_.clear(); }
private:
    Bytes buffer_;
};

enum class AlarmType : std::uint8_t { StrongLight = 0x01, Fire = 0x03 };
struct AlarmReport
{
    AlarmType type{AlarmType::Fire};
    bool shutterClosed{false};
    std::array<Point, 5> fireCenters{}; // 文档以 (0,0) 表示未使用区域，未定义区域数
};
bool decodeAlarmReport(const Frame& frame, AlarmReport& out);

enum class Error
{
    None, NotConnected, InvalidArgument, UnsupportedDevice, WriteFailed, Timeout,
    ConnectionLost, DeviceRejected, ProtocolError, MalformedResponse, NeedsReconnect
};
struct Result
{
    Error error{Error::None};
    std::optional<DeviceError> deviceError;
    Frame frame;
    explicit operator bool() const { return error == Error::None; }
};
const char* errorString(Error error);
} // namespace airui

// 与 ShihuiCore 一致的同步控制接口，复用 Urt 的接收线程。
// 命令在内部串行执行；connect/disconnect 由同一控制线程负责，disconnect 可唤醒等待者。
// 回调在接收线程运行，不能在回调中同步发命令、断开或析构本对象，应投递到控制线程。
class AiruiCore
{
public:
    explicit AiruiCore(std::string port = airui::kDefaultPort, std::uint32_t baud = airui::kDefaultBaud,
                       airui::DeviceType type = airui::DeviceType::Imaging);
    ~AiruiCore();
    AiruiCore(const AiruiCore&) = delete;
    AiruiCore& operator=(const AiruiCore&) = delete;

    bool connect();
    void disconnect();
    bool reconnect();
    bool isConnected() const;
    bool setResponseTimeout(int milliseconds); // 默认 500ms，标定等耗时操作可调大
    airui::Result lastResult() const;
    void setFrameCallback(std::function<void(const airui::Frame&)> callback);
    void setAlarmCallback(std::function<void(const airui::AlarmReport&)> callback);
    // 低层接口保留原始值，便于设备差异核对；同样执行能力、长度、应答及超时检查。
    airui::Result transact(airui::Command command, airui::Operation operation,
                           const airui::Bytes& parameters = {}, int timeoutMs = -1);

    bool getSerialNumber(std::string& value);
    bool getSensorWidth(std::uint16_t& value);
    bool getSensorHeight(std::uint16_t& value);
    bool backgroundCorrection();
    bool shutterCorrection();
    bool setAutoShutter(bool automatic);
    bool setShutterTimeInterval(std::uint8_t minutes);
    bool getShutterTimeInterval(std::uint8_t& minutes);
    bool setShutterFpaInterval(double celsius);
    bool getShutterFpaInterval(double& celsius);
    bool setShutterCoreInterval(double celsius);
    bool getShutterCoreInterval(double& celsius);
    bool getCoreTemperature(double& celsius);
    bool getFpaTemperature(double& celsius);
    bool saveSettings();
    bool factoryReset();

    bool setDigitalZoom(const airui::Rectangle& region);
    bool setDigitalZoom(double ratio, std::uint16_t width, std::uint16_t height); // 1.0~8.0，0.1 步进
    bool setImageFlip(airui::ImageFlip mode);
    bool setAnalogVideo(bool enabled);
    bool freezeImage(bool freeze);
    bool setBootLogo(bool enabled);
    bool setPalette(airui::Palette palette);
    bool getPalette(airui::Palette& palette);
    bool setWarningThreshold(airui::WarningColor color, std::uint8_t threshold);
    bool setDigitalVideo(airui::DigitalVideo interface);
    bool setVideoSource(airui::VideoSource parallel, airui::VideoSource serial = airui::VideoSource::Original);
    bool setImageMode(airui::ImageMode mode);
    bool getImageMode(airui::ImageMode& mode);
    bool setBrightness(std::uint8_t value);
    bool getBrightness(std::uint8_t& value);
    bool setContrast(std::uint8_t value);
    bool getContrast(std::uint8_t& value);
    bool setDetail(std::uint8_t value);
    bool getDetail(std::uint8_t& value);
    bool setSpatialFilter(std::uint8_t value);
    bool getSpatialFilter(std::uint8_t& value);
    bool setTemporalFilter(std::uint8_t value);
    bool getTemporalFilter(std::uint8_t& value);

    bool setLensType(airui::LensType type);
    bool getLensType(airui::LensType& type);
    bool setManualAlarm(bool enabled);
    bool setAutofocusAfterZoom(bool enabled);
    bool setFocusTemperatureInterval(double celsius);
    bool getFocusTemperatureInterval(double& celsius);
    bool autofocus();
    bool driveFocus(airui::FocusDirection direction, airui::Adjustment adjustment = airui::Adjustment::Coarse);
    bool stopFocus();
    bool getFocusPosition(std::uint16_t& position);
    bool setFocusSpeed(std::uint8_t speed); // 0~32
    bool getFocusSpeed(std::uint8_t& speed);
    bool getFocusTravel(airui::MotorTravel& travel);
    bool driveZoom(airui::ZoomDirection direction, airui::Adjustment adjustment = airui::Adjustment::Coarse);
    bool stopZoom();
    bool getZoomPosition(std::uint16_t& position);
    bool setZoomSpeed(std::uint8_t speed);
    bool getZoomSpeed(std::uint8_t& speed);
    bool getZoomTravel(airui::MotorTravel& travel);
    bool writePreset(airui::Motor motor, std::uint8_t index); // 0~9
    bool readPreset(airui::Motor motor, std::uint8_t index, std::uint16_t& position);
    bool callPreset(airui::Motor motor, std::uint8_t index);
    bool getFocalLength(double& millimeters);
    bool setFocalLength(double millimeters);
    bool setFocalLengthDisplay(bool enabled);
    bool getFieldOfView(airui::Axis axis, float& degrees); // IEEE-754 float32 小端

    // 以下测温接口仅允许 DeviceType::Radiometric；火点报警、强光保护两类均可使用。
    // 温度使用当前设备单位；环境参数设置后需显式 applyEnvironment()。
    bool setTemperatureRange(airui::TemperatureRange range);
    bool setTemperatureDisplay(bool enabled);
    bool getTemperatureUnit(airui::TemperatureUnit& unit);
    bool setTemperatureUnit(airui::TemperatureUnit unit);
    bool getReflectedTemperature(double& value);
    bool setReflectedTemperature(double value);
    bool getAtmosphericTemperature(double& value);
    bool setAtmosphericTemperature(double value);
    bool getEmissivity(double& value); // 0~1
    bool setEmissivity(double value);
    bool getTargetDistance(double& meters);
    bool setTargetDistance(double meters);
    bool getHumidity(double& value); // 比例 0~1，不是百分数 0~100
    bool setHumidity(double value);
    bool applyEnvironment();
    bool getPointTemperature(airui::Point point, double& value);
    bool setCenterTemperatureDisplay(bool enabled);
    bool getCenterTemperature(airui::CenterTemperature& value);
    bool setFireAlarm(bool enabled);
    bool setFireThreshold(std::uint16_t threshold); // 1~16383，原始灰度阈值
    bool getFireThreshold(std::uint16_t& threshold);
    bool setTemperatureStretch(bool enabled);
    bool setStretchLow(double value);
    bool getStretchLow(double& value);
    bool setStretchHigh(double value);
    bool getStretchHigh(double& value);
    bool setTemperatureAlarm(airui::TemperatureAlarm mode);
    bool getTemperatureAlarm(airui::TemperatureAlarm& mode);
    bool setIsotherm(bool enabled);
    bool getIsotherm(bool& enabled);
    bool setAlarmLow(double value);
    bool getAlarmLow(double& value);
    bool setAlarmHigh(double value);
    bool getAlarmHigh(double& value);
    bool setLensTemperatureCorrection(bool enabled);
    bool hasLensCorrectionData(bool& saved);
    bool eraseLensCorrection();
    bool calibrateLensTemperature(std::int16_t targetTemperature); // 整数温度，不乘 10/10000
    bool saveLensCorrection();
    bool getStrongLightProtection(airui::StrongLightProtection& value);
    bool setStrongLightProtection(const airui::StrongLightProtection& value);

    bool showBlindPixelCursor(bool show);
    bool moveBlindPixelCursor(airui::CursorMove move);
    bool editBlindPixel(airui::BlindPixelEdit action);
    bool saveBlindPixels();
    bool calibrateLensK(airui::LensKAction action);
    bool compensateImage(airui::CompensationAction action);
    bool getSynchronization(airui::Synchronization& value);
    bool setSynchronization(const airui::Synchronization& value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool invalidArgument();
    bool malformedResponse();
    bool write(airui::Command command, const airui::Bytes& parameters,
               airui::Operation operation = airui::Operation::Write);
    bool read(airui::Command command, airui::Bytes& value, const airui::Bytes& parameters = {});
    bool readByte(airui::Command command, std::uint8_t& value, const airui::Bytes& parameters = {});
    bool readWord(airui::Command command, std::uint16_t& value, const airui::Bytes& parameters = {});
    bool readScaled(airui::Command command, double& value, double scale, unsigned bytes,
                    bool signedValue, const airui::Bytes& parameters = {});
    bool writeScaled(airui::Command command, double value, double scale, unsigned bytes,
                     bool signedValue = false);
};
