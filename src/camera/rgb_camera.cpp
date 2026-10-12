#include "rgb_camera.h"

// ---------------------------------------------------------------------------
// RGBCamera 实现：把工程侧语义（0~100 的档位、业务约定的模式码）折算成
// 世辉协议值后转给 ShihuiCore。所有方法在未连接时直接返回 0（失败）。
//
// 注意：下面的数值换算与原 CameraNode 的 SHIHUI 分支保持一致，改动会影响上层
// （例如界面滑条 0~100 对应的效果），调整前请确认。
// ---------------------------------------------------------------------------

namespace
{
// 未连接时统一失败
inline bool ready(const std::unique_ptr<ShihuiCore> &core)
{
    return core != nullptr && core->isConnected();
}
}

RGBCamera::RGBCamera(const std::string &port, uint64_t baud)
    : shihui_camera_(std::make_unique<ShihuiCore>(port, baud))
{
}

RGBCamera::~RGBCamera()
{
    if (shihui_camera_)
    {
        shihui_camera_->disconnect();
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
bool RGBCamera::connect()
{
    if (!shihui_camera_)
    {
        return false;
    }
    return shihui_camera_->connect();
}

void RGBCamera::disconnect()
{
    if (shihui_camera_)
    {
        shihui_camera_->disconnect();
    }
}

bool RGBCamera::isConnected() const
{
    return shihui_camera_ && shihui_camera_->isConnected();
}

bool RGBCamera::reconnect()
{
    if (!shihui_camera_)
    {
        return false;
    }
    return shihui_camera_->reconnect();
}

bool RGBCamera::init(const std::string &port, uint64_t baud)
{
    if (shihui_camera_)
    {
        shihui_camera_->disconnect();
    }
    shihui_camera_ = std::make_unique<ShihuiCore>(port, baud);
    return shihui_camera_ != nullptr;
}

void RGBCamera::setAutoReconnect(bool enable)
{
    if (shihui_camera_)
    {
        shihui_camera_->setAutoReconnect(enable);
    }
}

void RGBCamera::setMaxReconnectAttempts(int32_t attempts)
{
    if (shihui_camera_)
    {
        shihui_camera_->setMaxReconnectAttempts(attempts);
    }
}

void RGBCamera::setReconnectDelay(int32_t delay_ms)
{
    if (shihui_camera_)
    {
        shihui_camera_->setReconnectDelay(delay_ms);
    }
}

bool RGBCamera::getAutoReconnect() const
{
    return shihui_camera_ ? shihui_camera_->getAutoReconnect() : false;
}

int32_t RGBCamera::getMaxReconnectAttempts() const
{
    return shihui_camera_ ? shihui_camera_->getMaxReconnectAttempts() : 0;
}

int32_t RGBCamera::getReconnectDelay() const
{
    return shihui_camera_ ? shihui_camera_->getReconnectDelay() : 0;
}

void RGBCamera::setReportCallback(ReportCallback callback)
{
    if (shihui_camera_)
    {
        shihui_camera_->setShihuiReportCallback(std::move(callback));
    }
}

// ---------------------------------------------------------------------------
// 变倍
// ---------------------------------------------------------------------------
int32_t RGBCamera::continuousZoom(uint8_t direction, uint8_t speed)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    ZoomControlType type = ZoomControlType::ZOOM_STOP;
    uint8_t level = 0;

    switch (direction)
    {
    case ZOOM_DIR_IN:
        // 放大：默认 0x27；给速度则映射到 0x21~0x27
        level = 0x27;
        type = ZoomControlType::ZOOM_TELE;
        if (speed > 0 && speed <= 100)
        {
            level = static_cast<uint8_t>(map_value(static_cast<double>(speed), 0.0, 100.0, 0x21, 0x27));
        }
        break;
    case ZOOM_DIR_OUT:
        level = 0x37;
        type = ZoomControlType::ZOOM_WIDE;
        if (speed > 0 && speed <= 100)
        {
            level = static_cast<uint8_t>(map_value(static_cast<double>(speed), 0.0, 100.0, 0x31, 0x37));
        }
        break;
    default:
        type = ZoomControlType::ZOOM_STOP;
        break;
    }

    return shihui_camera_->setZoomControl(type, level) ? 1 : 0;
}

int32_t RGBCamera::zoomToPosition(uint16_t position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setZoomControl(ZoomControlType::ZOOM_DIRECT, position) ? 1 : 0;
}

int32_t RGBCamera::setCustomZoom(uint32_t position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setCustomZoom(position) ? 1 : 0;
}

int32_t RGBCamera::setOriginalZoom(uint32_t position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setOriginalZoom(position) ? 1 : 0;
}

int32_t RGBCamera::setZoomFocus(uint32_t zoomPosition, uint32_t focusPosition, bool isThermal)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setZoomFocus(zoomPosition, focusPosition, isThermal) ? 1 : 0;
}

