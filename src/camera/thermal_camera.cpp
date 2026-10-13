#include"camera/thermal_camera.h"

#include<algorithm>
#include<cmath>
#include<limits>
#include<stdexcept>

namespace
{
bool validConfig(const ThermalCamera::Config& config)
{
    return !config.port.empty() && config.baud>0 &&
        config.baud<=static_cast<std::uint32_t>(std::numeric_limits<int>::max()) &&
        config.responseTimeoutMs>0 &&
        (config.type==airui::DeviceType::Imaging || config.type==airui::DeviceType::Radiometric);
}
bool fitsScaled(double value, double scale, double minimum, double maximum)
{
    const auto encoded=std::round(value*scale);
    return std::isfinite(value) && std::isfinite(encoded) && encoded>=minimum && encoded<=maximum;
}
bool validEnvironment(const ThermalCamera::Environment& value)
{
    return fitsScaled(value.reflectedTemperature,10000,std::numeric_limits<std::int32_t>::min(),
                      std::numeric_limits<std::int32_t>::max()) &&
        fitsScaled(value.atmosphericTemperature,10000,std::numeric_limits<std::int32_t>::min(),
                   std::numeric_limits<std::int32_t>::max()) &&
        std::isfinite(value.emissivity) && value.emissivity>=0 && value.emissivity<=1 &&
        std::isfinite(value.humidity) && value.humidity>=0 && value.humidity<=1 &&
        value.targetDistanceMeters>=0 && fitsScaled(value.targetDistanceMeters,10000,0,
                                                   std::numeric_limits<std::uint32_t>::max());
}
std::uint8_t motorSpeed(std::uint8_t percent)
{
    return static_cast<std::uint8_t>(std::max(1,(percent*32+50)/100));
}
bool validAdjustment(airui::Adjustment value)
{
    return value==airui::Adjustment::Coarse || value==airui::Adjustment::Fine;
}
}

ThermalCamera::ThermalCamera(const std::string& port, std::uint32_t baud, airui::DeviceType type)
    : ThermalCamera(Config{port,baud,type,500}) {}

ThermalCamera::ThermalCamera(const Config& config) : config_(config)
{
    if(!validConfig(config)) throw std::invalid_argument("invalid thermal camera configuration");
    camera_=std::make_unique<AiruiCore>(config.port,config.baud,config.type);
    camera_->setResponseTimeout(config.responseTimeoutMs);
}
ThermalCamera::~ThermalCamera() { disconnect(); }

bool ThermalCamera::connect()
{
    // 先取消在途等待，再拿业务锁；否则长超时命令会阻塞生命周期操作。
    camera_->disconnect();
    std::lock_guard<std::mutex> lock(commandMutex_);
    const bool success=camera_->connect();
    completeLocked(success);
    return success;
}
void ThermalCamera::disconnect()
{
    camera_->disconnect();
    std::lock_guard<std::mutex> lock(commandMutex_);
    failLocked(airui::Error::NotConnected);
}
bool ThermalCamera::reconnect() { return connect(); }
bool ThermalCamera::isConnected() const
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    return camera_->isConnected();
}
bool ThermalCamera::init(const Config& config)
{
    if(!validConfig(config))
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        failLocked(airui::Error::InvalidArgument);
        return false;
    }
    auto replacement=std::make_unique<AiruiCore>(config.port,config.baud,config.type);
    replacement->setResponseTimeout(config.responseTimeoutMs);
    camera_->disconnect();
    std::lock_guard<std::mutex> lock(commandMutex_);
    replacement->setAlarmCallback(reportCallback_);
    replacement->setFrameCallback(frameCallback_);
    camera_=std::move(replacement);
    config_=config;
    last_={};
    return true;
}
bool ThermalCamera::init(const std::string& port, std::uint32_t baud)
{
    auto next=config();
    next.port=port; next.baud=baud;
    return init(next);
}
ThermalCamera::Config ThermalCamera::config() const
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    return config_;
}
bool ThermalCamera::setResponseTimeout(int milliseconds)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(!camera_->setResponseTimeout(milliseconds)) { completeLocked(false); return false; }
    config_.responseTimeoutMs=milliseconds;
    last_={};
    return true;
}
airui::Result ThermalCamera::lastResult() const
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    return last_;
}
void ThermalCamera::setReportCallback(ReportCallback callback)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    reportCallback_=std::move(callback);
    camera_->setAlarmCallback(reportCallback_);
}
void ThermalCamera::setFrameCallback(FrameCallback callback)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    frameCallback_=std::move(callback);
    camera_->setFrameCallback(frameCallback_);
}
std::int32_t ThermalCamera::completeLocked(bool success)
{
    last_=camera_->lastResult();
    return success?1:0;
}
std::int32_t ThermalCamera::failLocked(airui::Error error)
{
    last_={error,std::nullopt,{}};
    return 0;
}

