#include"camera/rgb_camera_ctl.h"

#include"log/log.h"

#include<chrono>
#include<cstring>
#include<fstream>
#include<thread>

// ===========================================================================
// 文件内辅助工具
// ===========================================================================
namespace
{
// （宽, 高, 帧率）→ 世辉视频格式档位；没有对应档位返回 0
uint8_t videoFormatOf(uint32_t width, uint32_t height, uint32_t fps)
{
    struct Entry
    {
        uint32_t width;
        uint32_t height;
        uint32_t fps;
        uint8_t  format;
    };
    static const Entry kTable[] = {
        {2688, 1520, 25, FORMAT_1520_25P}, {2688, 1520, 30, FORMAT_1520_30P},
        {2688, 1520, 50, FORMAT_1520_50P}, {2688, 1520, 60, FORMAT_1520_60P},
        {1920, 1080, 25, FORMAT_1080_25P}, {1920, 1080, 30, FORMAT_1080_30P},
        {1920, 1080, 50, FORMAT_1080_50P}, {1920, 1080, 60, FORMAT_1080_60P},
        {1280,  720, 50, FORMAT_720_50P},  {1280,  720, 60, FORMAT_720_60P},
    };
    for (const auto &entry : kTable)
    {
        if (entry.width == width && entry.height == height && entry.fps == fps)
        {
            return entry.format;
        }
    }
    return 0;
}

// 按行写一个平面（stride 可能大于 width，要逐行写才能去掉行尾填充）
bool writePlane(std::ofstream &ofs, const void *virtAddr, uint32_t width, uint32_t rows, uint32_t stride)
{
    if (virtAddr == nullptr || width == 0 || rows == 0 || stride < width)
    {
        return false;
    }
    const uint8_t *src = static_cast<const uint8_t *>(virtAddr);
    for (uint32_t row = 0; row < rows; ++row)
    {
        ofs.write(reinterpret_cast<const char *>(src), width);
        src += stride;
    }
    return ofs.good();
}

// ---- 配置自检 --------------------------------------------------------------
// 提前挡住"配置本身不成立"的情况，免得后面只拿到一句看不懂的 SDK 错误码
const char *checkConfig(const RGBCameraCtl::Config &cfg)
{
    if (cfg.width == 0 || cfg.height == 0)
    {
        return "width/height 不能为 0";
    }
    if (cfg.laneNum == 0 || cfg.laneNum > MIPI_LANE_NUM)
    {
        return "laneNum 必须在 1~8（MIPI_LANE_NUM）";
    }
    if (cfg.vpssChn < 0 || cfg.vpssChn >= static_cast<hi_vpss_chn>(HI_VPSS_MAX_CHN_NUM))
    {
        return "vpssChn 越界（VPSS 只有 2 个通道：0/1）";
    }
    if (!cfg.vpssChnCfg[cfg.vpssChn].enable)
    {
        return "默认抓图通道 vpssChn 没启用（vpssChnCfg[vpssChn].enable = false）";
    }
    {
        // 至少要启用一个通道，否则组建出来也没有出口
        bool anyEnabled = false;
        for (hi_vpss_chn chn = 0; chn < static_cast<hi_vpss_chn>(HI_VPSS_MAX_CHN_NUM); ++chn)
        {
            if (cfg.vpssChnCfg[chn].enable)
            {
                anyEnabled = true;
                break;
            }
        }
        if (!anyEnabled)
        {
            return "vpssChnCfg 里一个通道都没启用";
        }
    }
    if (cfg.getFrameTimeoutMs <= 0)
    {
        return "getFrameTimeoutMs 必须 > 0";
    }
    if (cfg.vpssOffline != HI_TRUE)
    {
        // online 时帧不进 DDR，user 通道取不到
        return "vpssOffline 必须为 HI_TRUE：抓图要求帧落 DDR（online 模式取不到帧）";
    }
    return nullptr;
}
}   // namespace

// ===========================================================================
// RGBCameraCtl
// ===========================================================================
RGBCameraCtl::RGBCameraCtl()
    : m_camera(std::make_unique<RGBCamera>())
{
}