int32_t RGBCamera::setThermalDZoom(uint8_t position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setThermalDZoom(position) ? 1 : 0;
}

int32_t RGBCamera::setZoomMode(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setZoomMode(static_cast<ZoomMode>(mode)) ? 1 : 0;
}

int32_t RGBCamera::getZoomMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getZoomMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getMaximumZoomRatio(uint8_t &opticalZoom, uint8_t &digitalZoom)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getMaximumZoomRatio(opticalZoom, digitalZoom) ? 1 : 0;
}

int32_t RGBCamera::getZoomPosition(uint16_t &position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getZoomPosition(position) ? 1 : 0;
}

int32_t RGBCamera::getCurrentZoomPos(uint32_t &targetPosition, uint32_t &originalZoomRatio,
                                     uint16_t &zoomPosition)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getCurrentZoomPosition(targetPosition, originalZoomRatio, zoomPosition) ? 1 : 0;
}

int32_t RGBCamera::getContinuousZoomPos(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getContinuousZoomPos(mode) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 聚焦
// ---------------------------------------------------------------------------
int32_t RGBCamera::continuousFocus(uint8_t direction, uint8_t speed)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    FocusControlType type = FocusControlType::FOCUS_STOP;
    uint8_t level = 0;

    switch (direction)
    {
    case FOCUS_DIR_NEAR:
        // 近焦：默认 0x37，给速度则映射到 0x31~0x37
        level = 0x37;
        type = FocusControlType::FOCUS_NEAR;
        if (speed > 0 && speed <= 100)
        {
            level = static_cast<uint8_t>(map_value(static_cast<double>(speed), 0.0, 100.0, 0x31, 0x37));
        }
        break;
    case FOCUS_DIR_FAR:
        // 远焦：默认 0x27，给速度则映射到 0x21~0x27
        level = 0x27;
        type = FocusControlType::FOCUS_FAR;
        if (speed > 0 && speed <= 100)
        {
            level = static_cast<uint8_t>(map_value(static_cast<double>(speed), 0.0, 100.0, 0x21, 0x27));
        }
        break;
    default:
        type = FocusControlType::FOCUS_STOP;
        break;
    }

    return shihui_camera_->setFocusControl(type, level) ? 1 : 0;
}

int32_t RGBCamera::focusToPosition(uint16_t position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setFocusControl(FocusControlType::FOCUS_DIRECT, position) ? 1 : 0;
}

int32_t RGBCamera::autoFocus()
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setFocusControl(FocusControlType::FOCUS_ONE_PUSH, 0) ? 1 : 0;
}

int32_t RGBCamera::setFocusSensitivity(uint8_t sensitivity)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setFocusSensitivity(static_cast<FocusSensitivity>(sensitivity)) ? 1 : 0;
}

int32_t RGBCamera::getAFSensitivity(uint8_t &sensitivity)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getAFSensitivity(sensitivity) ? 1 : 0;
}

