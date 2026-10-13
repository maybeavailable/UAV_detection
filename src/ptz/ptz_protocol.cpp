#include"ptz_protocol.h"
#include"common/urt.h"

#include<atomic>
#include<chrono>
#include<cmath>
#include<condition_variable>
#include<limits>
#include<mutex>
#include<optional>
#include<stdexcept>
#include<utility>

namespace ptz
{
namespace
{
void require(bool valid, const char* message)
{
    if (!valid) throw std::invalid_argument(message);
}
void requireSpeed(unsigned speed)
{
    require(speed <= kMaxSpeed, "PTZ speed level must be in [0, 64]");
}
void requirePreset(std::uint16_t preset)
{
    // 文档写 1~80、131~2098，但 Data1 固定 00、Data2 只有一字节。
    // 未说明 256~2098 的编码；仅接受能按文档无歧义发送的编号，禁止截断。
    require((preset >= 1 && preset <= 80) || (preset >= 131 && preset <= 255),
            "PTZ preset must be in [1, 80] or [131, 255]; larger IDs have no documented encoding");
}
void putBigEndian16(std::uint8_t* data, std::uint16_t value)
{
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}
ExtendedFrame extendedBase(ExtendedCommand command)
{
    return {0x6E, 0x01, 0x10, 0xAA, 0x0B, static_cast<std::uint8_t>(command),
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE6};
}
} // namespace

std::uint16_t readBigEndian16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) | data[1]);
}

std::uint16_t toHundredths(double value)
{
    require(std::isfinite(value) && value >= 0.0 && value <= 655.35,
            "PTZ value must be finite and in [0, 655.35]");
    return static_cast<std::uint16_t>(std::round(value * kUnitsPerDegree));
}

double fromHundredths(std::uint16_t value)
{
    return static_cast<double>(value) / kUnitsPerDegree;
}

std::uint8_t pelcoChecksum(const PelcoFrame& frame)
{
    unsigned sum = 0;
    for (std::size_t i = 1; i <= 5; ++i) sum += frame[i];
    return static_cast<std::uint8_t>(sum & 0xFF);
}

PelcoFrame encodePelco(std::uint8_t address, std::uint8_t command1,
                       std::uint8_t command2, std::uint8_t data1, std::uint8_t data2)
{
    PelcoFrame frame{kPelcoSync, address, command1, command2, data1, data2, 0};
    frame[6] = pelcoChecksum(frame);
    return frame;
}

PelcoFrame makePelcoFrame(PelcoCommand command, std::uint8_t data1,
                          std::uint8_t data2, std::uint8_t address)
{
    return encodePelco(address, 0, static_cast<std::uint8_t>(command), data1, data2);
}

PelcoFrame makeMove(HorizontalDirection horizontal, unsigned horizontalSpeed,
                    VerticalDirection vertical, unsigned verticalSpeed,
                    std::uint8_t address)
{
    require(static_cast<unsigned>(horizontal) <= 2 && static_cast<unsigned>(vertical) <= 2,
            "Invalid PTZ direction");
    requireSpeed(horizontalSpeed);
    requireSpeed(verticalSpeed);
    const std::uint8_t h = horizontal == HorizontalDirection::Right ? 0x02 :
                           horizontal == HorizontalDirection::Left ? 0x04 : 0x00;
    const std::uint8_t v = vertical == VerticalDirection::Up ? 0x08 :
                           vertical == VerticalDirection::Down ? 0x10 : 0x00;
    return encodePelco(address, 0, static_cast<std::uint8_t>(h | v),
                       horizontal == HorizontalDirection::None ? 0 : static_cast<std::uint8_t>(horizontalSpeed),
                       vertical == VerticalDirection::None ? 0 : static_cast<std::uint8_t>(verticalSpeed));
}

PelcoFrame makeStop(std::uint8_t address)
{
    return makePelcoFrame(PelcoCommand::Stop, 0, 0, address);
}