std::int32_t ThermalCamera::continuousZoom(std::uint8_t direction, std::uint8_t speed,
                                         airui::Adjustment adjustment)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(direction==ZOOM_DIR_STOP) return completeLocked(camera_->stopZoom());
    if((direction!=ZOOM_DIR_IN && direction!=ZOOM_DIR_OUT) || speed>100 || !validAdjustment(adjustment))
        return failLocked(airui::Error::InvalidArgument);
    if(speed && !camera_->setZoomSpeed(motorSpeed(speed))) return completeLocked(false);
    return completeLocked(camera_->driveZoom(direction==ZOOM_DIR_IN?airui::ZoomDirection::Tele:
                                            airui::ZoomDirection::Wide,adjustment));
}
std::int32_t ThermalCamera::continuousFocus(std::uint8_t direction, std::uint8_t speed,
                                          airui::Adjustment adjustment)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(direction==FOCUS_DIR_STOP) return completeLocked(camera_->stopFocus());
    if((direction!=FOCUS_DIR_NEAR && direction!=FOCUS_DIR_FAR) || speed>100 || !validAdjustment(adjustment))
        return failLocked(airui::Error::InvalidArgument);
    if(speed && !camera_->setFocusSpeed(motorSpeed(speed))) return completeLocked(false);
    return completeLocked(camera_->driveFocus(direction==FOCUS_DIR_NEAR?airui::FocusDirection::Near:
                                             airui::FocusDirection::Far,adjustment));
}
std::int32_t ThermalCamera::setImageFlip(std::uint8_t mode)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(mode>FLIP_BOTH) return failLocked(airui::Error::InvalidArgument);
    constexpr airui::ImageFlip modes[]={airui::ImageFlip::None,airui::ImageFlip::Horizontal,
                                       airui::ImageFlip::Vertical,airui::ImageFlip::Diagonal};
    return completeLocked(camera_->setImageFlip(modes[mode]));
}
std::int32_t ThermalCamera::setPseudoColor(std::uint8_t color)
{
    return setPalette(static_cast<airui::Palette>(color));
}
std::int32_t ThermalCamera::getPseudoColor(std::uint8_t& color)
{
    return call([&](AiruiCore& camera) {
        airui::Palette palette;
        if(!camera.getPalette(palette)) return false;
        color=static_cast<std::uint8_t>(palette);
        return true;
    });
}
std::int32_t ThermalCamera::setNoiseReduction(std::uint8_t mode, std::uint8_t level)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(mode>1 || level>100) return failLocked(airui::Error::InvalidArgument);
    return completeLocked(mode==0?camera_->setSpatialFilter(level):camera_->setTemporalFilter(level));
}
std::int32_t ThermalCamera::setVideoFreeze(bool freeze) { return freezeImage(freeze); }