RGBCameraCtl::~RGBCameraCtl()
{
    teardown();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
bool RGBCameraCtl::init(const Config &cfg)
{
    if (m_inited)
    {
        UAV_LOG_WARN_F("RGBCameraCtl 已经 init 过，忽略本次调用\n");
        return true;
    }

    if (const char *err = checkConfig(cfg))
    {
        UAV_LOG_ERROR_F("配置不合法：%s\n", err);
        return false;
    }
    m_cfg = cfg;

    if (!buildPipeline())
    {
        UAV_LOG_ERROR_F("抓图链路建立失败，回滚\n");
        teardown();
        return false;
    }

    m_inited = true;
    UAV_LOG_INFO_F("抓图链路就绪：%ux%u，vi %d/%d → vpss %d/%d（取图用 getFrame/saveFrame）\n",
                   m_cfg.width, m_cfg.height,
                   static_cast<int>(m_cfg.viPipe), static_cast<int>(m_cfg.viChn),
                   static_cast<int>(m_cfg.vpssGrp), static_cast<int>(m_cfg.vpssChn));
    return true;
}

bool RGBCameraCtl::buildPipeline()
{
    return prepareCamera() && buildMipi() && buildVi() && buildVpss();
}

void RGBCameraCtl::deinit()
{
    teardown();
}

void RGBCameraCtl::teardown()
{
    const bool anything = m_inited || m_viInited || m_vpssInited || m_viBound ||
                          (m_camera && m_camera->isConnected());
    if (!anything)
    {
        return;
    }

    UAV_LOG_INFO_F("开始拆抓图链路\n");

    // 拆的顺序和建的时候完全相反
    if (m_viBound)
    {
        mipi_ctl::vi_un_bind_vpss(m_cfg.viPipe, m_cfg.viChn, m_cfg.vpssGrp, m_cfg.vpssChn);
        m_viBound = false;
    }
    if (m_vpssInited)
    {
        mipi_ctl::venc_vpss_deinit(m_cfg.vpssGrp, &m_vpssCfg);
        m_vpssInited = false;
    }
    if (m_viInited)
    {
        mipi_ctl::vi_deinit(&m_viCfg);
        m_viInited = false;
    }
    if (m_camera && m_camera->isConnected())
    {
        m_camera->disconnect();
    }

    m_inited = false;
    UAV_LOG_INFO_F("抓图链路已拆除\n");
}

// ---------------------------------------------------------------------------
// 相机准备：串口连接 → 下发输出格式
//（相机供电由外部负责；不等 0x38，理由见下面 applyCameraOutput 上方的说明）
// ---------------------------------------------------------------------------
bool RGBCameraCtl::prepareCamera()
{
    // 1) 串口连接
    // m_camera->init() 只是按新串口/波特率重建内核；串口不存在时这里会失败，交给 connect() 一并处理
    m_camera->init(m_cfg.cameraPort, m_cfg.cameraBaud);

    if (!m_camera->connect())
    {
        if (m_cfg.cameraRequired)
        {
            UAV_LOG_ERROR_F("相机控制串口连不上：%s@%llu\n",
                            m_cfg.cameraPort.c_str(),
                            static_cast<unsigned long long>(m_cfg.cameraBaud));
            return false;
        }
        // 抓图和相机控制是两条独立通路：串口挂了也要能抓到图，只是参数调不了
        UAV_LOG_WARN_F("相机控制串口未连上（%s@%llu）：抓图继续，参数调节不可用\n",
                       m_cfg.cameraPort.c_str(),
                       static_cast<unsigned long long>(m_cfg.cameraBaud));
        return true;
    }

    UAV_LOG_INFO_F("相机控制已连接：%s@%llu\n",
                   m_cfg.cameraPort.c_str(),
                   static_cast<unsigned long long>(m_cfg.cameraBaud));

    if (!m_cfg.configureCameraOutput)
    {
        UAV_LOG_INFO_F("跳过下发相机输出格式（configureCameraOutput = false）\n");
        return true;
    }

    // 2) 下发输出格式（MIPI 接口 + 分辨率 + 帧率档位），再给相机一点切换时间
    //    不等相机 0x38：实测经常等不到，白阻塞；命令直接发，需要时外面再调 applyCameraOutput()
    if (!applyCameraOutput())
    {
        UAV_LOG_ERROR_F("下发相机输出格式失败\n");
        if (m_cfg.cameraRequired)
        {
            return false;
        }
    }
    else if (m_cfg.formatSettleMs > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_cfg.formatSettleMs));
    }
    return true;
}

// 说明：为什么不等相机的「初始化完成」上报(0x38)
//   8.11 的做法是"上下电 → 等 0x38（最多 50s）→ 再下发格式"，但实测 0x38 经常等不到，
//   等它只会白阻塞（原先还带一轮 setInitialize() 软复位，最坏 100 秒）。
//   本模块改成：串口一连上就把命令发下去。若某台相机确实需要更久才就绪，把
//   configureCameraOutput 置 false，等确认就绪后再自己调 applyCameraOutput()。
//   想观察 0x38 就用 setReportCallback()（RGBCamera 的上报回调原样透传）。

