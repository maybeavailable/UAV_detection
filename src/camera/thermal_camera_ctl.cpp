#include "camera/thermal_camera_ctl.h"
#include "log/log.h"

#include <exception>
#include <fstream>

namespace
{
bool validConfig(const ThermalCameraCtl::Config& config)
{
    return config.width == ThermalCameraCtl::kWidth && config.height == ThermalCameraCtl::kHeight &&
        config.mipiDev < MIPI_RX_MAX_DEV_NUM &&
        config.viDev >= 0 && config.viDev < HI_VI_MAX_DEV_NUM &&
        static_cast<combo_dev_t>(config.viDev) == config.mipiDev &&
        config.viPipe >= 0 && config.viPipe < HI_VI_MAX_PHY_PIPE_NUM &&
        config.viChn == 0 && // 此链路只使用 VI 物理通道 0
        config.vpssGrp >= 0 && static_cast<hi_u32>(config.vpssGrp) < HI_VPSS_MAX_GRP_NUM &&
        config.pipeDepth > 0 && config.pipeDepth <= 16 && config.chnDepth <= 8 &&
        config.getFrameTimeoutMs > 0 &&
        config.laneIds[0] >= 0 && config.laneIds[0] < MIPI_LANE_NUM &&
        config.laneIds[1] >= 0 && config.laneIds[1] < MIPI_LANE_NUM &&
        config.laneIds[0] != config.laneIds[1];
}

bool mediaSuccess(hi_s32 result, const char* operation)
{
    if (result == HI_SUCCESS) return true;
    UAV_LOG_ERROR_F("ThermalCameraCtl: %s failed: %#x", operation, static_cast<unsigned>(result));
    return false;
}
}

ThermalCameraCtl::~ThermalCameraCtl() { deinit(); }

bool ThermalCameraCtl::init(const Config& config)
{
    if (inited_)
    {
        UAV_LOG_ERROR_F("ThermalCameraCtl: deinit before reinitializing");
        return false;
    }
    if (!validConfig(config))
    {
        UAV_LOG_ERROR_F("ThermalCameraCtl: invalid 640x512/single-channel capture configuration");
        return false;
    }
    // ThermalCamera 同时校验串口配置；即使串口非必需，非法配置仍不能忽略。
    if (!camera_.init(config.camera))
    {
        UAV_LOG_ERROR_F("ThermalCameraCtl: invalid serial configuration");
        return false;
    }
    config_ = config;
    frames_.store(0);
    errors_.store(0);
    lastPtsUs_.store(0);
    if (!camera_.connect())
    {
        if (config_.cameraRequired)
        {
            UAV_LOG_ERROR_F("ThermalCameraCtl: required camera serial connection failed");
            deinit();
            return false;
        }
        UAV_LOG_WARN_F("ThermalCameraCtl: serial offline, continuing video capture");
    }
    // 艾睿协议没有此 MIPI 输出的分辨率/帧率设置，不下发可见光的格式指令。
    if (!buildMipi() || !buildVi() || !buildVpss())
    {
        deinit();
        return false;
    }
    inited_ = true;
    UAV_LOG_INFO_F("ThermalCameraCtl: 640x512 NV21 ready, VI %d/%d -> VPSS %d/0",
                   config_.viPipe, config_.viChn, config_.vpssGrp);
    return true;
}

bool ThermalCameraCtl::buildMipi()
{
    if (!mediaSuccess(mipi_ctl::vi_enable_mipi_clock(config_.mipiDev), "enable MIPI clock")) return false;
    mipiClockEnabled_ = true;
    if (!mediaSuccess(mipi_ctl::vi_reset_mipi(config_.mipiDev), "reset MIPI")) return false;

    combo_dev_attr_t attr{};
    attr.devno = config_.mipiDev;
    attr.input_mode = INPUT_MODE_MIPI;
    attr.data_rate = MIPI_DATA_RATE_X1;
    attr.img_rect.width = kWidth;
    attr.img_rect.height = kHeight;
    attr.mipi_attr.input_data_type = DATA_TYPE_YUV422_8BIT;
    attr.mipi_attr.wdr_mode = HI_MIPI_WDR_MODE_NONE;
    for (auto& lane : attr.mipi_attr.lane_id) lane = -1;
    attr.mipi_attr.lane_id[0] = config_.laneIds[0];
    attr.mipi_attr.lane_id[1] = config_.laneIds[1];
    return mediaSuccess(mipi_ctl::vi_set_mipi_attr(attr), "set MIPI attributes") &&
        mediaSuccess(mipi_ctl::vi_unreset_mipi(config_.mipiDev), "unreset MIPI");
}

bool ThermalCameraCtl::buildVi()
{
    viConfig_ = {};
    viConfig_.vi_dev = config_.viDev;
    viConfig_.pipe = config_.viPipe;
    viConfig_.chn = config_.viChn;
    viConfig_.size = {kWidth, kHeight};
    viConfig_.chn_pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    viConfig_.pipe_depth = config_.pipeDepth;
    viConfig_.chn_depth = config_.chnDepth;
    viConfig_.vpss_offline = HI_TRUE;
    if (!mediaSuccess(mipi_ctl::vi_init(&viConfig_), "start VI")) return false;
    viStarted_ = true;
    return true;
}

