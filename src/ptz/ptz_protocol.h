#pragma once

#include<array>
#include<cstddef>
#include<cstdint>
#include<functional>
#include<memory>
#include<string>
#include<vector>

// 依据《光电转台通信协议（改）》及《无极变速指令》。
// 协议仅处理字节，实现位于 ptz_protocol.cpp；frame.data()/frame.size() 可交给 Urt::write。
// 文档未规定串口参数。Pelco-D 地址默认 1；扩展帧的固定头不作为可配置地址。
namespace ptz
{
using PelcoFrame = std::array<std::uint8_t, 7>;
using ExtendedFrame = std::array<std::uint8_t, 18>;
constexpr std::uint8_t kDefaultAddress = 0x01;
constexpr std::uint8_t kPelcoSync = 0xFF;
constexpr std::uint8_t kMaxSpeed = 0x40; // 普通运动/扫描/巡航/定位的速度级
constexpr std::uint16_t kUnitsPerDegree = 100;

// 所有已定义的 Pelco-D 命令均为 Cmd1=0，以下数值为 Cmd2。
enum class PelcoCommand : std::uint8_t
{
    Stop = 0x00,
    Right = 0x02, SetPreset = 0x03, Left = 0x04, ClearPreset = 0x05,
    CallPreset = 0x07, Up = 0x08, AuxiliaryOn = 0x09, AuxiliaryOff = 0x0B,
    Down = 0x10, SetLineScanStart = 0x11, SetLineScanEnd = 0x13,
    LineScanOn = 0x1B, LineScanOff = 0x1D,
    SetScanSpeed = 0x31, SetApplePeelStep = 0x33,
    SetHorizontalAngle = 0x4B, SetVerticalAngle = 0x4D,
    QueryHorizontalAngle = 0x51, QueryVerticalAngle = 0x53,
    HorizontalAngleReport = 0x59, VerticalAngleReport = 0x5B,
    SetPositionSpeed = 0x5F, SetCruiseSpeed = 0x67
};

// 语义方向；组帧时分别转换为 Pelco-D 位标志和扩展帧方向码。
enum class HorizontalDirection : std::uint8_t { None, Right, Left };
enum class VerticalDirection : std::uint8_t { None, Up, Down };
enum class Axis : std::uint8_t { Horizontal, Vertical };
enum class ExtendedCommand : std::uint8_t
{
    VariableSpeed = 0x01, Position = 0x02, PositionWithSpeed = 0x03
};

// 复用预置位命令的特殊功能编号，不能作为普通预置位使用。
enum class SpecialPreset : std::uint8_t
{
    ApplePeelScan = 0x5D,
    WatchOn = 0x70, WatchOff = 0x71, WatchAction = 0x75,
    SelfCheck = 0x77, Reboot = 0x78
};
enum class CruiseDwell : std::uint8_t
{
    Seconds4 = 100, Seconds10, Seconds20, Seconds30,
    Seconds60, Seconds120, Seconds180, Seconds240
};
enum class WatchTime : std::uint8_t
{
    Minutes1 = 81, Minutes2, Minutes5, Minutes10, Minutes15,
    Minutes20, Minutes30, Minutes40, Minutes50, Minutes60
};
enum class WatchAction : std::uint8_t { Preset1, Cruise1, LineScan1 };

std::uint16_t readBigEndian16(const std::uint8_t* data);

// 角度单位为度、扩展速度单位为度/秒；均乘 100 并四舍五入到最近的整数。
// 只检查 16 位表示范围，扩展协议未给出机械限位或最大转速。
std::uint16_t toHundredths(double value);
double fromHundredths(std::uint16_t value);

// Checksum = (Address + Cmd1 + Cmd2 + Data1 + Data2) & 0xFF，不含同步字节。
std::uint8_t pelcoChecksum(const PelcoFrame& frame);

// 原始接口保留所有字节，可用于设备扩展；不检查命令语义。
PelcoFrame encodePelco(std::uint8_t address, std::uint8_t command1,
                       std::uint8_t command2, std::uint8_t data1, std::uint8_t data2);
PelcoFrame makePelcoFrame(PelcoCommand command, std::uint8_t data1 = 0,
                          std::uint8_t data2 = 0, std::uint8_t address = kDefaultAddress);

// 以下便捷接口检查参数；非法参数抛 std::invalid_argument。
// 两轴可以同时运动；None 轴的数据字节固定为 0。
PelcoFrame makeMove(HorizontalDirection horizontal, unsigned horizontalSpeed,
                    VerticalDirection vertical, unsigned verticalSpeed,
                    std::uint8_t address = kDefaultAddress);
PelcoFrame makeStop(std::uint8_t address = kDefaultAddress);
// 普通预置位支持 1~80、131~255；文档未说明 256~2098 的编码。
PelcoFrame makeSetPreset(std::uint16_t preset, std::uint8_t address = kDefaultAddress);
PelcoFrame makeCallPreset(std::uint16_t preset, std::uint8_t address = kDefaultAddress);
PelcoFrame makeClearPreset(std::uint16_t preset, std::uint8_t address = kDefaultAddress);
PelcoFrame makeAuxiliary(bool enabled, std::uint8_t auxiliary,
                         std::uint8_t address = kDefaultAddress);

// 线扫编号 0~7，通常使用 0。
PelcoFrame makeLineScan(PelcoCommand command, unsigned scan = 0,
                        std::uint8_t address = kDefaultAddress);

// 苹果皮扫描：设置起点/终点/调用分别复用 SetPreset/ClearPreset/CallPreset，编号 0x5D。
PelcoFrame makeApplePeelScan(PelcoCommand command, std::uint8_t address = kDefaultAddress);
PelcoFrame makeApplePeelStep(double degrees, std::uint8_t address = kDefaultAddress);
PelcoFrame makeSpeed(PelcoCommand command, unsigned horizontal, unsigned vertical,
                     std::uint8_t address = kDefaultAddress);

// 巡航用文档中的预置位编号 100~107（十进制），不是 0~7。
PelcoFrame makeCallCruise(unsigned preset, std::uint8_t address = kDefaultAddress);
PelcoFrame makeCruiseDwell(CruiseDwell dwell, std::uint8_t address = kDefaultAddress);
PelcoFrame makeWatch(bool enabled, std::uint8_t address = kDefaultAddress);
PelcoFrame makeWatchAction(WatchAction action, std::uint8_t address = kDefaultAddress);
PelcoFrame makeWatchTime(WatchTime time, std::uint8_t address = kDefaultAddress);

// Pelco-D 水平范围 0~360 度；俯仰范围 0~90、270~359.99 度。
// 俯仰采用文档中的无符号圆周角，实际机械范围以设备技术要求为准。
PelcoFrame makeAngle(Axis axis, double degrees, std::uint8_t address = kDefaultAddress);
PelcoFrame makeQueryAngle(Axis axis, std::uint8_t address = kDefaultAddress);
PelcoFrame makeReboot(std::uint8_t address = kDefaultAddress);
PelcoFrame makeSelfCheck(std::uint8_t address = kDefaultAddress);
PelcoFrame makeRealtimeReport(bool enabled, std::uint8_t address = kDefaultAddress);

struct PelcoMessage
{
    std::uint8_t address{0}, command1{0}, command2{0}, data1{0}, data2{0};
};
// 接受完整的单个 7 字节帧，检查同步字节及校验和。失败时不修改 out。
bool decodePelcoFrame(const std::uint8_t* data, std::size_t size, PelcoMessage& out);
struct AngleReport
{
    std::uint8_t address{0};
    Axis axis{Axis::Horizontal};
    std::uint16_t hundredths{0};
    double degrees{0}; // 保留无符号圆周角，不自行转换成负俯仰角
};
// 查询应答和实时回传格式相同；水平 0x59、俯仰 0x5B 各为独立 7 字节帧。
bool decodeAngleReport(const PelcoMessage& message, AngleReport& out);
bool decodeAngleReport(const std::uint8_t* data, std::size_t size, AngleReport& out);

// 串口分包/粘包解析，仅解析已定义回传格式的 Pelco-D；每个实例由同一接收线程使用。
// 校验失败后寻找下一同步字节；内部最多保留 6 字节未完成帧。
class PelcoParser
{
public:
    std::vector<PelcoMessage> feed(const std::uint8_t* data, std::size_t size);
    void reset();
private:
    PelcoFrame buffer_{};
    std::size_t used_{0};
};

// 扩展帧固定 18 字节，末尾固定 00 E6，文档未定义额外校验和。
// 所有 16 位数据均为高字节在前。Raw 接口单位为 0.01 度或 0.01 度/秒。
// 无级变速帧先俯仰后方位：VDir@6、VSpeed@9~10、HDir@11、HSpeed@14~15。
// 文档方位速度举例中重复写 Byte9/10，按两份文档的表格应为 Byte14/15。
ExtendedFrame makeVariableSpeedRaw(HorizontalDirection horizontal, std::uint16_t horizontalSpeed,
                                   VerticalDirection vertical, std::uint16_t verticalSpeed);
ExtendedFrame makeVariableSpeed(HorizontalDirection horizontal, double horizontalDegreesPerSecond,
                                VerticalDirection vertical, double verticalDegreesPerSecond);
ExtendedFrame makePositionRaw(std::uint16_t horizontalAngle, std::uint16_t verticalAngle);
ExtendedFrame makePosition(double horizontalDegrees, double verticalDegrees);
struct AxisPositionRaw
{
    bool enabled{false};
    std::uint16_t speedHundredths{0}, angleHundredths{0};
};
struct AxisPosition
{
    bool enabled{false};
    double degreesPerSecond{0}, degrees{0};
};
// 定位和带速度定位均先方位后俯仰，只有使能字节为 1 的轴才执行。
ExtendedFrame makePositionWithSpeedRaw(const AxisPositionRaw& horizontal, const AxisPositionRaw& vertical);
ExtendedFrame makePositionWithSpeed(const AxisPosition& horizontal, const AxisPosition& vertical);

enum class Error
{
    None, NotConnected, InvalidArgument, WriteFailed, Timeout, ConnectionLost, NeedsReconnect
};
struct Result
{
    Error error{Error::None};
    PelcoMessage frame; // 仅 queryAngle 成功时含应答；发送命令没有文档定义的 ACK
    explicit operator bool() const;
};
const char* errorString(Error error);
} // namespace ptz