// 聚焦近限：业务侧用 0~7 档，折算成镜头的绝对位置（与原实现同一张表）
int32_t RGBCamera::setFocusLimit(uint16_t limit)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    uint16_t value = 0x8000;   // 默认最远
    switch (limit)
    {
    case 1: value = 0x7000; break;
    case 2: value = 0x6000; break;
    case 3: value = 0x4000; break;
    case 4: value = 0x3000; break;
    case 5: value = 0x1B00; break;
    case 6: value = 0x1900; break;
    case 7: value = 0x1600; break;
    default: value = 0x8000; break;
    }
    return shihui_camera_->setFocusLimit(value) ? 1 : 0;
}

int32_t RGBCamera::getFocusNearLimit(uint16_t &limit)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getFocusNearLimit(limit) ? 1 : 0;
}

int32_t RGBCamera::setRoiFocus(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setRoiFocus(x1, y1, x2, y2) ? 1 : 0;
}

int32_t RGBCamera::setAutoFocusMode(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setAutoFocusMode(static_cast<AutoFocusMode>(mode)) ? 1 : 0;
}

int32_t RGBCamera::getAFMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getAFMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getFocusMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getFocusMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getFocusPosition(uint16_t &position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getFocusPosition(position) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 图像参数（工程值 0~100 → 协议值）
// ---------------------------------------------------------------------------
int32_t RGBCamera::setBrightness(uint8_t mode, uint8_t value)
{
    (void)mode;
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    value = static_cast<uint8_t>(static_cast<float>(value) / 100.0f * 255.0f);
    return shihui_camera_->setImageProperty(ImagePropertyType::BRIGHTNESS, value) ? 1 : 0;
}

int32_t RGBCamera::getBrightness(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getImageProperty(ImagePropertyType::BRIGHTNESS, value) ? 1 : 0;
}

int32_t RGBCamera::setContrast(uint8_t mode, uint8_t value)
{
    (void)mode;
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    value = static_cast<uint8_t>(static_cast<float>(value) / 100.0f * 255.0f);
    return shihui_camera_->setImageProperty(ImagePropertyType::CONTRAST, value) ? 1 : 0;
}

int32_t RGBCamera::getContrast(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getImageProperty(ImagePropertyType::CONTRAST, value) ? 1 : 0;
}

int32_t RGBCamera::setSaturation(uint8_t value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    value = static_cast<uint8_t>(static_cast<float>(value) / 100.0f * 255.0f);
    return shihui_camera_->setImageProperty(ImagePropertyType::SATURATION, value) ? 1 : 0;
}

int32_t RGBCamera::getSaturation(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getImageProperty(ImagePropertyType::SATURATION, value) ? 1 : 0;
}

int32_t RGBCamera::setSharpness(uint8_t mode, uint8_t value)
{
    (void)mode;
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    value = static_cast<uint8_t>(map_value(static_cast<double>(value), 0.0, 100.0, 0x00, 0x0F));
    return shihui_camera_->setSharpness(value) ? 1 : 0;
}

int32_t RGBCamera::getSharpness(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getSharpness(value) ? 1 : 0;
}

int32_t RGBCamera::setGamma(uint8_t value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    value = static_cast<uint8_t>(map_value(static_cast<double>(value), 0.0, 100.0, 0x00, 0x0F));
    return shihui_camera_->setGamma(value) ? 1 : 0;
}

int32_t RGBCamera::getGamma(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    // 原实现这里误调用了 setGamma（把查询写成了设置），此处改为真正的查询
    return shihui_camera_->getGamma(value) ? 1 : 0;
}

int32_t RGBCamera::setImageProperty(uint8_t type, uint8_t value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setImageProperty(static_cast<ImagePropertyType>(type), value) ? 1 : 0;
}

int32_t RGBCamera::setImageStyle(uint8_t style)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setImageStyle(static_cast<ImageStyle>(style)) ? 1 : 0;
}

int32_t RGBCamera::getStyle(uint8_t &style)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getStyle(style) ? 1 : 0;
}