PelcoFrame makeSetPreset(std::uint16_t preset, std::uint8_t address)
{
    requirePreset(preset);
    return makePelcoFrame(PelcoCommand::SetPreset, 0, static_cast<std::uint8_t>(preset), address);
}

PelcoFrame makeCallPreset(std::uint16_t preset, std::uint8_t address)
{
    requirePreset(preset);
    return makePelcoFrame(PelcoCommand::CallPreset, 0, static_cast<std::uint8_t>(preset), address);
}

PelcoFrame makeClearPreset(std::uint16_t preset, std::uint8_t address)
{
    requirePreset(preset);
    return makePelcoFrame(PelcoCommand::ClearPreset, 0, static_cast<std::uint8_t>(preset), address);
}

PelcoFrame makeAuxiliary(bool enabled, std::uint8_t auxiliary,
                         std::uint8_t address)
{
    return makePelcoFrame(enabled ? PelcoCommand::AuxiliaryOn : PelcoCommand::AuxiliaryOff,
                          0, auxiliary, address);
}

PelcoFrame makeLineScan(PelcoCommand command, unsigned scan,
                        std::uint8_t address)
{
    require(command == PelcoCommand::SetLineScanStart || command == PelcoCommand::SetLineScanEnd ||
            command == PelcoCommand::LineScanOn || command == PelcoCommand::LineScanOff,
            "Invalid PTZ line scan command");
    require(scan <= 7, "PTZ line scan must be in [0, 7]");
    return makePelcoFrame(command, 0, static_cast<std::uint8_t>(scan), address);
}

PelcoFrame makeApplePeelScan(PelcoCommand command, std::uint8_t address)
{
    require(command == PelcoCommand::SetPreset || command == PelcoCommand::ClearPreset ||
            command == PelcoCommand::CallPreset, "Invalid PTZ apple peel scan command");
    return makePelcoFrame(command, 0, static_cast<std::uint8_t>(SpecialPreset::ApplePeelScan), address);
}

PelcoFrame makeApplePeelStep(double degrees, std::uint8_t address)
{
    const auto value = toHundredths(degrees);
    return makePelcoFrame(PelcoCommand::SetApplePeelStep, static_cast<std::uint8_t>(value >> 8),
                          static_cast<std::uint8_t>(value & 0xFF), address);
}

PelcoFrame makeSpeed(PelcoCommand command, unsigned horizontal, unsigned vertical,
                     std::uint8_t address)
{
    require(command == PelcoCommand::SetScanSpeed || command == PelcoCommand::SetCruiseSpeed ||
            command == PelcoCommand::SetPositionSpeed, "Invalid PTZ speed command");
    requireSpeed(horizontal);
    requireSpeed(vertical);
    return makePelcoFrame(command, static_cast<std::uint8_t>(horizontal),
                          static_cast<std::uint8_t>(vertical), address);
}

PelcoFrame makeCallCruise(unsigned preset, std::uint8_t address)
{
    require(preset >= 100 && preset <= 107, "PTZ cruise preset must be in [100, 107]");
    return makePelcoFrame(PelcoCommand::CallPreset, 0, static_cast<std::uint8_t>(preset), address);
}

PelcoFrame makeCruiseDwell(CruiseDwell dwell, std::uint8_t address)
{
    const auto value = static_cast<std::uint8_t>(dwell);
    require(value >= 100 && value <= 107, "Invalid PTZ cruise dwell");
    return makePelcoFrame(PelcoCommand::SetPreset, 0, value, address);
}

PelcoFrame makeWatch(bool enabled, std::uint8_t address)
{
    return makePelcoFrame(PelcoCommand::CallPreset, 0,
                          static_cast<std::uint8_t>(enabled ? SpecialPreset::WatchOn : SpecialPreset::WatchOff),
                          address);
}

PelcoFrame makeWatchAction(WatchAction action, std::uint8_t address)
{
    PelcoCommand command;
    switch (action)
    {
    case WatchAction::Preset1: command = PelcoCommand::SetPreset; break;
    case WatchAction::Cruise1: command = PelcoCommand::ClearPreset; break;
    case WatchAction::LineScan1: command = PelcoCommand::CallPreset; break;
    default: throw std::invalid_argument("Invalid PTZ watch action");
    }
    return makePelcoFrame(command, 0, static_cast<std::uint8_t>(SpecialPreset::WatchAction), address);
}