bool ThermalCamera::querySensorSizeLocked(SensorSize& size)
{
    SensorSize value;
    if(!camera_->getSensorWidth(value.width) || !camera_->getSensorHeight(value.height))
    {
        completeLocked(false);
        return false;
    }
    if(value.width==0 || value.height==0)
    {
        failLocked(airui::Error::MalformedResponse);
        return false;
    }
    size=value;
    completeLocked(true);
    return true;
}
std::int32_t ThermalCamera::getSensorSize(SensorSize& size)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    return querySensorSizeLocked(size)?1:0;
}
std::int32_t ThermalCamera::setDigitalZoom(double ratio)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(!std::isfinite(ratio) || ratio<1 || ratio>8 ||
       std::abs(ratio*10-std::round(ratio*10))>1e-6)
        return failLocked(airui::Error::InvalidArgument);
    SensorSize size;
    if(!querySensorSizeLocked(size)) return 0;
    return completeLocked(camera_->setDigitalZoom(ratio,size.width,size.height));
}
std::int32_t ThermalCamera::setDigitalZoom(const airui::Rectangle& region)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(region.left>region.right || region.top>region.bottom) return failLocked(airui::Error::InvalidArgument);
    SensorSize size;
    if(!querySensorSizeLocked(size)) return 0;
    if(region.right>=size.width || region.bottom>=size.height) return failLocked(airui::Error::InvalidArgument);
    return completeLocked(camera_->setDigitalZoom(region));
}
std::int32_t ThermalCamera::getPointTemperature(airui::Point point, double& value)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(config_.type!=airui::DeviceType::Radiometric) return failLocked(airui::Error::UnsupportedDevice);
    SensorSize size;
    if(!querySensorSizeLocked(size)) return 0;
    if(point.x>=size.width || point.y>=size.height) return failLocked(airui::Error::InvalidArgument);
    return completeLocked(camera_->getPointTemperature(point,value));
}
std::int32_t ThermalCamera::getEnvironment(Environment& value)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    Environment next;
    if(!camera_->getReflectedTemperature(next.reflectedTemperature) ||
       !camera_->getAtmosphericTemperature(next.atmosphericTemperature) ||
       !camera_->getEmissivity(next.emissivity) || !camera_->getTargetDistance(next.targetDistanceMeters) ||
       !camera_->getHumidity(next.humidity)) return completeLocked(false);
    if(!validEnvironment(next)) return failLocked(airui::Error::MalformedResponse);
    value=next;
    return completeLocked(true);
}
std::int32_t ThermalCamera::setEnvironment(const Environment& value)
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    if(config_.type!=airui::DeviceType::Radiometric) return failLocked(airui::Error::UnsupportedDevice);
    if(!validEnvironment(value)) return failLocked(airui::Error::InvalidArgument);
    return completeLocked(camera_->setReflectedTemperature(value.reflectedTemperature) &&
        camera_->setAtmosphericTemperature(value.atmosphericTemperature) && camera_->setEmissivity(value.emissivity) &&
        camera_->setTargetDistance(value.targetDistanceMeters) && camera_->setHumidity(value.humidity) &&
        camera_->applyEnvironment());
}