bool RGBCameraCtl::applyCameraOutput()
{
    if (!cameraConnected())
    {
        UAV_LOG_WARN_F("applyCameraOutput：相机未连接\n");
        return false;
    }

    bool ok = true;

    // 输出接口：1 = MIPI（0 = LVDS/BT.1120）
    ok = camCall("setFormat(MIPI)", [this] { return m_camera->setFormat(1); }) && ok;

    // 分辨率（世辉单独的分辨率设置项）
    ok = camCall("setHorizontalResolution",
                 [this] { return m_camera->setHorizontalResolution(m_cfg.width); }) && ok;
    ok = camCall("setVerticalResolution",
                 [this] { return m_camera->setVerticalResolution(m_cfg.height); }) && ok;

    // 标准视频格式档位（分辨率 + 帧率）；没有对应档位就跳过，由用户自己设
    const uint8_t format = videoFormatOf(m_cfg.width, m_cfg.height, m_cfg.fps);
    if (format != 0)
    {
        ok = camCall("setVideoFormat", [this, format] { return m_camera->setVideoFormat(format); }) && ok;
    }
    else
    {
        UAV_LOG_WARN_F("世辉没有 %ux%u@%u 的标准档位，跳过 setVideoFormat\n",
                       m_cfg.width, m_cfg.height, m_cfg.fps);
    }
    return ok;
}

bool RGBCameraCtl::buildMipi()
{
    combo_dev_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.devno                     = m_cfg.mipiDev;
    attr.input_mode                = INPUT_MODE_MIPI;
    attr.data_rate                 = MIPI_DATA_RATE_X1;
    attr.img_rect.x                = 0;
    attr.img_rect.y                = 0;
    attr.img_rect.width            = m_cfg.width;
    attr.img_rect.height           = m_cfg.height;
    attr.mipi_attr.input_data_type = DATA_TYPE_YUV422_8BIT;
    attr.mipi_attr.wdr_mode        = HI_MIPI_WDR_MODE_NONE;
    for (int i = 0; i < MIPI_LANE_NUM; ++i)
    {
        attr.mipi_attr.lane_id[i] = (i < static_cast<int>(m_cfg.laneNum))
                                        ? static_cast<short>(i)
                                        : static_cast<short>(-1);
    }

    UAV_LOG_INFO_F("MIPI dev %d：YUV422 8bit，%ux%u，%u lane\n",
                   static_cast<int>(m_cfg.mipiDev), m_cfg.width, m_cfg.height, m_cfg.laneNum);

    // 顺序照海思的做法：给时钟 → 复位 → 下发 dev 属性 → 解复位
    if (mipi_ctl::vi_enable_mipi_clock(m_cfg.mipiDev) != HI_SUCCESS)
    {
        UAV_LOG_WARN_F("vi_enable_mipi_clock 失败（相机可能自带时钟）\n");
    }
    if (mipi_ctl::vi_reset_mipi(m_cfg.mipiDev) != HI_SUCCESS)
    {
        UAV_LOG_WARN_F("vi_reset_mipi 失败\n");
    }
    if (mipi_ctl::vi_set_mipi_attr(attr) != HI_SUCCESS)
    {
        UAV_LOG_ERROR_F("vi_set_mipi_attr 失败（先确认 %s 存在、参数是否合法）\n", MIPI_DEV_NODE);
        return false;
    }
    if (mipi_ctl::vi_unreset_mipi(m_cfg.mipiDev) != HI_SUCCESS)
    {
        UAV_LOG_ERROR_F("vi_unreset_mipi 失败\n");
        return false;
    }
    return true;
}

bool RGBCameraCtl::buildVi()
{
    m_viCfg = {};
    m_viCfg.vi_dev      = m_cfg.viDev;
    m_viCfg.pipe        = m_cfg.viPipe;
    m_viCfg.chn         = m_cfg.viChn;
    m_viCfg.size.width  = m_cfg.width;
    m_viCfg.size.height = m_cfg.height;
    // 通道输出格式：与 8.11 的可见光链路保持一致（YVU_SEMIPLANAR_420 = NV21）
    m_viCfg.chn_pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    m_viCfg.pipe_depth       = m_cfg.pipeDepth;
    m_viCfg.chn_depth        = m_cfg.chnDepth;
    m_viCfg.vpss_offline     = m_cfg.vpssOffline;

    if (mipi_ctl::vi_init(&m_viCfg) != HI_SUCCESS)
    {
        UAV_LOG_ERROR_F("vi_init 失败（vi dev %d，pipe %d，chn %d）\n",
                        static_cast<int>(m_cfg.viDev), static_cast<int>(m_cfg.viPipe),
                        static_cast<int>(m_cfg.viChn));
        return false;
    }
    m_viInited = true;
    return true;
}