// 业务侧翻转模式 → 协议翻转码（与原实现同一张表：0→0, 1→3, 2→1, 3→2）
int32_t RGBCamera::setImageFlip(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    uint8_t protocol_mode = 0;
    switch (mode)
    {
    case FLIP_NONE:       protocol_mode = 0; break;
    case FLIP_HORIZONTAL: protocol_mode = 3; break;
    case FLIP_VERTICAL:   protocol_mode = 1; break;
    case FLIP_BOTH:       protocol_mode = 2; break;
    default:              protocol_mode = 0; break;
    }
    return shihui_camera_->setImageFlip(protocol_mode) ? 1 : 0;
}

int32_t RGBCamera::setImageFlipMode(uint8_t mode)
{
    return setImageFlip(mode);
}

int32_t RGBCamera::getImageFlip(uint8_t &flip_mode)
{
    return getPictureFlipMode(flip_mode);
}

int32_t RGBCamera::getPictureFlipMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getPictureFlipMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getLRReverseMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getLRReverseMode(mode) ? 1 : 0;
}

int32_t RGBCamera::setVideoFreeze(bool freeze)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->freezeImage(freeze) ? 1 : 0;
}

// 降噪：mode 0 → 2D，其它(1) → 3D；level 为 0~100 的协议值（原实现直接透传）
int32_t RGBCamera::setNoiseReduction(uint8_t mode, uint8_t level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    if (mode == 0)
    {
        return shihui_camera_->setNoiseReduction2D(level) ? 1 : 0;
    }
    return shihui_camera_->setNoiseReduction3D(level) ? 1 : 0;
}

int32_t RGBCamera::getNR2D(uint8_t &level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getNR2D(level) ? 1 : 0;
}

int32_t RGBCamera::getNR3D(uint8_t &level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getNR3D(level) ? 1 : 0;
}

int32_t RGBCamera::setAINR(uint8_t mode, uint8_t level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setAINR(mode, level) ? 1 : 0;
}

int32_t RGBCamera::setPseudoColor(uint8_t color)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setPseudoColor(color) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 白平衡与曝光
// ---------------------------------------------------------------------------
int32_t RGBCamera::setWhiteBalanceMode(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setWhiteBalance(static_cast<WhiteBalanceMode>(mode)) ? 1 : 0;
}

int32_t RGBCamera::getWhiteBalance(WhiteBalanceMode &mode, uint8_t &r_gain, uint8_t &b_gain)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getWhiteBalance(mode, r_gain, b_gain) ? 1 : 0;
}

int32_t RGBCamera::setRGBGain(uint8_t r, uint8_t g, uint8_t b)
{
    // 世辉只有 R/B 两路增益，g 被忽略
    (void)g;
    return setRBgain(r, b);
}

int32_t RGBCamera::setRBgain(uint8_t rGain, uint8_t bGain)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setRBgain(rGain, bGain) ? 1 : 0;
}

// 业务曝光模式 → 协议曝光模式（与原实现同一张表）
int32_t RGBCamera::setExposureMode(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    ExposureMode expMode = ExposureMode::EXPOSURE_AUTO;
    switch (mode)
    {
    case 0x00: expMode = ExposureMode::EXPOSURE_AUTO; break;
    case 0x01: expMode = ExposureMode::EXPOSURE_MANUAL; break;
    case 0x02: expMode = ExposureMode::EXPOSURE_IRIS_PRIORITY; break;
    case 0x03: expMode = ExposureMode::EXPOSURE_SHUTTER_PRIORITY; break;
    default: return 0;
    }
    return shihui_camera_->setExposureMode(expMode) ? 1 : 0;
}