// 与 AiruiCore 一致，使用 PImpl 隐藏 Urt、接收线程、同步等待及锁。
// 文档未指定串口设备和波特率，构造时必须显式传入；使用 8N1。
// 控制命令返回 true 仅表示完整发送；角度查询等待本地址对应轴的回传。
// 查询与实时回传格式相同，无法区分来源；查询返回请求后收到的第一个匹配样本。
// 超时后查询需 reconnect，防止迟到应答误配；stop 等发送接口仍可调用。
// 命令内部串行执行；connect/disconnect 由同一控制线程管理，disconnect 可唤醒查询。
// 回调在接收线程执行，不应同步发命令、断开或析构本对象，应投递到控制线程。
class PtzCore
{
public:
    explicit PtzCore(std::string port, std::uint32_t baud, std::uint8_t address = ptz::kDefaultAddress);
    ~PtzCore();
    PtzCore(const PtzCore&) = delete;
    PtzCore& operator=(const PtzCore&) = delete;

    bool connect();
    void disconnect();
    bool reconnect();
    bool isConnected() const;
    bool setResponseTimeout(int milliseconds); // 默认 500ms
    ptz::Result lastResult() const;
    // 仅派发本地址未被同步查询消耗的帧，角度帧同时派发到两个回调。
    void setFrameCallback(std::function<void(const ptz::PelcoMessage&)> callback);
    void setAngleCallback(std::function<void(const ptz::AngleReport&)> callback);