bool RGBCameraCtl::buildVpss()
{
    // 组级最大尺寸：取"采集尺寸"和"各启用通道输出尺寸"的较大者，保证组够大。
    // （mipi_ctl::vpss_init 现在真的会用 max_size 当组属性了）
    uint32_t maxWidth  = m_cfg.width;
    uint32_t maxHeight = m_cfg.height;

    m_vpssCfg = {};
    // 与 8.11 一致：vpss_stream_param.pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420
    m_vpssCfg.pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;

    for (hi_vpss_chn chn = 0; chn < static_cast<hi_vpss_chn>(HI_VPSS_MAX_CHN_NUM); ++chn)
    {
        const Config::VpssChnConfig &chnCfg = m_cfg.vpssChnCfg[chn];
        const uint32_t width  = (chnCfg.width  != 0) ? chnCfg.width  : m_cfg.width;
        const uint32_t height = (chnCfg.height != 0) ? chnCfg.height : m_cfg.height;

        m_vpssCfg.output_size[chn].width  = width;
        m_vpssCfg.output_size[chn].height = height;
        m_vpssCfg.compress_mode[chn]      = HI_COMPRESS_MODE_NONE;
        m_vpssCfg.enable[chn]             = chnCfg.enable ? HI_TRUE : HI_FALSE;

        if (chnCfg.enable)
        {
            maxWidth  = (width  > maxWidth)  ? width  : maxWidth;
            maxHeight = (height > maxHeight) ? height : maxHeight;
            UAV_LOG_INFO_F("vpss grp %d chn %d：%ux%u（已启用）\n",
                           static_cast<int>(m_cfg.vpssGrp), static_cast<int>(chn),
                           width, height);
        }
    }
    m_vpssCfg.max_size.width  = maxWidth;
    m_vpssCfg.max_size.height = maxHeight;

    if (mipi_ctl::vpss_init(m_cfg.vpssGrp, &m_vpssCfg) != HI_SUCCESS)
    {
        UAV_LOG_ERROR_F("vpss_init 失败（grp %d）\n", static_cast<int>(m_cfg.vpssGrp));
        return false;
    }
    m_vpssInited = true;

    // VI → VPSS 绑定：目的通道用第一个已启用的（默认就是 chn0），和 8.11 的
    // vi_bind_vpss(..., vpss_cfg.chn_arr[0]) 一致
    hi_vpss_chn bindChn = 0;
    for (hi_vpss_chn chn = 0; chn < static_cast<hi_vpss_chn>(HI_VPSS_MAX_CHN_NUM); ++chn)
    {
        if (m_cfg.vpssChnCfg[chn].enable)
        {
            bindChn = chn;
            break;
        }
    }

    if (mipi_ctl::vi_bind_vpss(m_cfg.viPipe, m_cfg.viChn,
                               m_cfg.vpssGrp, bindChn) != HI_SUCCESS)
    {
        UAV_LOG_ERROR_F("vi_bind_vpss 失败（vi %d/%d → vpss %d/%d）\n",
                        static_cast<int>(m_cfg.viPipe), static_cast<int>(m_cfg.viChn),
                        static_cast<int>(m_cfg.vpssGrp), static_cast<int>(bindChn));
        return false;
    }
    m_viBound = true;
    return true;
}

// ---------------------------------------------------------------------------
// 抓图
// ---------------------------------------------------------------------------
bool RGBCameraCtl::getFrame(FrameHandler handler, int timeoutMs)
{
    return getFrameOn(m_cfg.vpssChn, handler, timeoutMs);
}

bool RGBCameraCtl::getFrame(hi_vpss_chn chn, FrameHandler handler, int timeoutMs)
{
    return getFrameOn(chn, handler, timeoutMs);
}

