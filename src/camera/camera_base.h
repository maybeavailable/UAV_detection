#pragma once
#include"common/common.h"


enum class PixelFormat : uint32_t
{
    UNKNOWN,
    NV12,
    BGR888,
    RGB888,
    YUV420P
};


struct CameraParam
{
public:
    CameraParam() = default;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t fps{0};
    PixelFormat format{PixelFormat::UNKNOWN};

};

class CameraBase
{
public:
    CameraBase() = default;
    virtual ~CameraBase() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpened() const = 0;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool getFrame(std::vector<std::uint8_t>& frame) = 0;

    virtual bool setParam(const CameraParam& param) = 0;
    virtual CameraParam getParam() const = 0;

private:
    CameraParam m_param;
};