    // 低层发送入口检查帧格式；Pelco-D 允许使用帧内指定的其他设备地址。
    ptz::Result send(const ptz::PelcoFrame& frame);
    ptz::Result send(const ptz::ExtendedFrame& frame);
    ptz::Result queryAngle(ptz::Axis axis, int timeoutMs = -1);

    bool move(ptz::HorizontalDirection horizontal, unsigned horizontalSpeed,
              ptz::VerticalDirection vertical, unsigned verticalSpeed);
    bool stop();
    bool setPreset(std::uint16_t preset);
    bool callPreset(std::uint16_t preset);
    bool clearPreset(std::uint16_t preset);
    bool setAuxiliary(bool enabled, std::uint8_t auxiliary);
    bool setLineScanStart(unsigned scan = 0);
    bool setLineScanEnd(unsigned scan = 0);
    bool setLineScanEnabled(bool enabled, unsigned scan = 0);
    bool setApplePeelScanStart();
    bool setApplePeelScanEnd();
    bool callApplePeelScan();
    bool setApplePeelStep(double degrees);
    bool setScanSpeed(unsigned horizontal, unsigned vertical);
    bool callCruise(unsigned preset); // 100~107
    bool setCruiseDwell(ptz::CruiseDwell dwell);
    bool setCruiseSpeed(unsigned horizontal, unsigned vertical);
    bool setWatch(bool enabled);
    bool setWatchAction(ptz::WatchAction action);
    bool setWatchTime(ptz::WatchTime time);
    bool setHorizontalAngle(double degrees);
    bool setVerticalAngle(double degrees);
    bool getHorizontalAngle(double& degrees);
    bool getVerticalAngle(double& degrees);
    bool reboot();
    bool selfCheck();
    bool setRealtimeReport(bool enabled);
    bool setPositionSpeed(unsigned horizontal, unsigned vertical);
    // 扩展速度单位为度/秒，定位角度单位为度。
    bool moveVariableSpeed(ptz::HorizontalDirection horizontal, double horizontalDegreesPerSecond,
                           ptz::VerticalDirection vertical, double verticalDegreesPerSecond);
    bool setPosition(double horizontalDegrees, double verticalDegrees);
    bool setPositionWithSpeed(const ptz::AxisPosition& horizontal, const ptz::AxisPosition& vertical);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool invalidArgument();
    bool getAngle(ptz::Axis axis, double& degrees);
};