std::int32_t ThermalCamera::getSerialNumber(std::string& value)
{
    return call([&](AiruiCore& camera) { return camera.getSerialNumber(value); });
}
std::int32_t ThermalCamera::getSensorWidth(std::uint16_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getSensorWidth(value); });
}
std::int32_t ThermalCamera::getSensorHeight(std::uint16_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getSensorHeight(value); });
}
std::int32_t ThermalCamera::backgroundCorrection()
{
    return call([&](AiruiCore& camera) { return camera.backgroundCorrection(); });
}
std::int32_t ThermalCamera::shutterCorrection()
{
    return call([&](AiruiCore& camera) { return camera.shutterCorrection(); });
}
std::int32_t ThermalCamera::setAutoShutter(bool automatic)
{
    return call([&](AiruiCore& camera) { return camera.setAutoShutter(automatic); });
}
std::int32_t ThermalCamera::setShutterTimeInterval(std::uint8_t minutes)
{
    return call([&](AiruiCore& camera) { return camera.setShutterTimeInterval(minutes); });
}
std::int32_t ThermalCamera::getShutterTimeInterval(std::uint8_t& minutes)
{
    return call([&](AiruiCore& camera) { return camera.getShutterTimeInterval(minutes); });
}
std::int32_t ThermalCamera::setShutterFpaInterval(double celsius)
{
    return call([&](AiruiCore& camera) { return camera.setShutterFpaInterval(celsius); });
}
std::int32_t ThermalCamera::getShutterFpaInterval(double& celsius)
{
    return call([&](AiruiCore& camera) { return camera.getShutterFpaInterval(celsius); });
}
std::int32_t ThermalCamera::setShutterCoreInterval(double celsius)
{
    return call([&](AiruiCore& camera) { return camera.setShutterCoreInterval(celsius); });
}
std::int32_t ThermalCamera::getShutterCoreInterval(double& celsius)
{
    return call([&](AiruiCore& camera) { return camera.getShutterCoreInterval(celsius); });
}
std::int32_t ThermalCamera::getCoreTemperature(double& celsius)
{
    return call([&](AiruiCore& camera) { return camera.getCoreTemperature(celsius); });
}
std::int32_t ThermalCamera::getFpaTemperature(double& celsius)
{
    return call([&](AiruiCore& camera) { return camera.getFpaTemperature(celsius); });
}
std::int32_t ThermalCamera::saveSettings()
{
    return call([&](AiruiCore& camera) { return camera.saveSettings(); });
}
std::int32_t ThermalCamera::factoryReset()
{
    return call([&](AiruiCore& camera) { return camera.factoryReset(); });
}
std::int32_t ThermalCamera::setAnalogVideo(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setAnalogVideo(enabled); });
}
std::int32_t ThermalCamera::freezeImage(bool freeze)
{
    return call([&](AiruiCore& camera) { return camera.freezeImage(freeze); });
}
std::int32_t ThermalCamera::setBootLogo(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setBootLogo(enabled); });
}
std::int32_t ThermalCamera::setPalette(airui::Palette palette)
{
    return call([&](AiruiCore& camera) { return camera.setPalette(palette); });
}
std::int32_t ThermalCamera::getPalette(airui::Palette& palette)
{
    return call([&](AiruiCore& camera) { return camera.getPalette(palette); });
}
std::int32_t ThermalCamera::setWarningThreshold(airui::WarningColor color, std::uint8_t threshold)
{
    return call([&](AiruiCore& camera) { return camera.setWarningThreshold(color, threshold); });
}
std::int32_t ThermalCamera::setDigitalVideo(airui::DigitalVideo interface)
{
    return call([&](AiruiCore& camera) { return camera.setDigitalVideo(interface); });
}
std::int32_t ThermalCamera::setVideoSource(airui::VideoSource parallel, airui::VideoSource serial)
{
    return call([&](AiruiCore& camera) { return camera.setVideoSource(parallel, serial); });
}
std::int32_t ThermalCamera::setImageMode(airui::ImageMode mode)
{
    return call([&](AiruiCore& camera) { return camera.setImageMode(mode); });
}
std::int32_t ThermalCamera::getImageMode(airui::ImageMode& mode)
{
    return call([&](AiruiCore& camera) { return camera.getImageMode(mode); });
}
std::int32_t ThermalCamera::setBrightness(std::uint8_t value)
{
    return call([&](AiruiCore& camera) { return camera.setBrightness(value); });
}
std::int32_t ThermalCamera::getBrightness(std::uint8_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getBrightness(value); });
}
std::int32_t ThermalCamera::setContrast(std::uint8_t value)
{
    return call([&](AiruiCore& camera) { return camera.setContrast(value); });
}
std::int32_t ThermalCamera::getContrast(std::uint8_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getContrast(value); });
}
std::int32_t ThermalCamera::setDetail(std::uint8_t value)
{
    return call([&](AiruiCore& camera) { return camera.setDetail(value); });
}
std::int32_t ThermalCamera::getDetail(std::uint8_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getDetail(value); });
}
std::int32_t ThermalCamera::setSpatialFilter(std::uint8_t value)
{
    return call([&](AiruiCore& camera) { return camera.setSpatialFilter(value); });
}
std::int32_t ThermalCamera::getSpatialFilter(std::uint8_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getSpatialFilter(value); });
}
std::int32_t ThermalCamera::setTemporalFilter(std::uint8_t value)
{
    return call([&](AiruiCore& camera) { return camera.setTemporalFilter(value); });
}
std::int32_t ThermalCamera::getTemporalFilter(std::uint8_t& value)
{
    return call([&](AiruiCore& camera) { return camera.getTemporalFilter(value); });
}
std::int32_t ThermalCamera::setLensType(airui::LensType type)
{
    return call([&](AiruiCore& camera) { return camera.setLensType(type); });
}
std::int32_t ThermalCamera::getLensType(airui::LensType& type)
{
    return call([&](AiruiCore& camera) { return camera.getLensType(type); });
}
std::int32_t ThermalCamera::setManualAlarm(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setManualAlarm(enabled); });
}
std::int32_t ThermalCamera::setAutofocusAfterZoom(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setAutofocusAfterZoom(enabled); });
}
std::int32_t ThermalCamera::setFocusTemperatureInterval(double celsius)
{
    return call([&](AiruiCore& camera) { return camera.setFocusTemperatureInterval(celsius); });
}
std::int32_t ThermalCamera::getFocusTemperatureInterval(double& celsius)
{
    return call([&](AiruiCore& camera) { return camera.getFocusTemperatureInterval(celsius); });
}
std::int32_t ThermalCamera::autoFocus()
{
    return call([&](AiruiCore& camera) { return camera.autofocus(); });
}
std::int32_t ThermalCamera::driveFocus(airui::FocusDirection direction, airui::Adjustment adjustment)
{
    return call([&](AiruiCore& camera) { return camera.driveFocus(direction, adjustment); });
}
std::int32_t ThermalCamera::stopFocus()
{
    return call([&](AiruiCore& camera) { return camera.stopFocus(); });
}
std::int32_t ThermalCamera::getFocusPosition(std::uint16_t& position)
{
    return call([&](AiruiCore& camera) { return camera.getFocusPosition(position); });
}
std::int32_t ThermalCamera::setFocusSpeed(std::uint8_t speed)
{
    return call([&](AiruiCore& camera) { return camera.setFocusSpeed(speed); });
}
std::int32_t ThermalCamera::getFocusSpeed(std::uint8_t& speed)
{
    return call([&](AiruiCore& camera) { return camera.getFocusSpeed(speed); });
}
std::int32_t ThermalCamera::getFocusTravel(airui::MotorTravel& travel)
{
    return call([&](AiruiCore& camera) { return camera.getFocusTravel(travel); });
}
std::int32_t ThermalCamera::driveZoom(airui::ZoomDirection direction, airui::Adjustment adjustment)
{
    return call([&](AiruiCore& camera) { return camera.driveZoom(direction, adjustment); });
}
std::int32_t ThermalCamera::stopZoom()
{
    return call([&](AiruiCore& camera) { return camera.stopZoom(); });
}
std::int32_t ThermalCamera::getZoomPosition(std::uint16_t& position)
{
    return call([&](AiruiCore& camera) { return camera.getZoomPosition(position); });
}
std::int32_t ThermalCamera::setZoomSpeed(std::uint8_t speed)
{
    return call([&](AiruiCore& camera) { return camera.setZoomSpeed(speed); });
}
std::int32_t ThermalCamera::getZoomSpeed(std::uint8_t& speed)
{
    return call([&](AiruiCore& camera) { return camera.getZoomSpeed(speed); });
}
std::int32_t ThermalCamera::getZoomTravel(airui::MotorTravel& travel)
{
    return call([&](AiruiCore& camera) { return camera.getZoomTravel(travel); });
}
std::int32_t ThermalCamera::writePreset(airui::Motor motor, std::uint8_t index)
{
    return call([&](AiruiCore& camera) { return camera.writePreset(motor, index); });
}
std::int32_t ThermalCamera::readPreset(airui::Motor motor, std::uint8_t index, std::uint16_t& position)
{
    return call([&](AiruiCore& camera) { return camera.readPreset(motor, index, position); });
}
std::int32_t ThermalCamera::callPreset(airui::Motor motor, std::uint8_t index)
{
    return call([&](AiruiCore& camera) { return camera.callPreset(motor, index); });
}
std::int32_t ThermalCamera::getFocalLength(double& millimeters)
{
    return call([&](AiruiCore& camera) { return camera.getFocalLength(millimeters); });
}
std::int32_t ThermalCamera::setFocalLength(double millimeters)
{
    return call([&](AiruiCore& camera) { return camera.setFocalLength(millimeters); });
}
std::int32_t ThermalCamera::setFocalLengthDisplay(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setFocalLengthDisplay(enabled); });
}
std::int32_t ThermalCamera::getFieldOfView(airui::Axis axis, float& degrees)
{
    return call([&](AiruiCore& camera) { return camera.getFieldOfView(axis, degrees); });
}
std::int32_t ThermalCamera::setTemperatureRange(airui::TemperatureRange range)
{
    return call([&](AiruiCore& camera) { return camera.setTemperatureRange(range); });
}
std::int32_t ThermalCamera::setTemperatureDisplay(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setTemperatureDisplay(enabled); });
}
std::int32_t ThermalCamera::getTemperatureUnit(airui::TemperatureUnit& unit)
{
    return call([&](AiruiCore& camera) { return camera.getTemperatureUnit(unit); });
}
std::int32_t ThermalCamera::setTemperatureUnit(airui::TemperatureUnit unit)
{
    return call([&](AiruiCore& camera) { return camera.setTemperatureUnit(unit); });
}
std::int32_t ThermalCamera::getReflectedTemperature(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getReflectedTemperature(value); });
}
std::int32_t ThermalCamera::setReflectedTemperature(double value)
{
    return call([&](AiruiCore& camera) { return camera.setReflectedTemperature(value); });
}
std::int32_t ThermalCamera::getAtmosphericTemperature(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getAtmosphericTemperature(value); });
}
std::int32_t ThermalCamera::setAtmosphericTemperature(double value)
{
    return call([&](AiruiCore& camera) { return camera.setAtmosphericTemperature(value); });
}
std::int32_t ThermalCamera::getEmissivity(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getEmissivity(value); });
}
std::int32_t ThermalCamera::setEmissivity(double value)
{
    return call([&](AiruiCore& camera) { return camera.setEmissivity(value); });
}
std::int32_t ThermalCamera::getTargetDistance(double& meters)
{
    return call([&](AiruiCore& camera) { return camera.getTargetDistance(meters); });
}
std::int32_t ThermalCamera::setTargetDistance(double meters)
{
    return call([&](AiruiCore& camera) { return camera.setTargetDistance(meters); });
}
std::int32_t ThermalCamera::getHumidity(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getHumidity(value); });
}
std::int32_t ThermalCamera::setHumidity(double value)
{
    return call([&](AiruiCore& camera) { return camera.setHumidity(value); });
}
std::int32_t ThermalCamera::applyEnvironment()
{
    return call([&](AiruiCore& camera) { return camera.applyEnvironment(); });
}
std::int32_t ThermalCamera::setCenterTemperatureDisplay(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setCenterTemperatureDisplay(enabled); });
}
std::int32_t ThermalCamera::getCenterTemperature(airui::CenterTemperature& value)
{
    return call([&](AiruiCore& camera) { return camera.getCenterTemperature(value); });
}
std::int32_t ThermalCamera::setFireAlarm(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setFireAlarm(enabled); });
}
std::int32_t ThermalCamera::setFireThreshold(std::uint16_t threshold)
{
    return call([&](AiruiCore& camera) { return camera.setFireThreshold(threshold); });
}
std::int32_t ThermalCamera::getFireThreshold(std::uint16_t& threshold)
{
    return call([&](AiruiCore& camera) { return camera.getFireThreshold(threshold); });
}
std::int32_t ThermalCamera::setTemperatureStretch(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setTemperatureStretch(enabled); });
}
std::int32_t ThermalCamera::setStretchLow(double value)
{
    return call([&](AiruiCore& camera) { return camera.setStretchLow(value); });
}
std::int32_t ThermalCamera::getStretchLow(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getStretchLow(value); });
}
std::int32_t ThermalCamera::setStretchHigh(double value)
{
    return call([&](AiruiCore& camera) { return camera.setStretchHigh(value); });
}
std::int32_t ThermalCamera::getStretchHigh(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getStretchHigh(value); });
}
std::int32_t ThermalCamera::setTemperatureAlarm(airui::TemperatureAlarm mode)
{
    return call([&](AiruiCore& camera) { return camera.setTemperatureAlarm(mode); });
}
std::int32_t ThermalCamera::getTemperatureAlarm(airui::TemperatureAlarm& mode)
{
    return call([&](AiruiCore& camera) { return camera.getTemperatureAlarm(mode); });
}
std::int32_t ThermalCamera::setIsotherm(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setIsotherm(enabled); });
}
std::int32_t ThermalCamera::getIsotherm(bool& enabled)
{
    return call([&](AiruiCore& camera) { return camera.getIsotherm(enabled); });
}
std::int32_t ThermalCamera::setAlarmLow(double value)
{
    return call([&](AiruiCore& camera) { return camera.setAlarmLow(value); });
}
std::int32_t ThermalCamera::getAlarmLow(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getAlarmLow(value); });
}
std::int32_t ThermalCamera::setAlarmHigh(double value)
{
    return call([&](AiruiCore& camera) { return camera.setAlarmHigh(value); });
}
std::int32_t ThermalCamera::getAlarmHigh(double& value)
{
    return call([&](AiruiCore& camera) { return camera.getAlarmHigh(value); });
}
std::int32_t ThermalCamera::setLensTemperatureCorrection(bool enabled)
{
    return call([&](AiruiCore& camera) { return camera.setLensTemperatureCorrection(enabled); });
}
std::int32_t ThermalCamera::hasLensCorrectionData(bool& saved)
{
    return call([&](AiruiCore& camera) { return camera.hasLensCorrectionData(saved); });
}
std::int32_t ThermalCamera::eraseLensCorrection()
{
    return call([&](AiruiCore& camera) { return camera.eraseLensCorrection(); });
}
std::int32_t ThermalCamera::calibrateLensTemperature(std::int16_t targetTemperature)
{
    return call([&](AiruiCore& camera) { return camera.calibrateLensTemperature(targetTemperature); });
}
std::int32_t ThermalCamera::saveLensCorrection()
{
    return call([&](AiruiCore& camera) { return camera.saveLensCorrection(); });
}
std::int32_t ThermalCamera::getStrongLightProtection(airui::StrongLightProtection& value)
{
    return call([&](AiruiCore& camera) { return camera.getStrongLightProtection(value); });
}
std::int32_t ThermalCamera::setStrongLightProtection(const airui::StrongLightProtection& value)
{
    return call([&](AiruiCore& camera) { return camera.setStrongLightProtection(value); });
}
std::int32_t ThermalCamera::showBlindPixelCursor(bool show)
{
    return call([&](AiruiCore& camera) { return camera.showBlindPixelCursor(show); });
}
std::int32_t ThermalCamera::moveBlindPixelCursor(airui::CursorMove move)
{
    return call([&](AiruiCore& camera) { return camera.moveBlindPixelCursor(move); });
}
std::int32_t ThermalCamera::editBlindPixel(airui::BlindPixelEdit action)
{
    return call([&](AiruiCore& camera) { return camera.editBlindPixel(action); });
}
std::int32_t ThermalCamera::saveBlindPixels()
{
    return call([&](AiruiCore& camera) { return camera.saveBlindPixels(); });
}
std::int32_t ThermalCamera::calibrateLensK(airui::LensKAction action)
{
    return call([&](AiruiCore& camera) { return camera.calibrateLensK(action); });
}
std::int32_t ThermalCamera::compensateImage(airui::CompensationAction action)
{
    return call([&](AiruiCore& camera) { return camera.compensateImage(action); });
}
std::int32_t ThermalCamera::getSynchronization(airui::Synchronization& value)
{
    return call([&](AiruiCore& camera) { return camera.getSynchronization(value); });
}
std::int32_t ThermalCamera::setSynchronization(const airui::Synchronization& value)
{
    return call([&](AiruiCore& camera) { return camera.setSynchronization(value); });
}