PelcoFrame makeWatchTime(WatchTime time, std::uint8_t address)
{
    const auto value = static_cast<std::uint8_t>(time);
    require(value >= 81 && value <= 90, "Invalid PTZ watch time");
    return makePelcoFrame(PelcoCommand::SetPreset, 0, value, address);
}

PelcoFrame makeAngle(Axis axis, double degrees, std::uint8_t address)
{
    require(std::isfinite(degrees), "PTZ angle must be finite");
    PelcoCommand command;
    switch (axis)
    {
    case Axis::Horizontal:
        require(degrees >= 0.0 && degrees <= 360.0, "PTZ horizontal angle must be in [0, 360]");
        command = PelcoCommand::SetHorizontalAngle;
        break;
    case Axis::Vertical:
        require((degrees >= 0.0 && degrees <= 90.0) || (degrees >= 270.0 && degrees <= 359.99),
                "PTZ vertical angle must be in [0, 90] or [270, 359.99]");
        command = PelcoCommand::SetVerticalAngle;
        break;
    default: throw std::invalid_argument("Invalid PTZ angle axis");
    }
    const auto value = toHundredths(degrees);
    return makePelcoFrame(command, static_cast<std::uint8_t>(value >> 8),
                          static_cast<std::uint8_t>(value & 0xFF), address);
}

PelcoFrame makeQueryAngle(Axis axis, std::uint8_t address)
{
    require(axis == Axis::Horizontal || axis == Axis::Vertical, "Invalid PTZ angle axis");
    return makePelcoFrame(axis == Axis::Horizontal ? PelcoCommand::QueryHorizontalAngle :
                          PelcoCommand::QueryVerticalAngle, 0, 0, address);
}

PelcoFrame makeReboot(std::uint8_t address)
{
    return makePelcoFrame(PelcoCommand::CallPreset, 0, static_cast<std::uint8_t>(SpecialPreset::Reboot), address);
}

PelcoFrame makeSelfCheck(std::uint8_t address)
{
    return makePelcoFrame(PelcoCommand::CallPreset, 0, static_cast<std::uint8_t>(SpecialPreset::SelfCheck), address);
}

PelcoFrame makeRealtimeReport(bool enabled, std::uint8_t address)
{
    return makeAuxiliary(enabled, 0x05, address);
}

bool decodePelcoFrame(const std::uint8_t* data, std::size_t size, PelcoMessage& out)
{
    if (!data || size != PelcoFrame{}.size() || data[0] != kPelcoSync) return false;
    unsigned sum = 0;
    for (std::size_t i = 1; i <= 5; ++i) sum += data[i];
    if (static_cast<std::uint8_t>(sum & 0xFF) != data[6]) return false;
    out = {data[1], data[2], data[3], data[4], data[5]};
    return true;
}

bool decodeAngleReport(const PelcoMessage& message, AngleReport& out)
{
    if (message.command1 != 0) return false;
    Axis axis;
    if (message.command2 == static_cast<std::uint8_t>(PelcoCommand::HorizontalAngleReport))
        axis = Axis::Horizontal;
    else if (message.command2 == static_cast<std::uint8_t>(PelcoCommand::VerticalAngleReport))
        axis = Axis::Vertical;
    else return false;
    const auto raw = static_cast<std::uint16_t>((static_cast<unsigned>(message.data1) << 8) | message.data2);
    out = {message.address, axis, raw, fromHundredths(raw)};
    return true;
}

bool decodeAngleReport(const std::uint8_t* data, std::size_t size, AngleReport& out)
{
    PelcoMessage message;
    return decodePelcoFrame(data, size, message) && decodeAngleReport(message, out);
}