// 查询曝光模式：协议值 → 业务约定值（0x00 自动/0x01 手动/0x02 光圈优先/0x03 快门优先）
int32_t RGBCamera::getExposureMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    // 原实现对未初始化的入参做 switch 后再查询，这里直接查询并换算
    uint8_t aeMode = 0;
    if (!shihui_camera_->getAEMode(aeMode))
    {
        return 0;
    }

    switch (static_cast<ExposureMode>(aeMode))
    {
    case ExposureMode::EXPOSURE_AUTO:             mode = 0x00; break;
    case ExposureMode::EXPOSURE_MANUAL:           mode = 0x01; break;
    case ExposureMode::EXPOSURE_IRIS_PRIORITY:    mode = 0x02; break;
    case ExposureMode::EXPOSURE_SHUTTER_PRIORITY: mode = 0x03; break;
    default:                                      mode = aeMode; break;
    }
    return 1;
}

int32_t RGBCamera::getAEMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getAEMode(mode) ? 1 : 0;
}

int32_t RGBCamera::setExposure(uint8_t mode, uint8_t apertureMin, uint8_t apertureMax,
                               uint32_t shutterMin, uint32_t shutterMax, uint8_t gainMin,
                               uint8_t gainMax)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setExposure(static_cast<ExposureMode>(mode), apertureMin, apertureMax,
                                       shutterMin, shutterMax, gainMin, gainMax) ? 1 : 0;
}

// 曝光控制：type 0~15 的语义见头文件；部分类型需要把 0~100 折算成协议值
int32_t RGBCamera::setExposureControl(uint8_t type, uint8_t value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    ExposureControlType controlType = ExposureControlType::EXP_COMP_DIRECT;
    switch (type)
    {
    case 0:  controlType = ExposureControlType::APERTURE_UP; break;
    case 1:  controlType = ExposureControlType::APERTURE_DOWN; break;
    case 2:  controlType = ExposureControlType::APERTURE_RESET; break;
    case 3:  controlType = ExposureControlType::APERTURE_DIRECT; break;
    case 4:  controlType = ExposureControlType::GAIN_UP; break;
    case 5:  controlType = ExposureControlType::GAIN_DOWN; break;
    case 6:  controlType = ExposureControlType::GAIN_RESET; break;
    case 7:
        controlType = ExposureControlType::GAIN_DIRECT;
        if (value > 0 && value <= 100)
        {
            value = static_cast<uint8_t>(map_value(static_cast<double>(value), 0.0, 100.0, 0x00, 0x0F));
        }
        break;
    case 8:  controlType = ExposureControlType::SHUTTER_UP; break;
    case 9:  controlType = ExposureControlType::SHUTTER_DOWN; break;
    case 10: controlType = ExposureControlType::SHUTTER_RESET; break;
    case 11: controlType = ExposureControlType::SHUTTER_DIRECT; break;
    case 12: controlType = ExposureControlType::EXP_COMP_UP; break;
    case 13: controlType = ExposureControlType::EXP_COMP_DOWN; break;
    case 14: controlType = ExposureControlType::EXP_COMP_RESET; break;
    case 15:
        controlType = ExposureControlType::EXP_COMP_DIRECT;
        if (value > 0 && value <= 100)
        {
            value = static_cast<uint8_t>(map_value(static_cast<double>(value), 0.0, 100.0, 0x00, 0x0E));
        }
        break;
    default: controlType = ExposureControlType::EXP_COMP_DIRECT; break;
    }

    return shihui_camera_->setExposureControl(controlType, value) ? 1 : 0;
}

int32_t RGBCamera::setAperture(uint8_t aperture)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setExposureControl(ExposureControlType::APERTURE_DIRECT, aperture) ? 1 : 0;
}

int32_t RGBCamera::setShutterSpeed(uint8_t speed)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setExposureControl(ExposureControlType::SHUTTER_DIRECT, speed) ? 1 : 0;
}

int32_t RGBCamera::setGain(uint8_t gain)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setExposureControl(ExposureControlType::GAIN_DIRECT, gain) ? 1 : 0;
}

int32_t RGBCamera::getShutterPosition(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getShutterPosition(value) ? 1 : 0;
}

int32_t RGBCamera::getGainPosition(uint8_t &position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getGainPosition(position) ? 1 : 0;
}

int32_t RGBCamera::getExposureCompensationPosition(uint8_t &position)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getExposureCompensationPosition(position) ? 1 : 0;
}

