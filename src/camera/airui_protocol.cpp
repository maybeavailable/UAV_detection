#include"camera/airui_protocol.h"
#include"common/urt.h"

#include<algorithm>
#include<atomic>
#include<chrono>
#include<cmath>
#include<condition_variable>
#include<cstring>
#include<limits>
#include<mutex>
#include<stdexcept>
#include<utility>

namespace airui
{
const std::vector<CommandInfo>& commandCatalog()
{
    // 长度按正文解释及收发报文交叉核对；文档勘误见 docs/airui-protocol.md。
    static const std::vector<CommandInfo> catalog = {
        {Command::AutoShutter, "AutoShutter", -1, 1, -1, 0, false, 6},
        {Command::Correction, "Correction", -1, -1, 2, 0, false, 6},
        {Command::ShutterTimeInterval, "ShutterTimeInterval", 0, 1, -1, 1, false, 6},
        {Command::ShutterFpaInterval, "ShutterFpaInterval", 0, 1, -1, 1, false, 7},
        {Command::ShutterCoreInterval, "ShutterCoreInterval", 0, 1, -1, 1, false, 8},
        {Command::CoreTemperature, "CoreTemperature", 0, -1, -1, 2, false, 8},
        {Command::FpaTemperature, "FpaTemperature", 0, -1, -1, 2, false, 9},
        {Command::SerialNumber, "SerialNumber", 0, -1, -1, 64, false, 5},
        {Command::SensorWidth, "SensorWidth", 0, -1, -1, 2, false, 5},
        {Command::SensorHeight, "SensorHeight", 0, -1, -1, 2, false, 5},
        {Command::SaveSettings, "SaveSettings", -1, -1, 0, 0, false, 9},
        {Command::FactoryReset, "FactoryReset", -1, -1, 1, 0, false, 9},
        {Command::DigitalZoom, "DigitalZoom", -1, -1, 8, 0, false, 10},
        {Command::ImageFlip, "ImageFlip", -1, 1, -1, 0, false, 13},
        {Command::AnalogVideo, "AnalogVideo", -1, -1, 1, 0, false, 13},
        {Command::Freeze, "Freeze", -1, -1, 1, 0, false, 13},
        {Command::BootLogo, "BootLogo", -1, -1, 1, 0, false, 14},
        {Command::Palette, "Palette", 1, -1, 1, 1, false, 14},
        {Command::WarningThreshold, "WarningThreshold", -1, 2, -1, 0, false, 16},
        {Command::DigitalVideo, "DigitalVideo", -1, -1, 2, 0, false, 16},
        {Command::VideoSource, "VideoSource", -1, 1, -1, 0, false, 17},
        {Command::ImageMode, "ImageMode", 0, 1, -1, 4, false, 17},
        {Command::Contrast, "Contrast", 0, 1, -1, 1, false, 18},
        {Command::Brightness, "Brightness", 0, 1, -1, 1, false, 19},
        {Command::Detail, "Detail", 0, 1, -1, 1, false, 19},
        {Command::SpatialFilter, "SpatialFilter", 0, 1, -1, 1, false, 20},
        {Command::TemporalFilter, "TemporalFilter", 0, 1, -1, 1, false, 21},
        {Command::LensType, "LensType", 1, 1, -1, 1, false, 21},
        {Command::ManualAlarm, "ManualAlarm", -1, 1, -1, 0, false, 22},
        {Command::AutofocusAfterZoom, "AutofocusAfterZoom", -1, 1, -1, 0, false, 22},
        {Command::FocusTemperatureInterval, "FocusTemperatureInterval", 1, 2, -1, 2, false, 23},
        {Command::Autofocus, "Autofocus", -1, 1, -1, 0, false, 23},
        {Command::FocusDrive, "FocusDrive", -1, 2, -1, 0, false, 24},
        {Command::FocusStop, "FocusStop", -1, 1, -1, 0, false, 24},
        {Command::FocusPosition, "FocusPosition", 1, -1, -1, 2, false, 24},
        {Command::FocusSpeed, "FocusSpeed", 1, 1, -1, 1, false, 25},
        {Command::FocusTravel, "FocusTravel", 1, -1, -1, 4, false, 25},
        {Command::ZoomDrive, "ZoomDrive", -1, 2, -1, 0, false, 26},
        {Command::ZoomStop, "ZoomStop", -1, 1, -1, 0, false, 26},
        {Command::ZoomPosition, "ZoomPosition", 1, -1, -1, 2, false, 26},
        {Command::ZoomSpeed, "ZoomSpeed", 1, 1, -1, 1, false, 27},
        {Command::ZoomTravel, "ZoomTravel", 1, -1, -1, 4, false, 27},
        {Command::Preset, "Preset", 2, 2, -1, 2, false, 28},
        {Command::CallPreset, "CallPreset", -1, 2, -1, 0, false, 28},
        {Command::TemperatureRange, "TemperatureRange", -1, 1, -1, 0, true, 29},
        {Command::TemperatureDisplay, "TemperatureDisplay", -1, 1, -1, 0, true, 29},
        {Command::TemperatureUnit, "TemperatureUnit", 1, 1, -1, 4, true, 30},
        {Command::ReflectedTemperature, "ReflectedTemperature", 1, 4, -1, 4, true, 30},
        {Command::AtmosphericTemperature, "AtmosphericTemperature", 1, 4, -1, 4, true, 31},
        {Command::Emissivity, "Emissivity", 1, 4, -1, 4, true, 32},
        {Command::TargetDistance, "TargetDistance", 1, 4, -1, 4, true, 32},
        {Command::Humidity, "Humidity", 1, 4, -1, 4, true, 33},
        {Command::ApplyEnvironment, "ApplyEnvironment", -1, 1, -1, 0, true, 33},
        {Command::PointTemperature, "PointTemperature", 4, -1, -1, 4, true, 33},
        {Command::CenterTemperatureDisplay, "CenterTemperatureDisplay", -1, 1, -1, 0, true, 34},
        {Command::CenterTemperature, "CenterTemperature", 1, -1, -1, 8, true, 34},
        {Command::FireAlarm, "FireAlarm", -1, 1, -1, 0, false, 34},
        {Command::FireThreshold, "FireThreshold", 0, 2, -1, 2, false, 35},
        {Command::TemperatureStretch, "TemperatureStretch", -1, 1, -1, 0, true, 36},
        {Command::StretchLow, "StretchLow", 0, 4, -1, 4, true, 36},
        {Command::StretchHigh, "StretchHigh", 0, 4, -1, 4, true, 37},
        {Command::TemperatureAlarm, "TemperatureAlarm", 0, 1, -1, 1, true, 37},
        {Command::Isotherm, "Isotherm", 0, 1, -1, 1, true, 38},
        {Command::AlarmLow, "AlarmLow", 0, 4, -1, 4, true, 39},
        {Command::AlarmHigh, "AlarmHigh", 0, 4, -1, 4, true, 39},
        {Command::LensTemperatureCorrection, "LensTemperatureCorrection", -1, 1, -1, 0, true, 40},
        {Command::LensCorrectionData, "LensCorrectionData", 0, -1, 1, 1, true, 40},
        {Command::EraseLensCorrection, "EraseLensCorrection", -1, -1, 1, 0, true, 41},
        {Command::CalibrateLensTemperature, "CalibrateLensTemperature", -1, -1, 2, 0, true, 41},
        {Command::StrongLightProtection, "StrongLightProtection", 1, 4, -1, 4, false, 42},
        {Command::BlindPixelCursor, "BlindPixelCursor", -1, -1, 1, 0, false, 43},
        {Command::BlindPixelMove, "BlindPixelMove", -1, -1, 1, 0, false, 43},
        {Command::BlindPixelEdit, "BlindPixelEdit", -1, 1, -1, 0, false, 44},
        {Command::SaveBlindPixels, "SaveBlindPixels", -1, -1, 0, 0, false, 44},
        {Command::LensKCalibration, "LensKCalibration", -1, 1, -1, 0, false, 44},
        {Command::ImageCompensation, "ImageCompensation", -1, 1, -1, 0, false, 45},
        {Command::Synchronization, "Synchronization", 1, 2, -1, 2, false, 45},
        {Command::FocalLength, "FocalLength", 1, -1, -1, 2, false, 47},
        {Command::SetFocalLength, "SetFocalLength", -1, 2, -1, 0, false, 47},
        {Command::FocalLengthDisplay, "FocalLengthDisplay", -1, 1, -1, 0, false, 47},
        {Command::FieldOfView, "FieldOfView", 1, -1, -1, 4, false, 48},
    };
    return catalog;
}

const CommandInfo* commandInfo(Command command)
{
    for (const auto& item : commandCatalog()) if (item.command == command) return &item;
    return nullptr;
}

std::uint8_t checksum(const std::uint8_t* data, std::size_t size)
{
    std::uint8_t sum = 0;
    if (data) for (std::size_t i = 0; i < size; ++i) sum = static_cast<std::uint8_t>(sum + data[i]);
    return sum;
}

Bytes encodeCommand(Command command, Operation operation, const Bytes& parameters)
{
    const auto* info = commandInfo(command);
    if (!info) throw std::invalid_argument("unknown Airui command");
    int expected = -1;
    switch (operation)
    {
    case Operation::Read: expected = info->readBytes; break;
    case Operation::Write: expected = info->writeBytes; break;
    case Operation::Action: expected = info->actionBytes; break;
    default: break;
    }
    if (expected < 0 || parameters.size() != static_cast<std::size_t>(expected))
        throw std::invalid_argument("unsupported Airui operation or parameter length");
    const auto code = static_cast<std::uint16_t>(command);
    Bytes packet{kCommandHead, static_cast<std::uint8_t>(4 + parameters.size()),
                 static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code),
                 static_cast<std::uint8_t>(operation)};
    packet.insert(packet.end(), parameters.begin(), parameters.end());
    packet.push_back(checksum(packet.data(), packet.size()));
    packet.insert(packet.end(), {0xEB, 0xAA});
    return packet;
}