ExtendedFrame makeVariableSpeedRaw(HorizontalDirection horizontal, std::uint16_t horizontalSpeed,
                                   VerticalDirection vertical, std::uint16_t verticalSpeed)
{
    require(horizontal == HorizontalDirection::Right || horizontal == HorizontalDirection::Left,
            "Extended PTZ horizontal direction must be Right or Left");
    require(vertical == VerticalDirection::Up || vertical == VerticalDirection::Down,
            "Extended PTZ vertical direction must be Up or Down");
    auto frame = extendedBase(ExtendedCommand::VariableSpeed);
    frame[6] = static_cast<std::uint8_t>(vertical);
    putBigEndian16(frame.data() + 9, verticalSpeed);
    frame[11] = static_cast<std::uint8_t>(horizontal);
    putBigEndian16(frame.data() + 14, horizontalSpeed);
    return frame;
}

ExtendedFrame makeVariableSpeed(HorizontalDirection horizontal, double horizontalDegreesPerSecond,
                                VerticalDirection vertical, double verticalDegreesPerSecond)
{
    return makeVariableSpeedRaw(horizontal, toHundredths(horizontalDegreesPerSecond),
                                 vertical, toHundredths(verticalDegreesPerSecond));
}

ExtendedFrame makePositionRaw(std::uint16_t horizontalAngle, std::uint16_t verticalAngle)
{
    auto frame = extendedBase(ExtendedCommand::Position);
    putBigEndian16(frame.data() + 9, horizontalAngle);
    putBigEndian16(frame.data() + 14, verticalAngle);
    return frame;
}

ExtendedFrame makePosition(double horizontalDegrees, double verticalDegrees)
{
    return makePositionRaw(toHundredths(horizontalDegrees), toHundredths(verticalDegrees));
}

ExtendedFrame makePositionWithSpeedRaw(const AxisPositionRaw& horizontal, const AxisPositionRaw& vertical)
{
    auto frame = extendedBase(ExtendedCommand::PositionWithSpeed);
    frame[6] = horizontal.enabled ? 0x01 : 0x00;
    putBigEndian16(frame.data() + 7, horizontal.speedHundredths);
    putBigEndian16(frame.data() + 9, horizontal.angleHundredths);
    frame[11] = vertical.enabled ? 0x01 : 0x00;
    putBigEndian16(frame.data() + 12, vertical.speedHundredths);
    putBigEndian16(frame.data() + 14, vertical.angleHundredths);
    return frame;
}

ExtendedFrame makePositionWithSpeed(const AxisPosition& horizontal, const AxisPosition& vertical)
{
    return makePositionWithSpeedRaw(
        {horizontal.enabled, toHundredths(horizontal.degreesPerSecond), toHundredths(horizontal.degrees)},
        {vertical.enabled, toHundredths(vertical.degreesPerSecond), toHundredths(vertical.degrees)});
}

std::vector<PelcoMessage> PelcoParser::feed(const std::uint8_t* data, std::size_t size)
{
    require(data || size == 0, "Null PTZ receive buffer");
    std::vector<PelcoMessage> messages;
    for (std::size_t i = 0; i < size; ++i)
    {
        if (used_ == 0 && data[i] != kPelcoSync) continue;
        buffer_[used_++] = data[i];
        if (used_ != buffer_.size()) continue;
        PelcoMessage message;
        if (decodePelcoFrame(buffer_.data(), buffer_.size(), message))
        {
            messages.push_back(message);
            used_ = 0;
        }
        else
        {
            std::size_t next = 1;
            while (next < used_ && buffer_[next] != kPelcoSync) ++next;
            for (std::size_t j = next; j < used_; ++j) buffer_[j - next] = buffer_[j];
            used_ -= next;
        }
    }
    return messages;
}

void PelcoParser::reset()
{
    used_ = 0;
}

Result::operator bool() const { return error == Error::None; }

const char* errorString(Error error)
{
    switch (error)
    {
    case Error::None: return "Success";
    case Error::NotConnected: return "Not connected";
    case Error::InvalidArgument: return "Invalid argument";
    case Error::WriteFailed: return "Serial write failed";
    case Error::Timeout: return "Angle response timed out";
    case Error::ConnectionLost: return "Connection lost";
    case Error::NeedsReconnect: return "Reconnect before querying again";
    }
    return "Unknown PTZ error";
}
} // namespace ptz