int32_t RGBCamera::setAntiFlickerMode(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setAntiFlickerMode(static_cast<AntiFlickerMode>(mode)) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 宽动态 / 透雾 / 防抖 / 强光抑制
// ---------------------------------------------------------------------------
int32_t RGBCamera::setWDR(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    WDRMode wdrMode = WDRMode::WDR_OFF;
    switch (mode)
    {
    case 0x00: wdrMode = WDRMode::WDR_OFF; break;
    case 0x01: wdrMode = WDRMode::WDR_ON; break;
    default: return 0;
    }
    return shihui_camera_->setWDRLevel(wdrMode, 0) ? 1 : 0;
}

int32_t RGBCamera::setWDRLevel(uint8_t mode, uint8_t level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setWDRLevel(mode, level) ? 1 : 0;
}

int32_t RGBCamera::getWDRMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getWDRMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getWD(uint8_t &level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getWD(level) ? 1 : 0;
}

// 业务除雾模式 → 协议模式（与原实现同一张表：0→自动 1→手动 2→关闭）
int32_t RGBCamera::setDefog(uint8_t mode, uint8_t level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    DefogMode defogMode = DefogMode::DEFOG_AUTO;
    switch (mode)
    {
    case 0x00: defogMode = DefogMode::DEFOG_AUTO; break;
    case 0x01: defogMode = DefogMode::DEFOG_MANUAL; break;
    case 0x02: defogMode = DefogMode::DEFOG_OFF; break;
    default: return 0;
    }

    if (level > 100)
    {
        level = 100;
    }
    return shihui_camera_->setDefogLevel(defogMode, level) ? 1 : 0;
}

int32_t RGBCamera::getElectronicFogMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getElectronicFogMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getFogPenetrationLevel(uint8_t &level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getFogPenetrationLevel(level) ? 1 : 0;
}

int32_t RGBCamera::setEIS(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    StabilizerMode stabMode = StabilizerMode::STABILIZER_OFF;
    switch (mode)
    {
    case 0x00: stabMode = StabilizerMode::STABILIZER_OFF; break;
    case 0x01: stabMode = StabilizerMode::STABILIZER_EIS; break;
    default: return 0;
    }
    return shihui_camera_->setStabilizerMode(stabMode) ? 1 : 0;
}

int32_t RGBCamera::getStabilizerMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getStabilizerMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getImageStabilizer(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getImageStabilizer(mode) ? 1 : 0;
}

int32_t RGBCamera::setHLC(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setHighlightCompensation(mode != 0) ? 1 : 0;
}

int32_t RGBCamera::getHighlightCompensationMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getHighlightCompensationMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getHighlightCompensationStrength(uint8_t &strength)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getHighlightCompensationStrength(strength) ? 1 : 0;
}

int32_t RGBCamera::setHeatHaze(uint8_t mode, uint8_t level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setHeatHaze(static_cast<HeatHazeMode>(mode),
                                       static_cast<HeatHazeLevel>(level)) ? 1 : 0;
}

int32_t RGBCamera::getHeatHaze(uint8_t &mode, uint8_t &level)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }

    HeatHazeMode hazeMode = HeatHazeMode::HEAT_HAZE_OFF;
    HeatHazeLevel hazeLevel = HeatHazeLevel::HEAT_HAZE_LEVEL_LOW;
    if (!shihui_camera_->getHeatHaze(hazeMode, hazeLevel))
    {
        return 0;
    }
    mode = static_cast<uint8_t>(hazeMode);
    level = static_cast<uint8_t>(hazeLevel);
    return 1;
}

// ---------------------------------------------------------------------------
// 日夜（ICR）
// ---------------------------------------------------------------------------
int32_t RGBCamera::setICR(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setICR(mode == 0 ? ICRMode::ICR_OFF : ICRMode::ICR_ON) ? 1 : 0;
}

int32_t RGBCamera::getICRMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getICRMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getICREnableMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getICREnableMode(mode) ? 1 : 0;
}