Bytes littleEndian16(std::uint16_t value)
{
    return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)};
}
Bytes littleEndian32(std::uint32_t value)
{
    return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
}
std::uint16_t readLittleEndian16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t readLittleEndian32(const std::uint8_t* p)
{
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

namespace
{
bool knownSet(std::uint8_t group) { return group == 1 || group == 2 || group == 7 || group == 8; }
}

bool decodeFrame(const std::uint8_t* data, std::size_t size, Frame& out)
{
    if (!data || size < 7 || size > kMaxFrameBytes || size != static_cast<std::size_t>(data[1]) + 4 ||
        data[size-2] != 0xEB || data[size-1] != 0xAA || checksum(data,size-3) != data[size-3]) return false;
    Frame frame;
    std::size_t offset;
    if (data[0] == kCommandHead)
    {
        if (data[1] < 4 || !knownSet(data[2]) || data[4] > 2) return false;
        frame.commandSet = static_cast<CommandSet>(data[2]);
        frame.command = data[3]; frame.operation = data[4]; offset = 5;
    }
    else if (data[0] == kResponseHead)
    {
        frame.response = true;
        // 08/33 变焦位置的应答也是 55 xx 08 33 33...，必须先判断扩展格式。
        if (data[1] >= 4 && (data[2] == 7 || data[2] == 8) && data[4] == kResponseOperation)
        {
            frame.commandSet = static_cast<CommandSet>(data[2]);
            frame.command = data[3]; frame.operation = data[4]; offset = 5;
        }
        else
        {
            if (data[1] < 3 || data[3] != kResponseOperation) return false;
            frame.command = data[2]; frame.operation = data[3]; offset = 4;
        }
    }
    else return false;
    frame.parameters.assign(data+offset,data+size-3);
    frame.wire.assign(data,data+size);
    out = std::move(frame);
    return true;
}

std::vector<Frame> FrameParser::feed(const std::uint8_t* data, std::size_t size)
{
    std::vector<Frame> frames;
    if (!data) return frames;
    // 逐字节处理，上限为一个最长报文，不随噪音或粘包长度增长。
    for (std::size_t i=0; i<size; ++i)
    {
        buffer_.push_back(data[i]);
        for (;;)
        {
            const auto begin=std::find_if(buffer_.begin(),buffer_.end(),[](auto b) { return b==0xAA || b==0x55; });
            buffer_.erase(buffer_.begin(),begin);
            if (buffer_.size()<2) break;
            const bool request=buffer_[0]==kCommandHead;
            const auto total=static_cast<std::size_t>(buffer_[1])+4;
            bool invalid=buffer_[1]<(request?4:3);
            if (request && buffer_.size()>=3 && !knownSet(buffer_[2])) invalid=true;
            if (request && buffer_.size()>=5 && buffer_[4]>2) invalid=true;
            if (!request && buffer_.size()>=4 && buffer_[3]!=kResponseOperation)
            {
                if (buffer_[2]!=7 && buffer_[2]!=8) invalid=true;
                else if (buffer_.size()>=5 && buffer_[4]!=kResponseOperation) invalid=true;
            }
            if (invalid) { buffer_.erase(buffer_.begin()); continue; }
            if (buffer_.size()<total) break;
            Frame frame;
            if (decodeFrame(buffer_.data(),total,frame))
            {
                frames.push_back(std::move(frame));
                buffer_.erase(buffer_.begin(),buffer_.begin()+total);
            }
            else buffer_.erase(buffer_.begin());
        }
    }
    return frames;
}

bool decodeAlarmReport(const Frame& frame, AlarmReport& out)
{
    if (frame.response || frame.commandSet!=CommandSet::Common || frame.command!=8 ||
        frame.operation!=1 || frame.parameters.size()!=21) return false;
    AlarmReport report;
    if (frame.parameters[0]==1)
    {
        report.type=AlarmType::StrongLight;
        report.shutterClosed=frame.parameters[1]!=0;
    }
    else if (frame.parameters[0]==3)
    {
        report.type=AlarmType::Fire;
        for(std::size_t i=0;i<5;++i)
        {
            report.fireCenters[i].x=readLittleEndian16(frame.parameters.data()+1+i*4);
            report.fireCenters[i].y=readLittleEndian16(frame.parameters.data()+3+i*4);
        }
    }
    else return false;
    out=report;
    return true;
}

const char* errorString(Error error)
{
    switch(error)
    {
    case Error::None: return "success";
    case Error::NotConnected: return "serial port not connected";
    case Error::InvalidArgument: return "invalid command argument";
    case Error::UnsupportedDevice: return "command requires a radiometric core";
    case Error::WriteFailed: return "serial write failed";
    case Error::Timeout: return "response timed out";
    case Error::ConnectionLost: return "serial connection lost";
    case Error::DeviceRejected: return "device rejected command";
    case Error::ProtocolError: return "device reported a protocol error";
    case Error::MalformedResponse: return "unexpected response length or value";
    case Error::NeedsReconnect: return "response synchronization lost; reconnect required";
    }
    return "unknown error";
}
} // namespace airui

