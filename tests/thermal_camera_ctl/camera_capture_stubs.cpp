#include "camera/rgb_camera_ctl.h"
#include "camera/thermal_camera_ctl.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
std::mutex traceMutex;
bool aclReady = false, deviceReady = false, contextReady = false, sysReady = false;
bool rgbReady = false, thermalReady = false;
int contextToken;
thread_local bool threadContext = false;
std::atomic<int> activeCalls{0};

void require(bool condition) { if (!condition) std::abort(); }
bool fault(const char* name)
{
    const char* selected = std::getenv("UAV_CAPTURE_TEST_FAULT");
    return selected && std::string(selected) == name;
}
void trace(const std::string& name)
{
    std::lock_guard<std::mutex> lock(traceMutex);
    const char* path = std::getenv("UAV_CAPTURE_TEST_TRACE");
    require(path != nullptr);
    std::ofstream output(path, std::ios::app); output << name << '\n';
    require(output.good());
}
int step(const char* name) { trace(name); return fault(name) ? -1 : 0; }

bool grab(const char* stream, hi_u32 width, hi_u32 height,
          const RGBCameraCtl::FrameHandler& callback, int timeout, unsigned seed)
{
    require(threadContext && sysReady && callback && timeout == 200);
    ++activeCalls;
    trace(std::string("get.") + stream);
    if (fault(stream))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        --activeCalls; return false;
    }
    thread_local unsigned sequence = 0;
    const unsigned stride = width + 16;
    thread_local std::vector<unsigned char> buffer;
    buffer.assign(stride * height * 3 / 2, 0xEE);
    ++sequence;
    for (unsigned row = 0; row < height * 3 / 2; ++row)
        std::fill_n(buffer.data() + row * stride, width, (seed + sequence) % 200);
    hi_video_frame_info info{};
    auto& frame = info.v_frame;
    frame.width = fault("bad_thermal_frame") && std::string(stream) == "thermal" ? 384 : width;
    frame.height = height;
    frame.pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    frame.video_format = HI_VIDEO_FORMAT_LINEAR;
    frame.compress_mode = HI_COMPRESS_MODE_NONE;
    frame.width_stride[0] = frame.width_stride[1] = stride;
    frame.virt_addr[0] = buffer.data(); frame.virt_addr[1] = buffer.data() + stride * height;
    frame.pts = sequence * 20000;
    callback(info);
    trace(std::string("release.") + stream);
    --activeCalls;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return true;
}
}

extern "C"
{
aclError aclInit(const char*)
{
    require(!aclReady);
    const auto result = step("acl_init"); aclReady = result == 0; return result;
}
aclError aclrtSetDevice(int32_t device)
{
    require(aclReady && device == 0);
    const auto result = step("device_set"); deviceReady = result == 0; return result;
}
aclError aclrtCreateContext(aclrtContext* context, int32_t device)
{
    require(deviceReady && device == 0);
    const auto result = step("context_create");
    if (result == 0) { *context = &contextToken; contextReady = true; }
    return result;
}
aclError aclrtSetCurrentContext(aclrtContext context)
{
    require(contextReady && context == &contextToken);
    const auto result = step("thread_context"); threadContext = result == 0; return result;
}
hi_s32 hi_mpi_sys_init()
{
    require(contextReady && !sysReady);
    const auto result = step("sys_init"); sysReady = result == 0; return result;
}
hi_s32 hi_mpi_sys_exit()
{
    require(sysReady && !rgbReady && !thermalReady && activeCalls.load() == 0);
    sysReady = false; return step("sys_exit");
}
aclError aclrtDestroyContext(aclrtContext context)
{
    require(contextReady && !sysReady && context == &contextToken && activeCalls.load() == 0);
    contextReady = false; return step("context_destroy");
}
aclError aclrtResetDevice(int32_t device)
{
    require(deviceReady && !contextReady && device == 0);
    deviceReady = false; return step("device_reset");
}
aclError aclFinalize()
{
    require(aclReady && !deviceReady);
    aclReady = false; return step("acl_finalize");
}
}

hi_s32 mipi_ctl::vi_set_mipi_hs_mode(lane_divide_mode_t mode)
{
    require(sysReady && !rgbReady && !thermalReady && mode == LANE_DIVIDE_MODE_1);
    return step("lane_mode");
}

RGBCameraCtl::RGBCameraCtl() = default;
RGBCameraCtl::~RGBCameraCtl() { deinit(); }
bool RGBCameraCtl::init(const Config& config)
{
    require(sysReady && config.viPipe == 2 && config.vpssGrp == 256);
    m_cfg = config; m_inited = rgbReady = step("rgb_init") == 0; return m_inited;
}
void RGBCameraCtl::deinit()
{
    if (!m_inited) return;
    require(activeCalls.load() == 0);
    trace("rgb_deinit"); m_inited = rgbReady = false;
}
bool RGBCameraCtl::getFrame(hi_vpss_chn chn, FrameHandler callback, int timeout)
{
    require(rgbReady && (chn == 0 || chn == 1));
    return grab(chn == 0 ? "rgb0" : "rgb1", chn == 0 ? m_cfg.width : m_cfg.vpssChnCfg[1].width,
                chn == 0 ? m_cfg.height : m_cfg.vpssChnCfg[1].height, callback, timeout, chn == 0 ? 10 : 20);
}

ThermalCameraCtl::~ThermalCameraCtl() { deinit(); }
bool ThermalCameraCtl::init(const Config& config)
{
    require(sysReady && config.width == 640 && config.height == 512 && config.viPipe == 3 && config.vpssGrp == 257);
    require(config.camera.port == "/dev/ttyCH9344USB0" && config.camera.baud == 115200);
    config_ = config; inited_ = thermalReady = step("thermal_init") == 0; return inited_;
}
void ThermalCameraCtl::deinit()
{
    if (!inited_) return;
    require(activeCalls.load() == 0);
    trace("thermal_deinit"); inited_ = thermalReady = false;
}
bool ThermalCameraCtl::getFrame(FrameHandler callback, int timeout)
{
    require(thermalReady);
    return grab("thermal", 640, 512, callback, timeout, 30);
}