int32_t RGBCamera::setAutoICR(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setAutoICR(static_cast<AutoICRMode>(mode)) ? 1 : 0;
}

int32_t RGBCamera::getAutoICRMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    // 原实现先把未初始化的局部变量赋给输出参数再查询，这里直接查询
    return shihui_camera_->getAutoICRMode(mode) ? 1 : 0;
}

int32_t RGBCamera::setAutoICRThreshold(uint8_t threshold)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setAutoICRThreshold(threshold) ? 1 : 0;
}

int32_t RGBCamera::getAutoICRThreshold(uint8_t &threshold)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getAutoICRThreshold(threshold) ? 1 : 0;
}

int32_t RGBCamera::setDayNightSwitchDelay(uint8_t type, uint8_t delay)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setDayNightSwitchDelay(type, delay) ? 1 : 0;
}

int32_t RGBCamera::getDayNightSwitchDelay(uint8_t type, uint8_t &delay)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getDayNightSwitchDelay(type, delay) ? 1 : 0;
}

int32_t RGBCamera::getIrcGain(uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getIrcGain(value) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// OSD
// ---------------------------------------------------------------------------
int32_t RGBCamera::setOSD(uint8_t mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    // 网络 OSD + 时间标题（与原实现一致）
    return shihui_camera_->setOSDDisplay(0, OSDRegion::OSD_NETWORK,
                                         OSDFunction::OSD_TIME_TITLE, mode == 0x01) ? 1 : 0;
}

int32_t RGBCamera::setOSDDisplay(uint8_t channel, uint8_t region, uint8_t function, bool enable)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setOSDDisplay(channel, static_cast<OSDRegion>(region),
                                         static_cast<OSDFunction>(function), enable) ? 1 : 0;
}

int32_t RGBCamera::setCustomOSD(const int8_t *text)
{
    if (!ready(shihui_camera_) || text == nullptr)
    {
        return 0;
    }
    return shihui_camera_->setCustomOSD(reinterpret_cast<const char *>(text)) ? 1 : 0;
}

int32_t RGBCamera::setThermalGPSOSD(bool enable)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setThermalGPSOSD(enable) ? 1 : 0;
}

int32_t RGBCamera::setZoomOSD(bool enable)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setZoomOSD(enable) ? 1 : 0;
}

int32_t RGBCamera::setThermalTimeTitleOSD(bool enable)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setThermalTimeTitleOSD(enable) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 视频格式 / 分辨率
// ---------------------------------------------------------------------------
int32_t RGBCamera::setVideoFormat(uint8_t type)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setVideoFormat(static_cast<VideoFormat>(type), 0, 0) ? 1 : 0;
}

int32_t RGBCamera::setVideoFormat(uint8_t visibleFormat, uint8_t thermalFormatHigh,
                                  uint8_t thermalFormatLow)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setVideoFormat(static_cast<VideoFormat>(visibleFormat),
                                          thermalFormatHigh, thermalFormatLow) ? 1 : 0;
}

int32_t RGBCamera::getVisibleLightVideoFormat(uint8_t &format)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getVisibleLightVideoFormat(format) ? 1 : 0;
}

int32_t RGBCamera::getThermalVideoFormat(uint8_t &format)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getThermalVideoFormat(format) ? 1 : 0;
}

int32_t RGBCamera::getMipiFormat(uint8_t &formatHigh, uint8_t &formatLow)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getMipiFormat(formatHigh, formatLow) ? 1 : 0;
}

int32_t RGBCamera::setFormat(uint8_t format)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setFormat(format) ? 1 : 0;
}

int32_t RGBCamera::getFormat(uint8_t &format)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getFormat(format) ? 1 : 0;
}

int32_t RGBCamera::setHorizontalResolution(uint32_t resolution)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setHorizontalResolution(resolution) ? 1 : 0;
}

int32_t RGBCamera::setVerticalResolution(uint32_t resolution)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setVerticalResolution(resolution) ? 1 : 0;
}