using airui::Bytes;
using airui::Command;
using airui::Operation;

struct AiruiCore::Impl
{
    std::string port;
    std::uint32_t baud;
    airui::DeviceType type;
    Urt uart;
    std::atomic<bool> connected{false};
    std::atomic<int> timeout{500};
    std::mutex transactionMutex, receiveMutex, callbackMutex, resultMutex;
    std::condition_variable ready;
    airui::FrameParser parser;
    std::optional<Command> pending;
    std::optional<airui::Frame> received;
    bool synchronized{true}; // receiveMutex 保护，超时后不自动重试无事务 ID 的命令
    airui::Result last;
    std::function<void(const airui::Frame&)> frameCallback;
    std::function<void(const airui::AlarmReport&)> alarmCallback;

    Impl(std::string path,std::uint32_t rate,airui::DeviceType device)
        : port(std::move(path)),baud(rate),type(device)
    {
        uart.set_error_callback([this](Urt::Error,int) {
            { std::lock_guard<std::mutex> lock(receiveMutex); connected.store(false); }
            ready.notify_all();
        });
    }
    airui::Result record(airui::Result result)
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        last=result;
        return result;
    }
    void receive(const std::uint8_t* data,std::size_t size)
    {
        std::vector<airui::Frame> unhandled;
        {
            std::lock_guard<std::mutex> lock(receiveMutex);
            for(auto& frame:parser.feed(data,size))
            {
                bool match=false;
                if(frame.response && pending && !received)
                {
                    const auto code=static_cast<std::uint16_t>(*pending);
                    const auto group=static_cast<airui::CommandSet>(code>>8);
                    const bool family=frame.commandSet ? *frame.commandSet==group :
                        (group==airui::CommandSet::Common || group==airui::CommandSet::ImageMode);
                    match=frame.command==0xFF || (family && frame.command==static_cast<std::uint8_t>(code));
                }
                if(match) { received=std::move(frame); ready.notify_all(); }
                else unhandled.push_back(std::move(frame));
            }
        }
        for(const auto& frame:unhandled)
        {
            std::function<void(const airui::Frame&)> onFrame;
            std::function<void(const airui::AlarmReport&)> onAlarm;
            {
                std::lock_guard<std::mutex> lock(callbackMutex);
                onFrame=frameCallback; onAlarm=alarmCallback;
            }
            try
            {
                airui::AlarmReport alarm;
                if(onAlarm && airui::decodeAlarmReport(frame,alarm)) onAlarm(alarm);
                if(onFrame) onFrame(frame);
            }
            catch(const std::exception& e) { UAV_LOG_ERROR << "Airui callback: " << e.what(); }
            catch(...) { UAV_LOG_ERROR << "Airui callback threw an unknown exception"; }
        }
    }
};