struct PtzCore::Impl
{
    const std::string port;
    const std::uint32_t baud;
    const std::uint8_t address;
    Urt uart;
    std::atomic<bool> connected{false};
    std::atomic<int> timeout{500};
    std::mutex transactionMutex, receiveMutex, callbackMutex, resultMutex;
    std::condition_variable ready;
    ptz::PelcoParser parser;
    std::optional<ptz::Axis> pending;
    std::optional<ptz::PelcoMessage> received;
    bool synchronized{true}; // receiveMutex 保护；查询超时后不自动重试
    ptz::Result last;
    std::function<void(const ptz::PelcoMessage&)> frameCallback;
    std::function<void(const ptz::AngleReport&)> angleCallback;

    Impl(std::string path, std::uint32_t rate, std::uint8_t deviceAddress)
        : port(std::move(path)), baud(rate), address(deviceAddress)
    {
        uart.set_error_callback([this](Urt::Error, int) {
            { std::lock_guard<std::mutex> lock(receiveMutex); connected.store(false); }
            ready.notify_all();
        });
    }

    ptz::Result record(ptz::Result result)
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        last = result;
        return result;
    }

    template<std::size_t N>
    ptz::Result sendFrame(const std::array<std::uint8_t, N>& frame)
    {
        std::lock_guard<std::mutex> transaction(transactionMutex);
        if (!connected.load()) return record({ptz::Error::NotConnected, {}});
        if (uart.write(frame.data(), frame.size()) != static_cast<int>(frame.size()))
        {
            { std::lock_guard<std::mutex> receive(receiveMutex); connected.store(false); }
            ready.notify_all();
            return record({ptz::Error::WriteFailed, {}});
        }
        if (!connected.load()) return record({ptz::Error::ConnectionLost, {}});
        return record({});
    }

    template<class Builder>
    bool execute(Builder builder)
    {
        try { return static_cast<bool>(sendFrame(builder())); }
        catch (const std::invalid_argument&) { record({ptz::Error::InvalidArgument, {}}); return false; }
    }

    void receive(const std::uint8_t* data, std::size_t size)
    {
        std::vector<ptz::PelcoMessage> unhandled;
        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            if (!connected.load()) return;
            for (const auto& frame : parser.feed(data, size))
            {
                if (frame.address != address) continue;
                ptz::AngleReport report;
                if (pending && !received && ptz::decodeAngleReport(frame, report) && report.axis == *pending)
                {
                    received = frame;
                    ready.notify_all();
                }
                else unhandled.push_back(frame);
            }
        }
        for (const auto& frame : unhandled)
        {
            std::function<void(const ptz::PelcoMessage&)> onFrame;
            std::function<void(const ptz::AngleReport&)> onAngle;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                onFrame = frameCallback;
                onAngle = angleCallback;
            }
            try
            {
                ptz::AngleReport report;
                if (onAngle && ptz::decodeAngleReport(frame, report)) onAngle(report);
                if (onFrame) onFrame(frame);
            }
            catch (const std::exception& error) { UAV_LOG_ERROR << "PTZ callback: " << error.what(); }
            catch (...) { UAV_LOG_ERROR << "PTZ callback threw an unknown exception"; }
        }
    }
};

PtzCore::PtzCore(std::string port, std::uint32_t baud, std::uint8_t address)
    : impl_(std::make_unique<Impl>(std::move(port), baud, address)) {}
PtzCore::~PtzCore() { disconnect(); }