int32_t RGBCamera::getHorizontalResolution(uint16_t &resolution)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getHorizontalResolution(resolution) ? 1 : 0;
}

int32_t RGBCamera::getVerticalResolution(uint16_t &resolution)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getVerticalResolution(resolution) ? 1 : 0;
}

int32_t RGBCamera::getMipiAcquisition(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getMipiAcquisition(mode) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 抓拍 / 录像
// ---------------------------------------------------------------------------
int32_t RGBCamera::setSnap(uint8_t count, uint8_t delay)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setSnap(count, delay) ? 1 : 0;
}

int32_t RGBCamera::setSnapOnePush()
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setSnapOnePush() ? 1 : 0;
}

int32_t RGBCamera::setRecord(bool start)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setRecord(start) ? 1 : 0;
}

int32_t RGBCamera::getSlowShutterMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getSlowShutterMode(mode) ? 1 : 0;
}

int32_t RGBCamera::getDZoomMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getDZoomMode(mode) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 系统
// ---------------------------------------------------------------------------
int32_t RGBCamera::setInitialize()
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setInitialize() ? 1 : 0;
}

int32_t RGBCamera::setImageInitialize()
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setImageInitialize() ? 1 : 0;
}

int32_t RGBCamera::setPowerOff()
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setPowerOff() ? 1 : 0;
}

int32_t RGBCamera::setBaudRate(uint8_t rate)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->setBaudRate(static_cast<BaudRate>(rate)) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
int32_t RGBCamera::getCoreTemperature(float &temperature)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getTemperature(temperature) ? 1 : 0;
}

int32_t RGBCamera::getProgramVersion(uint16_t &year, uint8_t &month, uint8_t &day,
                                     uint8_t &serialNumber)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getProgramVersion(year, month, day, serialNumber) ? 1 : 0;
}

int32_t RGBCamera::getVersionInfo(uint16_t &vendorId, uint16_t &modelId, uint8_t &majorVersion,
                                  uint8_t &minorVersion, uint8_t &fixVersion)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    // 原 CameraNode 对世辉这里直接返回 0，但 ShihuiCore 有 getVersion()，这里接上
    return shihui_camera_->getVersion(vendorId, modelId, majorVersion, minorVersion, fixVersion) ? 1 : 0;
}

int32_t RGBCamera::getLensZoomInfo(uint32_t &zoomTimes, uint32_t &zoomDirectionTimes)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getLensZoomInfo(zoomTimes, zoomDirectionTimes) ? 1 : 0;
}

int32_t RGBCamera::getLensFocusInfo(uint32_t &focusTimes, uint32_t &focusDirectionTimes)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getLensFocusInfo(focusTimes, focusDirectionTimes) ? 1 : 0;
}

int32_t RGBCamera::getLensIrisInfo(uint32_t &irisTimes, uint32_t &icrTimes)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getLensIrisInfo(irisTimes, icrTimes) ? 1 : 0;
}

int32_t RGBCamera::getRegisterValue(uint16_t regNum, uint8_t &value)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getRegisterValue(regNum, value) ? 1 : 0;
}

int32_t RGBCamera::getLockMode(uint8_t &mode)
{
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    return shihui_camera_->getLockMode(mode) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 兼容原 CameraNode 的参数通道
// ---------------------------------------------------------------------------
int32_t RGBCamera::setAdcGainParam(uint8_t subCommand, uint16_t data)
{
    (void)data;
    if (!ready(shihui_camera_))
    {
        return 0;
    }
    // 世辉：subCommand 0 → 仅光学变倍，其它 → 数字+光学变倍（沿用原实现）
    if (subCommand == 0)
    {
        return shihui_camera_->setZoomMode(ZoomMode::ZOOM_OPTICAL_ONLY) ? 1 : 0;
    }
    return shihui_camera_->setZoomMode(ZoomMode::ZOOM_DIGITAL_OPTICAL) ? 1 : 0;
}