AiruiCore::AiruiCore(std::string port,std::uint32_t baud,airui::DeviceType type)
    : impl_(std::make_unique<Impl>(std::move(port),baud,type)) {}
AiruiCore::~AiruiCore() { disconnect(); }

bool AiruiCore::connect()
{
    { std::lock_guard<std::mutex> receive(impl_->receiveMutex); impl_->connected.store(false); }
    impl_->ready.notify_all();
    std::lock_guard<std::mutex> transaction(impl_->transactionMutex);
    impl_->uart.close();
    if(impl_->baud>static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
       impl_->uart.open(impl_->port,static_cast<int>(impl_->baud),8,'N',1,false,true)!=0)
    {
        impl_->record({airui::Error::NotConnected,std::nullopt,{}});
        return false;
    }
    {
        std::lock_guard<std::mutex> receive(impl_->receiveMutex);
        impl_->parser.reset(); impl_->pending.reset(); impl_->received.reset(); impl_->synchronized=true;
        impl_->connected.store(true);
    }
    impl_->uart.clear_error();
    if(impl_->uart.start_receive([this](const std::uint8_t* data,std::size_t size) { impl_->receive(data,size); })!=0)
    {
        impl_->connected.store(false); impl_->uart.close();
        impl_->record({airui::Error::NotConnected,std::nullopt,{}});
        return false;
    }
    impl_->record({});
    return true;
}
void AiruiCore::disconnect()
{
    // 和等待谓词共用锁，避免 notify 恰好发生在 wait 睡眠之前而丢失唤醒。
    { std::lock_guard<std::mutex> receive(impl_->receiveMutex); impl_->connected.store(false); }
    impl_->ready.notify_all();
    std::lock_guard<std::mutex> transaction(impl_->transactionMutex);
    impl_->uart.close();
    std::lock_guard<std::mutex> receive(impl_->receiveMutex);
    impl_->pending.reset(); impl_->received.reset(); impl_->parser.reset();
}
bool AiruiCore::reconnect() { disconnect(); return connect(); }
bool AiruiCore::isConnected() const { return impl_->connected.load() && impl_->uart.is_open(); }
bool AiruiCore::setResponseTimeout(int milliseconds)
{
    if(milliseconds<=0) return invalidArgument();
    impl_->timeout.store(milliseconds);
    return true;
}
airui::Result AiruiCore::lastResult() const
{
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    return impl_->last;
}
void AiruiCore::setFrameCallback(std::function<void(const airui::Frame&)> callback)
{
    std::lock_guard<std::mutex> lock(impl_->callbackMutex); impl_->frameCallback=std::move(callback);
}
void AiruiCore::setAlarmCallback(std::function<void(const airui::AlarmReport&)> callback)
{
    std::lock_guard<std::mutex> lock(impl_->callbackMutex); impl_->alarmCallback=std::move(callback);
}

airui::Result AiruiCore::transact(Command command,Operation operation,const Bytes& parameters,int timeoutMs)
{
    Bytes packet;
    const auto* info=airui::commandInfo(command);
    try { packet=airui::encodeCommand(command,operation,parameters); }
    catch(const std::invalid_argument&) { return impl_->record({airui::Error::InvalidArgument,std::nullopt,{}}); }
    if(timeoutMs==-1) timeoutMs=impl_->timeout.load();
    if(timeoutMs<=0) return impl_->record({airui::Error::InvalidArgument,std::nullopt,{}});
    if(info->radiometricOnly && impl_->type!=airui::DeviceType::Radiometric)
        return impl_->record({airui::Error::UnsupportedDevice,std::nullopt,{}});
    std::lock_guard<std::mutex> transaction(impl_->transactionMutex);
    {
        std::lock_guard<std::mutex> receive(impl_->receiveMutex);
        if(!impl_->connected.load()) return impl_->record({airui::Error::NotConnected,std::nullopt,{}});
        if(!impl_->synchronized) return impl_->record({airui::Error::NeedsReconnect,std::nullopt,{}});
        impl_->pending=command; impl_->received.reset();
    }
    if(impl_->uart.write(packet)!=static_cast<int>(packet.size()))
    {
        std::lock_guard<std::mutex> receive(impl_->receiveMutex);
        impl_->pending.reset(); impl_->synchronized=false;
        return impl_->record({airui::Error::WriteFailed,std::nullopt,{}});
    }
    std::unique_lock<std::mutex> receive(impl_->receiveMutex);
    const bool arrived=impl_->ready.wait_for(receive,std::chrono::milliseconds(timeoutMs),[this] {
        return impl_->received.has_value() || !impl_->connected.load();
    });
    impl_->pending.reset();
    if(!impl_->connected.load()) return impl_->record({airui::Error::ConnectionLost,std::nullopt,{}});
    if(!arrived)
    {
        impl_->synchronized=false;
        return impl_->record({airui::Error::Timeout,std::nullopt,{}});
    }
    airui::Result result; result.frame=std::move(*impl_->received); impl_->received.reset();
    const auto& values=result.frame.parameters;
    if(result.frame.command==0xFF)
    {
        result.error=airui::Error::ProtocolError;
        if(values.size()==1) result.deviceError=static_cast<airui::DeviceError>(values[0]);
    }
    else if(values.size()!=(operation==Operation::Read?info->responseBytes:1))
        result.error=airui::Error::MalformedResponse;
    else if(operation!=Operation::Read && values[0]!=1)
        result.error=values[0]==0?airui::Error::DeviceRejected:airui::Error::MalformedResponse;
    return impl_->record(std::move(result));
}