bool PtzCore::connect()
{
    { std::lock_guard<std::mutex> receive(impl_->receiveMutex); impl_->connected.store(false); }
    impl_->ready.notify_all();
    std::lock_guard<std::mutex> transaction(impl_->transactionMutex);
    impl_->uart.close();
    if (impl_->baud > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        impl_->uart.open(impl_->port, static_cast<int>(impl_->baud), 8, 'N', 1, false, true) != 0)
    {
        impl_->record({ptz::Error::NotConnected, {}});
        return false;
    }
    {
        std::lock_guard<std::mutex> receive(impl_->receiveMutex);
        impl_->parser.reset();
        impl_->pending.reset();
        impl_->received.reset();
        impl_->synchronized = true;
        impl_->connected.store(true);
    }
    impl_->uart.clear_error();
    if (impl_->uart.start_receive([this](const std::uint8_t* data, std::size_t size) { impl_->receive(data, size); }) != 0)
    {
        impl_->connected.store(false);
        impl_->uart.close();
        impl_->record({ptz::Error::NotConnected, {}});
        return false;
    }
    impl_->record({});
    return true;
}

void PtzCore::disconnect()
{
    // 在等待谓词使用的锁内改变状态，避免取消通知丢失。
    { std::lock_guard<std::mutex> receive(impl_->receiveMutex); impl_->connected.store(false); }
    impl_->ready.notify_all();
    std::lock_guard<std::mutex> transaction(impl_->transactionMutex);
    impl_->uart.close();
    std::lock_guard<std::mutex> receive(impl_->receiveMutex);
    impl_->pending.reset();
    impl_->received.reset();
    impl_->parser.reset();
}

bool PtzCore::reconnect() { disconnect(); return connect(); }
bool PtzCore::isConnected() const { return impl_->connected.load() && impl_->uart.is_open(); }
bool PtzCore::setResponseTimeout(int milliseconds)
{
    if (milliseconds <= 0) return invalidArgument();
    impl_->timeout.store(milliseconds);
    return true;
}
ptz::Result PtzCore::lastResult() const
{
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    return impl_->last;
}
void PtzCore::setFrameCallback(std::function<void(const ptz::PelcoMessage&)> callback)
{
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->frameCallback = std::move(callback);
}
void PtzCore::setAngleCallback(std::function<void(const ptz::AngleReport&)> callback)
{
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->angleCallback = std::move(callback);
}

ptz::Result PtzCore::send(const ptz::PelcoFrame& frame)
{
    ptz::PelcoMessage decoded;
    if (!ptz::decodePelcoFrame(frame.data(), frame.size(), decoded))
        return impl_->record({ptz::Error::InvalidArgument, {}});
    return impl_->sendFrame(frame);
}
ptz::Result PtzCore::send(const ptz::ExtendedFrame& frame)
{
    constexpr std::array<std::uint8_t, 5> header{0x6E, 0x01, 0x10, 0xAA, 0x0B};
    for (std::size_t i = 0; i < header.size(); ++i)
        if (frame[i] != header[i]) return impl_->record({ptz::Error::InvalidArgument, {}});
    if (frame[16] != 0x00 || frame[17] != 0xE6 || frame[5] < 1 || frame[5] > 3)
        return impl_->record({ptz::Error::InvalidArgument, {}});
    if (frame[5] == static_cast<std::uint8_t>(ptz::ExtendedCommand::VariableSpeed) &&
        (frame[6] < 1 || frame[6] > 2 || frame[11] < 1 || frame[11] > 2 ||
         frame[7] != 0 || frame[8] != 0 || frame[12] != 0 || frame[13] != 0))
        return impl_->record({ptz::Error::InvalidArgument, {}});
    if (frame[5] == static_cast<std::uint8_t>(ptz::ExtendedCommand::Position) &&
        (frame[6] != 0 || frame[7] != 0 || frame[8] != 0 || frame[11] != 0 || frame[12] != 0 || frame[13] != 0))
        return impl_->record({ptz::Error::InvalidArgument, {}});
    return impl_->sendFrame(frame);
}