bool RGBCameraCtl::getFrameOn(hi_vpss_chn chn, const FrameHandler &handler, int timeoutMs)
{
    if (!m_inited)
    {
        UAV_LOG_ERROR_F("getFrame() 之前必须先 init()\n");
        return false;
    }
    if (!handler)
    {
        UAV_LOG_ERROR_F("getFrame() 需要一个有效的回调\n");
        return false;
    }
    if (chn < 0 || chn >= static_cast<hi_vpss_chn>(HI_VPSS_MAX_CHN_NUM) ||
        !m_cfg.vpssChnCfg[chn].enable)
    {
        UAV_LOG_ERROR_F("getFrame()：VPSS 通道 %d 没启用\n", static_cast<int>(chn));
        return false;
    }

    const int timeout = (timeoutMs < 0) ? m_cfg.getFrameTimeoutMs : timeoutMs;

    hi_video_frame_info frameInfo;
    memset(&frameInfo, 0, sizeof(frameInfo));

    const hi_s32 ret = hi_mpi_vpss_get_chn_frame(m_cfg.vpssGrp, chn, &frameInfo, timeout);
    if (ret != HI_SUCCESS)
    {
        // 超时（没有新帧）与真正的失败在这里都会返回非 0；只有连续失败才升级成告警
        const uint64_t errors = m_errors.fetch_add(1) + 1;
        if (errors == 1 || errors % 100 == 0)
        {
            UAV_LOG_WARN_F("hi_mpi_vpss_get_chn_frame(vpss %d/%d) 失败 %llu 次，最后一次 %#x\n",
                           static_cast<int>(m_cfg.vpssGrp), static_cast<int>(chn),
                           static_cast<unsigned long long>(errors), ret);
        }
        return false;
    }

    // 交给调用方用，回调返回后立刻还给 VPSS（所以回调里不要留存这块内存）
    handler(frameInfo);

    hi_mpi_vpss_release_chn_frame(m_cfg.vpssGrp, chn, &frameInfo);

    m_lastPtsUs.store(frameInfo.v_frame.pts);
    m_frames.fetch_add(1);
    return true;
}

bool RGBCameraCtl::saveFrame(const std::string &path, int timeoutMs)
{
    return saveFrame(m_cfg.vpssChn, path, timeoutMs);
}

bool RGBCameraCtl::saveFrame(hi_vpss_chn chn, const std::string &path, int timeoutMs)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs)
    {
        UAV_LOG_ERROR_F("saveFrame：打不开文件 %s\n", path.c_str());
        return false;
    }

    uint32_t writtenWidth  = 0;
    uint32_t writtenHeight = 0;
    bool     wrote         = false;

    const bool got = getFrame(
        chn,
        [&](const hi_video_frame_info &frameInfo)
        {
            const hi_video_frame &frame = frameInfo.v_frame;
            if (frame.pixel_format != HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420 &&
                frame.pixel_format != HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420)
            {
                UAV_LOG_ERROR_F("saveFrame：只支持 YUV420SP（nv12/nv21），当前 pixel_format=%d\n",
                                static_cast<int>(frame.pixel_format));
                return;
            }
            // Y 平面 + UV 交错平面（UV 平面高度是 height/2，每行 width 字节）
            const bool okY  = writePlane(ofs, frame.virt_addr[0], frame.width,
                                         frame.height, frame.width_stride[0]);
            const bool okUV = writePlane(ofs, frame.virt_addr[1], frame.width,
                                         frame.height / 2, frame.width_stride[1]);
            wrote         = okY && okUV;
            writtenWidth  = frame.width;
            writtenHeight = frame.height;
            if (!wrote)
            {
                UAV_LOG_ERROR_F("saveFrame：写 %s 失败\n", path.c_str());
            }
        },
        timeoutMs);

    if (!got)
    {
        return false;
    }

    UAV_LOG_INFO_F("saveFrame：已写出 %s（vpss chn %d，%ux%u YUV420SP）\n",
                   path.c_str(), static_cast<int>(chn), writtenWidth, writtenHeight);
    return wrote;
}

RGBCameraCtl::Stats RGBCameraCtl::stats() const
{
    Stats snapshot;
    snapshot.frames    = m_frames.load();
    snapshot.errors    = m_errors.load();
    snapshot.lastPtsUs = m_lastPtsUs.load();
    return snapshot;
}

bool RGBCameraCtl::reconnectCamera()
{
    if (!m_camera)
    {
        return false;
    }
    if (m_camera->reconnect())
    {
        UAV_LOG_INFO_F("相机控制已重连\n");
        return true;
    }
    UAV_LOG_ERROR_F("相机控制重连失败\n");
    return false;
}