bool AiruiCore::invalidArgument()
{
    impl_->record({airui::Error::InvalidArgument,std::nullopt,{}}); return false;
}
bool AiruiCore::malformedResponse()
{
    auto result=lastResult(); result.error=airui::Error::MalformedResponse; impl_->record(std::move(result)); return false;
}
bool AiruiCore::write(Command command,const Bytes& parameters,Operation operation)
{
    return static_cast<bool>(transact(command,operation,parameters));
}
bool AiruiCore::read(Command command,Bytes& value,const Bytes& parameters)
{
    auto result=transact(command,Operation::Read,parameters);
    if(!result) return false;
    value=std::move(result.frame.parameters); return true;
}
bool AiruiCore::readByte(Command command,std::uint8_t& value,const Bytes& parameters)
{
    Bytes data; if(!read(command,data,parameters)) return false;
    value=data[0]; return true;
}
bool AiruiCore::readWord(Command command,std::uint16_t& value,const Bytes& parameters)
{
    Bytes data; if(!read(command,data,parameters)) return false;
    value=airui::readLittleEndian16(data.data()); return true;
}
bool AiruiCore::readScaled(Command command,double& value,double scale,unsigned bytes,bool signedValue,const Bytes& parameters)
{
    Bytes data; if(!read(command,data,parameters)) return false;
    const std::uint32_t raw=bytes==1?data[0]:(bytes==2?airui::readLittleEndian16(data.data()):airui::readLittleEndian32(data.data()));
    std::int64_t number=raw;
    if(signedValue && (raw & (1ULL<<(bytes*8-1)))) number-=1LL<<(bytes*8);
    value=number/scale; return true;
}
bool AiruiCore::writeScaled(Command command,double value,double scale,unsigned bytes,bool signedValue)
{
    if(!std::isfinite(value) || (!signedValue && value<0)) return invalidArgument();
    const double encoded=std::round(value*scale);
    const double min=signedValue?-std::ldexp(1.0,bytes*8-1):0;
    const double max=std::ldexp(1.0,bytes*8-(signedValue?1:0))-1;
    if(!std::isfinite(encoded) || encoded<min || encoded>max) return invalidArgument();
    const auto raw=static_cast<std::uint32_t>(static_cast<std::int64_t>(encoded));
    return write(command,bytes==1?Bytes{static_cast<std::uint8_t>(raw)}:
        (bytes==2?airui::littleEndian16(raw):airui::littleEndian32(raw)));
}

namespace
{
template<class T> std::uint8_t byte(T value) { return static_cast<std::uint8_t>(value); }
Bytes words(std::initializer_list<std::uint16_t> values)
{
    Bytes data;
    for (const auto value : values)
    {
        const auto part=airui::littleEndian16(value);
        data.insert(data.end(),part.begin(),part.end());
    }
    return data;
}
bool validSource(airui::VideoSource source)
{
    return source==airui::VideoSource::Original || source==airui::VideoSource::Nuc ||
           source==airui::VideoSource::Drc || source==airui::VideoSource::Dns;
}
}

bool AiruiCore::getSerialNumber(std::string& value)
{
    Bytes data; if(!read(Command::SerialNumber,data)) return false;
    value.assign(data.begin(),std::find(data.begin(),data.end(),0)); return true;
}
bool AiruiCore::getSensorWidth(std::uint16_t& value) { return readWord(Command::SensorWidth,value); }
bool AiruiCore::getSensorHeight(std::uint16_t& value) { return readWord(Command::SensorHeight,value); }
bool AiruiCore::backgroundCorrection() { return write(Command::Correction,{0,2},Operation::Action); }
bool AiruiCore::shutterCorrection() { return write(Command::Correction,{1,1},Operation::Action); }
bool AiruiCore::setShutterTimeInterval(std::uint8_t value) { return write(Command::ShutterTimeInterval,{value}); }
bool AiruiCore::getShutterTimeInterval(std::uint8_t& value) { return readByte(Command::ShutterTimeInterval,value); }
bool AiruiCore::setShutterFpaInterval(double value) { return writeScaled(Command::ShutterFpaInterval,value,10,1); }
bool AiruiCore::getShutterFpaInterval(double& value) { return readScaled(Command::ShutterFpaInterval,value,10,1,false); }
bool AiruiCore::setShutterCoreInterval(double value) { return writeScaled(Command::ShutterCoreInterval,value,10,1); }
bool AiruiCore::getShutterCoreInterval(double& value) { return readScaled(Command::ShutterCoreInterval,value,10,1,false); }
bool AiruiCore::getCoreTemperature(double& value) { return readScaled(Command::CoreTemperature,value,100,2,true); }
bool AiruiCore::getFpaTemperature(double& value) { return readScaled(Command::FpaTemperature,value,100,2,true); }
bool AiruiCore::saveSettings() { return write(Command::SaveSettings,{},Operation::Action); }
bool AiruiCore::factoryReset() { return write(Command::FactoryReset,{0},Operation::Action); }