ptz::Result PtzCore::queryAngle(ptz::Axis axis, int timeoutMs)
{
    ptz::PelcoFrame frame;
    try { frame = ptz::makeQueryAngle(axis, impl_->address); }
    catch (const std::invalid_argument&) { return impl_->record({ptz::Error::InvalidArgument, {}}); }
    if (timeoutMs == -1) timeoutMs = impl_->timeout.load();
    if (timeoutMs <= 0) return impl_->record({ptz::Error::InvalidArgument, {}});
    std::lock_guard<std::mutex> transaction(impl_->transactionMutex);
    {
        std::lock_guard<std::mutex> receive(impl_->receiveMutex);
        if (!impl_->connected.load()) return impl_->record({ptz::Error::NotConnected, {}});
        if (!impl_->synchronized) return impl_->record({ptz::Error::NeedsReconnect, {}});
        impl_->pending = axis;
        impl_->received.reset();
    }
    if (impl_->uart.write(frame.data(), frame.size()) != static_cast<int>(frame.size()))
    {
        std::lock_guard<std::mutex> receive(impl_->receiveMutex);
        impl_->pending.reset();
        impl_->synchronized = false;
        impl_->connected.store(false);
        return impl_->record({ptz::Error::WriteFailed, {}});
    }
    std::unique_lock<std::mutex> receive(impl_->receiveMutex);
    const bool arrived = impl_->ready.wait_for(receive, std::chrono::milliseconds(timeoutMs), [this] {
        return impl_->received.has_value() || !impl_->connected.load();
    });
    impl_->pending.reset();
    if (!impl_->connected.load()) return impl_->record({ptz::Error::ConnectionLost, {}});
    if (!arrived)
    {
        impl_->synchronized = false;
        return impl_->record({ptz::Error::Timeout, {}});
    }
    const ptz::Result result{ptz::Error::None, *impl_->received};
    impl_->received.reset();
    return impl_->record(result);
}

bool PtzCore::invalidArgument()
{
    impl_->record({ptz::Error::InvalidArgument, {}});
    return false;
}
bool PtzCore::getAngle(ptz::Axis axis, double& degrees)
{
    const auto result = queryAngle(axis);
    if (!result) return false;
    ptz::AngleReport report;
    if (!ptz::decodeAngleReport(result.frame, report)) return false;
    degrees = report.degrees;
    return true;
}