bool ThermalCameraCtl::buildVpss()
{
    vpssConfig_ = {};
    vpssConfig_.max_size = {kWidth, kHeight};
    vpssConfig_.pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    vpssConfig_.output_size[kVpssChn] = {kWidth, kHeight};
    vpssConfig_.compress_mode[kVpssChn] = HI_COMPRESS_MODE_NONE;
    vpssConfig_.enable[kVpssChn] = HI_TRUE;
    if (!mediaSuccess(mipi_ctl::vpss_init(config_.vpssGrp, &vpssConfig_), "start VPSS")) return false;
    vpssStarted_ = true;
    if (!mediaSuccess(mipi_ctl::vi_bind_vpss(config_.viPipe, config_.viChn, config_.vpssGrp, kVpssChn),
                      "bind VI to VPSS")) return false;
    bound_ = true;
    return true;
}

void ThermalCameraCtl::deinit()
{
    inited_ = false;
    if (bound_)
    {
        mediaSuccess(mipi_ctl::vi_un_bind_vpss(config_.viPipe, config_.viChn, config_.vpssGrp, kVpssChn),
                     "unbind VI from VPSS");
        bound_ = false;
    }
    if (vpssStarted_)
    {
        mipi_ctl::venc_vpss_deinit(config_.vpssGrp, &vpssConfig_);
        vpssStarted_ = false;
    }
    if (viStarted_)
    {
        mediaSuccess(mipi_ctl::vi_deinit(&viConfig_), "stop VI");
        viStarted_ = false;
    }
    if (mipiClockEnabled_)
    {
        mediaSuccess(mipi_ctl::vi_reset_mipi(config_.mipiDev), "reset MIPI on shutdown");
        mediaSuccess(mipi_ctl::vi_disable_mipi_clock(config_.mipiDev), "disable MIPI clock");
        mipiClockEnabled_ = false;
    }
    // 接收线程退出后 isConnected 可能已为 false，仍需关闭遗留 fd 并回收线程。
    camera_.disconnect();
}

bool ThermalCameraCtl::getFrame(FrameHandler handler, int timeoutMs)
{
    if (!inited_ || !handler)
    {
        ++errors_;
        UAV_LOG_ERROR_F("ThermalCameraCtl: getFrame requires initialization and a callback");
        return false;
    }
    hi_video_frame_info frame{};
    const int timeout = timeoutMs < 0 ? config_.getFrameTimeoutMs : timeoutMs;
    const auto result = hi_mpi_vpss_get_chn_frame(config_.vpssGrp, kVpssChn, &frame, timeout);
    if (result != HI_SUCCESS)
    {
        const auto count = errors_.fetch_add(1) + 1;
        if (count == 1 || count % 100 == 0)
            UAV_LOG_WARN_F("ThermalCameraCtl: get frame failed: %#x, errors=%llu",
                           static_cast<unsigned>(result), static_cast<unsigned long long>(count));
        return false;
    }
    const auto pts = frame.v_frame.pts;
    bool handled = true;
    try { handler(frame); }
    catch (const std::exception& error)
    {
        handled = false;
        UAV_LOG_ERROR_F("ThermalCameraCtl: frame callback threw: %s", error.what());
    }
    catch (...)
    {
        handled = false;
        UAV_LOG_ERROR_F("ThermalCameraCtl: frame callback threw an unknown exception");
    }
    const bool released = mediaSuccess(
        hi_mpi_vpss_release_chn_frame(config_.vpssGrp, kVpssChn, &frame), "release frame");
    if (!handled || !released)
    {
        ++errors_;
        return false;
    }
    lastPtsUs_.store(pts);
    ++frames_;
    return true;
}

bool ThermalCameraCtl::saveFrame(const std::string& path, int timeoutMs)
{
    if (!inited_ || path.empty()) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        UAV_LOG_ERROR_F("ThermalCameraCtl: cannot open %s", path.c_str());
        return false;
    }
    bool written = false;
    const bool grabbed = getFrame([&](const hi_video_frame_info& info) {
        const auto& frame = info.v_frame;
        if ((frame.pixel_format != HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420 &&
             frame.pixel_format != HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420) ||
            frame.width != kWidth || frame.height != kHeight ||
            frame.compress_mode != HI_COMPRESS_MODE_NONE || frame.video_format != HI_VIDEO_FORMAT_LINEAR ||
            !frame.virt_addr[0] || !frame.virt_addr[1] ||
            frame.width_stride[0] < frame.width || frame.width_stride[1] < frame.width)
        {
            UAV_LOG_ERROR_F("ThermalCameraCtl: saveFrame requires mapped, linear 640x512 NV21/NV12 planes");
            return;
        }
        for (unsigned plane = 0; plane < 2; ++plane)
        {
            const auto* data = static_cast<const char*>(frame.virt_addr[plane]);
            const auto rows = plane == 0 ? frame.height : frame.height / 2;
            for (hi_u32 row = 0; row < rows; ++row)
            {
                output.write(data + static_cast<std::size_t>(row) * frame.width_stride[plane], frame.width);
                if (!output) return;
            }
        }
        written = true;
    }, timeoutMs);
    output.close(); // close/flush 的错误同样不能当成保存成功
    if (!grabbed || !written || !output)
    {
        UAV_LOG_ERROR_F("ThermalCameraCtl: failed to save frame to %s", path.c_str());
        return false;
    }
    return true;
}

ThermalCameraCtl::Stats ThermalCameraCtl::stats() const
{
    return {frames_.load(), errors_.load(), lastPtsUs_.load()};
}

bool ThermalCameraCtl::reconnectCamera()
{
    return camera_.reconnect();
}

bool ThermalCameraCtl::camCall(const char* name, const std::function<std::int32_t()>& operation)
{
    if (operation() == 1) return true;
    UAV_LOG_WARN_F("ThermalCameraCtl: camera command %s failed, error=%d", name,
                   static_cast<int>(camera_.lastResult().error));
    return false;
}