bool AiruiCore::setDigitalZoom(const airui::Rectangle& region)
{
    if(region.left>region.right || region.top>region.bottom) return invalidArgument();
    return write(Command::DigitalZoom,words({region.left,region.top,region.right,region.bottom}),Operation::Action);
}
bool AiruiCore::setDigitalZoom(double ratio,std::uint16_t width,std::uint16_t height)
{
    if(!std::isfinite(ratio) || ratio<1 || ratio>8 || width==0 || height==0 ||
       std::abs(ratio*10-std::round(ratio*10))>1e-6) return invalidArgument();
    airui::Rectangle region;
    region.left=static_cast<std::uint16_t>(std::round(width/2.0-width/(2*ratio)));
    region.top=static_cast<std::uint16_t>(std::round(height/2.0-height/(2*ratio)));
    const double right=std::floor(width/2.0+width/(2*ratio)-1);
    const double bottom=std::floor(height/2.0+height/(2*ratio)-1);
    if(right<0 || bottom<0) return invalidArgument();
    region.right=static_cast<std::uint16_t>(right); region.bottom=static_cast<std::uint16_t>(bottom);
    return setDigitalZoom(region);
}
bool AiruiCore::setImageFlip(airui::ImageFlip mode)
{
    const auto value=byte(mode);
    if(value!=1 && value!=2 && value!=4 && value!=8) return invalidArgument();
    return write(Command::ImageFlip,{value});
}
bool AiruiCore::setWarningThreshold(airui::WarningColor color,std::uint8_t threshold)
{
    if(byte(color)>2) return invalidArgument();
    return write(Command::WarningThreshold,{threshold,byte(color)});
}
bool AiruiCore::setDigitalVideo(airui::DigitalVideo interface)
{
    switch(interface)
    {
    case airui::DigitalVideo::Off: case airui::DigitalVideo::LvCmos: case airui::DigitalVideo::Bt1120:
    case airui::DigitalVideo::Bt601: case airui::DigitalVideo::Cds2: case airui::DigitalVideo::Cds3:
        return write(Command::DigitalVideo,airui::littleEndian16(static_cast<std::uint16_t>(interface)),Operation::Action);
    }
    return invalidArgument();
}
bool AiruiCore::setVideoSource(airui::VideoSource parallel,airui::VideoSource serial)
{
    if(!validSource(parallel) || !validSource(serial)) return invalidArgument();
    return write(Command::VideoSource,{static_cast<std::uint8_t>(byte(parallel)|(byte(serial)<<4))});
}
bool AiruiCore::setBrightness(std::uint8_t value)
{
    if(value>100) return invalidArgument();
    return write(Command::Brightness,{value});
}
bool AiruiCore::getBrightness(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::Brightness,decoded,{})) return false;
    if(decoded>100) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setContrast(std::uint8_t value)
{
    if(value>100) return invalidArgument();
    return write(Command::Contrast,{value});
}
bool AiruiCore::getContrast(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::Contrast,decoded,{})) return false;
    if(decoded>100) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setDetail(std::uint8_t value)
{
    if(value>100) return invalidArgument();
    return write(Command::Detail,{value});
}
bool AiruiCore::getDetail(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::Detail,decoded,{})) return false;
    if(decoded>100) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setSpatialFilter(std::uint8_t value)
{
    if(value>100) return invalidArgument();
    return write(Command::SpatialFilter,{value});
}
bool AiruiCore::getSpatialFilter(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::SpatialFilter,decoded,{})) return false;
    if(decoded>100) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setTemporalFilter(std::uint8_t value)
{
    if(value>100) return invalidArgument();
    return write(Command::TemporalFilter,{value});
}
bool AiruiCore::getTemporalFilter(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::TemporalFilter,decoded,{})) return false;
    if(decoded>100) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setFocusSpeed(std::uint8_t value)
{
    if(value>32) return invalidArgument();
    return write(Command::FocusSpeed,{value});
}
bool AiruiCore::getFocusSpeed(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::FocusSpeed,decoded,{0})) return false;
    if(decoded>32) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setZoomSpeed(std::uint8_t value)
{
    if(value>32) return invalidArgument();
    return write(Command::ZoomSpeed,{value});
}
bool AiruiCore::getZoomSpeed(std::uint8_t& value)
{
    std::uint8_t decoded; if(!readByte(Command::ZoomSpeed,decoded,{0})) return false;
    if(decoded>32) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setPalette(airui::Palette value)
{
    if(byte(value)>19) return invalidArgument();
    return write(Command::Palette,{byte(value)},Operation::Action);
}
bool AiruiCore::getPalette(airui::Palette& value)
{
    std::uint8_t decoded; if(!readByte(Command::Palette,decoded,{0})) return false;
    if(decoded>19) return malformedResponse();
    value=static_cast<airui::Palette>(decoded); return true;
}
bool AiruiCore::setImageMode(airui::ImageMode value)
{
    if(byte(value)>4) return invalidArgument();
    return write(Command::ImageMode,{byte(value)},Operation::Write);
}
bool AiruiCore::getImageMode(airui::ImageMode& value)
{
    std::uint8_t decoded; if(!readByte(Command::ImageMode,decoded,{})) return false;
    if(decoded>4) return malformedResponse();
    value=static_cast<airui::ImageMode>(decoded); return true;
}
bool AiruiCore::setLensType(airui::LensType value)
{
    if(byte(value)>3) return invalidArgument();
    return write(Command::LensType,{byte(value)},Operation::Write);
}
bool AiruiCore::getLensType(airui::LensType& value)
{
    std::uint8_t decoded; if(!readByte(Command::LensType,decoded,{0})) return false;
    if(decoded>3) return malformedResponse();
    value=static_cast<airui::LensType>(decoded); return true;
}
bool AiruiCore::setTemperatureAlarm(airui::TemperatureAlarm value)
{
    if(byte(value)>3) return invalidArgument();
    return write(Command::TemperatureAlarm,{byte(value)},Operation::Write);
}
bool AiruiCore::getTemperatureAlarm(airui::TemperatureAlarm& value)
{
    std::uint8_t decoded; if(!readByte(Command::TemperatureAlarm,decoded,{})) return false;
    if(decoded>3) return malformedResponse();
    value=static_cast<airui::TemperatureAlarm>(decoded); return true;
}
bool AiruiCore::setAutoShutter(bool enabled) { return write(Command::AutoShutter,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setAnalogVideo(bool enabled) { return write(Command::AnalogVideo,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Action); }
bool AiruiCore::freezeImage(bool enabled) { return write(Command::Freeze,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Action); }
bool AiruiCore::setBootLogo(bool enabled) { return write(Command::BootLogo,{static_cast<std::uint8_t>(enabled?128:0)},Operation::Action); }
bool AiruiCore::setManualAlarm(bool enabled) { return write(Command::ManualAlarm,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setAutofocusAfterZoom(bool enabled) { return write(Command::AutofocusAfterZoom,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setFocalLengthDisplay(bool enabled) { return write(Command::FocalLengthDisplay,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setTemperatureDisplay(bool enabled) { return write(Command::TemperatureDisplay,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setCenterTemperatureDisplay(bool enabled) { return write(Command::CenterTemperatureDisplay,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setFireAlarm(bool enabled) { return write(Command::FireAlarm,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setTemperatureStretch(bool enabled) { return write(Command::TemperatureStretch,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setIsotherm(bool enabled) { return write(Command::Isotherm,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::setLensTemperatureCorrection(bool enabled) { return write(Command::LensTemperatureCorrection,{static_cast<std::uint8_t>(enabled?1:0)},Operation::Write); }
bool AiruiCore::showBlindPixelCursor(bool enabled) { return write(Command::BlindPixelCursor,{static_cast<std::uint8_t>(enabled?193:64)},Operation::Action); }
bool AiruiCore::autofocus() { return write(Command::Autofocus,{0}); }
bool AiruiCore::stopFocus() { return write(Command::FocusStop,{0}); }
bool AiruiCore::stopZoom() { return write(Command::ZoomStop,{0}); }
bool AiruiCore::applyEnvironment() { return write(Command::ApplyEnvironment,{0}); }

bool AiruiCore::setFocusTemperatureInterval(double value) { return writeScaled(Command::FocusTemperatureInterval,value,100,2); }
bool AiruiCore::getFocusTemperatureInterval(double& value) { return readScaled(Command::FocusTemperatureInterval,value,100,2,false,{0}); }
bool AiruiCore::driveFocus(airui::FocusDirection direction,airui::Adjustment adjustment)
{
    if((byte(direction)!=1 && byte(direction)!=2) || byte(adjustment)>1) return invalidArgument();
    return write(Command::FocusDrive,{byte(direction),byte(adjustment)});
}
bool AiruiCore::driveZoom(airui::ZoomDirection direction,airui::Adjustment adjustment)
{
    if((byte(direction)!=1 && byte(direction)!=2) || byte(adjustment)>1) return invalidArgument();
    return write(Command::ZoomDrive,{byte(direction),byte(adjustment)});
}
bool AiruiCore::getFocusPosition(std::uint16_t& value) { return readWord(Command::FocusPosition,value,{0}); }
bool AiruiCore::getZoomPosition(std::uint16_t& value) { return readWord(Command::ZoomPosition,value,{0}); }
bool AiruiCore::getFocusTravel(airui::MotorTravel& value)
{
    Bytes data; if(!read(Command::FocusTravel,data,{0})) return false;
    airui::MotorTravel decoded{airui::readLittleEndian16(data.data()),airui::readLittleEndian16(data.data()+2)};
    if(decoded.minimum>decoded.maximum) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::getZoomTravel(airui::MotorTravel& value)
{
    Bytes data; if(!read(Command::ZoomTravel,data,{0})) return false;
    airui::MotorTravel decoded{airui::readLittleEndian16(data.data()),airui::readLittleEndian16(data.data()+2)};
    if(decoded.minimum>decoded.maximum) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::writePreset(airui::Motor motor,std::uint8_t index)
{
    if(byte(motor)>1 || index>9) return invalidArgument();
    return write(Command::Preset,{byte(motor),index});
}
bool AiruiCore::readPreset(airui::Motor motor,std::uint8_t index,std::uint16_t& value)
{
    if(byte(motor)>1 || index>9) return invalidArgument();
    return readWord(Command::Preset,value,{byte(motor),index});
}
bool AiruiCore::callPreset(airui::Motor motor,std::uint8_t index)
{
    if(byte(motor)>1 || index>9) return invalidArgument();
    return write(Command::CallPreset,{byte(motor),index});
}
bool AiruiCore::getFocalLength(double& value) { return readScaled(Command::FocalLength,value,10,2,false,{0}); }
bool AiruiCore::setFocalLength(double value) { return writeScaled(Command::SetFocalLength,value,10,2); }
bool AiruiCore::getFieldOfView(airui::Axis axis,float& value)
{
    if(byte(axis)>1) return invalidArgument();
    Bytes data; if(!read(Command::FieldOfView,data,{byte(axis)})) return false;
    const std::uint32_t raw=airui::readLittleEndian32(data.data());
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559,"IEEE-754 float32 required");
    float decoded; std::memcpy(&decoded,&raw,sizeof(decoded));
    if(!std::isfinite(decoded) || decoded<0 || decoded>180) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::setTemperatureRange(airui::TemperatureRange value)
{
    if(byte(value)!=0 && byte(value)!=1 && byte(value)!=3) return invalidArgument();
    return write(Command::TemperatureRange,{byte(value)});
}
bool AiruiCore::getTemperatureUnit(airui::TemperatureUnit& value)
{
    Bytes data; if(!read(Command::TemperatureUnit,data,{0})) return false;
    const auto raw=airui::readLittleEndian32(data.data());
    if(raw>2) return malformedResponse();
    value=static_cast<airui::TemperatureUnit>(raw); return true;
}
bool AiruiCore::setTemperatureUnit(airui::TemperatureUnit value)
{
    if(byte(value)>2) return invalidArgument();
    return write(Command::TemperatureUnit,{byte(value)});
}
bool AiruiCore::getReflectedTemperature(double& value) { return readScaled(Command::ReflectedTemperature,value,10000,4,true,{0}); }
bool AiruiCore::setReflectedTemperature(double value)
{
    return writeScaled(Command::ReflectedTemperature,value,10000,4,true);
}
bool AiruiCore::getAtmosphericTemperature(double& value) { return readScaled(Command::AtmosphericTemperature,value,10000,4,true,{0}); }
bool AiruiCore::setAtmosphericTemperature(double value)
{
    return writeScaled(Command::AtmosphericTemperature,value,10000,4,true);
}
bool AiruiCore::getEmissivity(double& value) { return readScaled(Command::Emissivity,value,10000,4,false,{0}); }
bool AiruiCore::setEmissivity(double value)
{
    if(!std::isfinite(value) || value<0 || value>1) return invalidArgument();
    return writeScaled(Command::Emissivity,value,10000,4,false);
}
bool AiruiCore::getTargetDistance(double& value) { return readScaled(Command::TargetDistance,value,10000,4,false,{0}); }
bool AiruiCore::setTargetDistance(double value)
{
    return writeScaled(Command::TargetDistance,value,10000,4,false);
}
bool AiruiCore::getHumidity(double& value) { return readScaled(Command::Humidity,value,10000,4,false,{0}); }
bool AiruiCore::setHumidity(double value)
{
    if(!std::isfinite(value) || value<0 || value>1) return invalidArgument();
    return writeScaled(Command::Humidity,value,10000,4,false);
}
bool AiruiCore::getStretchLow(double& value) { return readScaled(Command::StretchLow,value,10000,4,true,{}); }
bool AiruiCore::setStretchLow(double value)
{
    return writeScaled(Command::StretchLow,value,10000,4,true);
}
bool AiruiCore::getStretchHigh(double& value) { return readScaled(Command::StretchHigh,value,10000,4,true,{}); }
bool AiruiCore::setStretchHigh(double value)
{
    return writeScaled(Command::StretchHigh,value,10000,4,true);
}
bool AiruiCore::getAlarmLow(double& value) { return readScaled(Command::AlarmLow,value,10,4,true,{}); }
bool AiruiCore::setAlarmLow(double value)
{
    return writeScaled(Command::AlarmLow,value,10,4,true);
}
bool AiruiCore::getAlarmHigh(double& value) { return readScaled(Command::AlarmHigh,value,10,4,true,{}); }
bool AiruiCore::setAlarmHigh(double value)
{
    return writeScaled(Command::AlarmHigh,value,10,4,true);
}
bool AiruiCore::getPointTemperature(airui::Point point,double& value)
{
    return readScaled(Command::PointTemperature,value,10,4,true,words({point.x,point.y}));
}
bool AiruiCore::getCenterTemperature(airui::CenterTemperature& value)
{
    Bytes data; if(!read(Command::CenterTemperature,data,{0})) return false;
    const auto raw=airui::readLittleEndian32(data.data());
    const std::int64_t signedRaw=raw>=0x80000000u?static_cast<std::int64_t>(raw)-0x100000000LL:raw;
    airui::CenterTemperature decoded;
    decoded.temperature=signedRaw/10.0;
    decoded.position={airui::readLittleEndian16(data.data()+4),airui::readLittleEndian16(data.data()+6)};
    value=decoded; return true;
}
bool AiruiCore::setFireThreshold(std::uint16_t value)
{
    if(value<1 || value>16383) return invalidArgument();
    return write(Command::FireThreshold,airui::littleEndian16(value));
}
bool AiruiCore::getFireThreshold(std::uint16_t& value)
{
    std::uint16_t decoded; if(!readWord(Command::FireThreshold,decoded)) return false;
    if(decoded<1 || decoded>16383) return malformedResponse();
    value=decoded; return true;
}
bool AiruiCore::getIsotherm(bool& value)
{
    std::uint8_t decoded; if(!readByte(Command::Isotherm,decoded)) return false;
    if(decoded>1) return malformedResponse();
    value=decoded!=0; return true;
}
bool AiruiCore::hasLensCorrectionData(bool& value)
{
    std::uint8_t decoded; if(!readByte(Command::LensCorrectionData,decoded)) return false;
    if(decoded>1) return malformedResponse();
    value=decoded!=0; return true;
}
bool AiruiCore::eraseLensCorrection() { return write(Command::EraseLensCorrection,{0},Operation::Action); }
bool AiruiCore::calibrateLensTemperature(std::int16_t value)
{
    return write(Command::CalibrateLensTemperature,airui::littleEndian16(static_cast<std::uint16_t>(value)),Operation::Action);
}
bool AiruiCore::saveLensCorrection() { return write(Command::LensCorrectionData,{0},Operation::Action); }
bool AiruiCore::getStrongLightProtection(airui::StrongLightProtection& value)
{
    Bytes data; if(!read(Command::StrongLightProtection,data,{0})) return false;
    if(data[0]>1) return malformedResponse();
    value={data[0]!=0,airui::readLittleEndian16(data.data()+1),data[3]}; return true;
}
bool AiruiCore::setStrongLightProtection(const airui::StrongLightProtection& value)
{
    Bytes data{static_cast<std::uint8_t>(value.enabled)};
    const auto threshold=airui::littleEndian16(value.threshold);
    data.insert(data.end(),threshold.begin(),threshold.end()); data.push_back(value.seconds);
    return write(Command::StrongLightProtection,data);
}
bool AiruiCore::moveBlindPixelCursor(airui::CursorMove move)
{
    const auto value=byte(move);
    if(!((value>=1 && value<=4) || (value>=0x81 && value<=0x84))) return invalidArgument();
    return write(Command::BlindPixelMove,{value},Operation::Action);
}
bool AiruiCore::editBlindPixel(airui::BlindPixelEdit action)
{
    if(byte(action)!=1 && byte(action)!=2) return invalidArgument();
    return write(Command::BlindPixelEdit,{byte(action)});
}
bool AiruiCore::saveBlindPixels() { return write(Command::SaveBlindPixels,{},Operation::Action); }
bool AiruiCore::calibrateLensK(airui::LensKAction action)
{
    if(byte(action)<0x0A || byte(action)>0x0E) return invalidArgument();
    return write(Command::LensKCalibration,{byte(action)});
}
bool AiruiCore::compensateImage(airui::CompensationAction action)
{
    if(byte(action)>2) return invalidArgument();
    return write(Command::ImageCompensation,{byte(action)});
}
bool AiruiCore::getSynchronization(airui::Synchronization& value)
{
    Bytes data; if(!read(Command::Synchronization,data,{1})) return false;
    if(data[0]>3 || data[1]<25 || data[1]>50) return malformedResponse();
    value={static_cast<airui::SyncMode>(data[0]),data[1]}; return true;
}
bool AiruiCore::setSynchronization(const airui::Synchronization& value)
{
    if(byte(value.mode)>3 || value.frequencyHz<25 || value.frequencyHz>50) return invalidArgument();
    return write(Command::Synchronization,{byte(value.mode),value.frequencyHz});
}