bool PtzCore::move(ptz::HorizontalDirection horizontal, unsigned horizontalSpeed,
                   ptz::VerticalDirection vertical, unsigned verticalSpeed)
{
    return impl_->execute([&] { return ptz::makeMove(horizontal, horizontalSpeed, vertical, verticalSpeed, impl_->address); });
}
bool PtzCore::stop() { return impl_->execute([&] { return ptz::makeStop(impl_->address); }); }
bool PtzCore::setPreset(std::uint16_t preset)
{
    return impl_->execute([&] { return ptz::makeSetPreset(preset, impl_->address); });
}
bool PtzCore::callPreset(std::uint16_t preset)
{
    return impl_->execute([&] { return ptz::makeCallPreset(preset, impl_->address); });
}
bool PtzCore::clearPreset(std::uint16_t preset)
{
    return impl_->execute([&] { return ptz::makeClearPreset(preset, impl_->address); });
}
bool PtzCore::setAuxiliary(bool enabled, std::uint8_t auxiliary)
{
    return impl_->execute([&] { return ptz::makeAuxiliary(enabled, auxiliary, impl_->address); });
}
bool PtzCore::setLineScanStart(unsigned scan)
{
    return impl_->execute([&] { return ptz::makeLineScan(ptz::PelcoCommand::SetLineScanStart, scan, impl_->address); });
}
bool PtzCore::setLineScanEnd(unsigned scan)
{
    return impl_->execute([&] { return ptz::makeLineScan(ptz::PelcoCommand::SetLineScanEnd, scan, impl_->address); });
}
bool PtzCore::setLineScanEnabled(bool enabled, unsigned scan)
{
    return impl_->execute([&] { return ptz::makeLineScan(enabled ? ptz::PelcoCommand::LineScanOn : ptz::PelcoCommand::LineScanOff,
                                                      scan, impl_->address); });
}
bool PtzCore::setApplePeelScanStart()
{
    return impl_->execute([&] { return ptz::makeApplePeelScan(ptz::PelcoCommand::SetPreset, impl_->address); });
}
bool PtzCore::setApplePeelScanEnd()
{
    return impl_->execute([&] { return ptz::makeApplePeelScan(ptz::PelcoCommand::ClearPreset, impl_->address); });
}
bool PtzCore::callApplePeelScan()
{
    return impl_->execute([&] { return ptz::makeApplePeelScan(ptz::PelcoCommand::CallPreset, impl_->address); });
}
bool PtzCore::setApplePeelStep(double degrees)
{
    return impl_->execute([&] { return ptz::makeApplePeelStep(degrees, impl_->address); });
}
bool PtzCore::setScanSpeed(unsigned horizontal, unsigned vertical)
{
    return impl_->execute([&] { return ptz::makeSpeed(ptz::PelcoCommand::SetScanSpeed, horizontal, vertical, impl_->address); });
}
bool PtzCore::callCruise(unsigned preset)
{
    return impl_->execute([&] { return ptz::makeCallCruise(preset, impl_->address); });
}
bool PtzCore::setCruiseDwell(ptz::CruiseDwell dwell)
{
    return impl_->execute([&] { return ptz::makeCruiseDwell(dwell, impl_->address); });
}
bool PtzCore::setCruiseSpeed(unsigned horizontal, unsigned vertical)
{
    return impl_->execute([&] { return ptz::makeSpeed(ptz::PelcoCommand::SetCruiseSpeed, horizontal, vertical, impl_->address); });
}
bool PtzCore::setWatch(bool enabled)
{
    return impl_->execute([&] { return ptz::makeWatch(enabled, impl_->address); });
}
bool PtzCore::setWatchAction(ptz::WatchAction action)
{
    return impl_->execute([&] { return ptz::makeWatchAction(action, impl_->address); });
}
bool PtzCore::setWatchTime(ptz::WatchTime time)
{
    return impl_->execute([&] { return ptz::makeWatchTime(time, impl_->address); });
}
bool PtzCore::setHorizontalAngle(double degrees)
{
    return impl_->execute([&] { return ptz::makeAngle(ptz::Axis::Horizontal, degrees, impl_->address); });
}
bool PtzCore::setVerticalAngle(double degrees)
{
    return impl_->execute([&] { return ptz::makeAngle(ptz::Axis::Vertical, degrees, impl_->address); });
}
bool PtzCore::getHorizontalAngle(double& degrees) { return getAngle(ptz::Axis::Horizontal, degrees); }
bool PtzCore::getVerticalAngle(double& degrees) { return getAngle(ptz::Axis::Vertical, degrees); }
bool PtzCore::reboot() { return impl_->execute([&] { return ptz::makeReboot(impl_->address); }); }
bool PtzCore::selfCheck() { return impl_->execute([&] { return ptz::makeSelfCheck(impl_->address); }); }
bool PtzCore::setRealtimeReport(bool enabled)
{
    return impl_->execute([&] { return ptz::makeRealtimeReport(enabled, impl_->address); });
}
bool PtzCore::setPositionSpeed(unsigned horizontal, unsigned vertical)
{
    return impl_->execute([&] { return ptz::makeSpeed(ptz::PelcoCommand::SetPositionSpeed, horizontal, vertical, impl_->address); });
}
bool PtzCore::moveVariableSpeed(ptz::HorizontalDirection horizontal, double horizontalDegreesPerSecond,
                               ptz::VerticalDirection vertical, double verticalDegreesPerSecond)
{
    return impl_->execute([&] { return ptz::makeVariableSpeed(horizontal, horizontalDegreesPerSecond,
                                                           vertical, verticalDegreesPerSecond); });
}
bool PtzCore::setPosition(double horizontalDegrees, double verticalDegrees)
{
    return impl_->execute([&] { return ptz::makePosition(horizontalDegrees, verticalDegrees); });
}
bool PtzCore::setPositionWithSpeed(const ptz::AxisPosition& horizontal, const ptz::AxisPosition& vertical)
{
    return impl_->execute([&] { return ptz::makePositionWithSpeed(horizontal, vertical); });
}
