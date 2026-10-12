#pragma once

#include"common/common.h"
#include"camera/rgb_camera.h"
#include"camera/mipi_ctl.h"

#include<atomic>
#include<functional>
#include<memory>
#include<string>

// ---------------------------------------------------------------------------
// RGBCameraCtl —— 可见光相机总控（只管**抓图**和相机参数，不做编码、不做推流）
//
//   世辉相机 ─MIPI(YUV422 8bit)─▶ MIPI RX ─▶ VI ─▶ VPSS ─┬─ chn0 ─▶ YUV 帧 2688x1520（主）
//      ▲                                                  └─ chn1 ─▶ YUV 帧 1920x1080（子）
//      │ 串口控制：变倍 / 聚焦 / 图像 / 曝光 / 白平衡 / ICR / OSD / 抓拍…
//      └──────────────────── RGBCamera（世辉协议，见 rgb_camera.h）
//
// 职责边界（有意为之）：
//   * 本类只把帧“抓出来”交给调用方，编码（VENC）、RTSP/GB28181 推流都不在这里；
//   * 所以没有取流线程：VI/VPSS 起来之后帧是自动产生的，谁需要谁调 getFrame() 拉一帧，
//     零拷贝拿到 hi_video_frame_info（含物理地址，可直接喂 DVPP / NPU），回调返回即释放；
//   * VPSS 一个组里两路输出（尺寸可不同，硬件缩放），取哪路由 getFrame(chn, ...) 指定。
//
// 典型用法：
//   RGBCameraCtl ctl;
//   RGBCameraCtl::Config cfg;                         // 默认值就是 8.11 __MIPI__ 那套：
//                                                     // 2688x1520@50 @dev0 / vi pipe2 chn0 / vpss grp256 chn0
//   if (!ctl.init(cfg)) return -1;                    // 连串口→下发格式→MIPI RX→VI→VPSS
//
//   ctl.getFrame([](const hi_video_frame_info &frame) {
//       // frame.v_frame.virt_addr[0] / phys_addr[0] ... 回调期间有效
//   });
//   ctl.saveFrame("/tmp/shot.yuv");                    // 抓一帧存原始 YUV420SP（NV21）
//   ctl.zoomIn(30); ctl.autoFocus(); ctl.setBrightness(60);
//
//   ctl.deinit();                                      // 拆链路 + 断开串口
//
// init() 的时序：
//   1) 打开串口并 connect
//   2) configureCameraOutput = true 时下发输出格式（MIPI 接口 + 分辨率 + 帧率档位）
//      → 等 formatSettleMs 让相机切完格式
//   3) MIPI RX（combo dev 属性）→ VI（dev/pipe/chn）→ VPSS（组 + USER 通道 + 绑定）
//
// 关于"等相机初始化完成上报(0x38)"：**本模块不等**。实测相机经常不上报 0x38，
// 等它只会白白阻塞（原先还带一轮 setInitialize() 软复位，最坏 100 秒）。
// 现在的做法是串口连上就把命令发下去；如果某台相机确实需要更长时间才就绪，
// 把 configureCameraOutput 置 false、等就绪后再自己调一次 applyCameraOutput() 即可。
// 相机供电同样由外部负责（本模块不做上下电）。
//
// 线程约定：
//   * init / deinit 属生命周期操作，请由同一个控制线程调用；
//   * getFrame / saveFrame 内部只做 hi_mpi_vpss_get_chn_frame + 回调 + release，
//     可从任意线程调用，但并发调用没有意义（同一通道的帧队列是共享的），建议由一个采集线程拉；
//   * 相机参数调节可从任意线程调用（串口收发线程安全），不要和 connect/disconnect 并发。
//
// 依赖：需要昇腾媒体运行环境（libacl_vi_mpi / libacl_vpss_mpi），只能在板卡上跑；
//       宿主机上没有 /dev/hi_mipi_rx，init() 会在那一步失败。
// ---------------------------------------------------------------------------
class RGBCameraCtl
{
public:
    // 抓到的一帧：VPSS 通道输出的 YUV420SP（默认 NV21 = YVU_SEMIPLANAR_420，与 8.11 一致），
    // **回调期间有效**，
    // 回调返回后帧缓冲就被还给 VPSS 了；要留存请自行拷贝或直接用 saveFrame()。
    using FrameHandler = std::function<void(const hi_video_frame_info &frame)>;

    struct Config
    {
        // ---- 采集 ----
        // 这一组默认值取自 8.11 里已经跑通的 __MIPI__ 可见光链路
        //（input_node.cpp:generate_camera_mpp_cfg / mpp_system.cpp:vi_visible_start），
        // 换机型或换接线时改这里即可。
        uint32_t    width    = 2688;   // = VISIBLE_WIDTH，必须与相机实际输出一致
        uint32_t    height   = 1520;   // = VISIBLE_HEIGHT
        uint32_t    fps      = 50;     // 相机输出档位用；50 → 世辉 0x0E(FORMAT_1520_50P)，
                                       // 与 8.11 实际调用的 setVideoFormat(0x0E) 一致
                                       //（配置文件里那个 25 是编码侧帧率，不是相机输出）
        combo_dev_t mipiDev  = 0;      // = stcomboDevAttr.devno
        uint32_t    laneNum  = 4;      // = mipi_attr.lane_id 0~3 全用上

        // ---- 设备 / 通道号 ----
        hi_vi_dev   viDev     = 0;     // = vi_dev
        hi_vi_pipe  viPipe    = 2;     // = vi_cfg.pipe_id（可见光那路）；热成像那路是 3
        hi_vi_chn   viChn     = 0;     // = vi_cfg.vi_chn
        hi_vpss_grp vpssGrp   = 256;   // = VISIBLE_VPSSGRP：__MIPI__ 分支是 256，__VDEC__ 分支才是 0
        hi_u32      pipeDepth = 3;     // = pipe_attr.depth
        hi_u32      chnDepth  = 0;     // = chn_attr.depth（取帧走 VPSS 通道，VI 通道不必缓存）
        hi_bool     vpssOffline = HI_TRUE;   // VI/VPSS 走 offline：帧落 DDR，才能取出来

        // ---- VPSS 输出通道 ----
        // 一个组最多 HI_VPSS_MAX_CHN_NUM(=2) 个通道，每路可以有不同的输出尺寸（VPSS 硬件缩放）。
        // 默认照 8.11 可见光那路：chn0 = 采集尺寸(2688x1520) 主，chn1 = 1920x1080 子。
        // width/height 填 0 表示"跟随采集尺寸"（Config::width/height）。
        struct VpssChnConfig
        {
            bool     enable = false;
            uint32_t width  = 0;
            uint32_t height = 0;
        };
        VpssChnConfig vpssChnCfg[HI_VPSS_MAX_CHN_NUM] = {
            {true,  0,    0   },   // chn0：主通道，跟随采集尺寸（2688x1520）
            {true,  1920, 1080},   // chn1：子通道，例如缩放后喂 detect
        };
        // 默认抓图通道：不带 chn 参数的 getFrame/saveFrame 用它（必须 enable）
        hi_vpss_chn vpssChn = 0;

        // ---- 相机串口（世辉协议）----
        std::string cameraPort = DEFAULT_CAMERA_PORT;
        uint64_t    cameraBaud = DEFAULT_CAMERA_BAUD;
        bool cameraRequired    = false;   // true：串口连不上 / 下发格式失败就让 init() 失败
        bool configureCameraOutput = true;   // true：init() 时下发 MIPI 格式 / 分辨率 / 帧率
                                             //（串口一连上就发，不等 0x38，见下面的说明）

        // ---- 相机准备时序 ----
        // 注意：本模块**不**等相机的"初始化完成"上报(0x38)，也不做上下电 ——
        // 实测 0x38 经常等不到，等它只会白阻塞。串口连上后直接把命令发下去。
        // 如果某台相机需要更长时间才就绪：把 configureCameraOutput 置 false，
        // 等确认就绪后再自己调一次 applyCameraOutput()。
        uint32_t    formatSettleMs = 100;   // 下发格式后等相机切换（8.11 只 sleep 10ms，留点余量）

        // ---- 抓图 ----
        int getFrameTimeoutMs = 1000;   // getFrame 默认等待超时（毫秒）
    };

    // 抓图统计（任意线程可读）
    struct Stats
    {
        uint64_t frames    = 0;   // 成功抓到的帧数
        uint64_t errors    = 0;   // 抓帧失败次数（含正常的等待超时）
        uint64_t lastPtsUs = 0;   // 最后一帧的 pts（微秒，来自 VPSS）
    };

    RGBCameraCtl();
    ~RGBCameraCtl();

    RGBCameraCtl(const RGBCameraCtl &) = delete;
    RGBCameraCtl &operator=(const RGBCameraCtl &) = delete;

    // === 生命周期 ===
    bool init(const Config &cfg);   // 建链路：串口 + MIPI RX + VI + VPSS + 绑定
    void deinit();                  // 拆链路（按相反顺序，幂等）
    bool isInited() const { return m_inited; }
    const Config &config() const { return m_cfg; }

    // === 抓图 ===
    // 拉一帧交给回调；timeoutMs < 0 用 Config::getFrameTimeoutMs。
    // 返回 true = 确实拿到帧且回调已执行；false = 超时/失败（已记日志与统计）。
    // 不带 chn 的版本用 Config::vpssChn（默认 chn0）；带 chn 的版本可以取任意已启用的通道
    //（例如 chn1 那路 1920x1080 直接喂 detect）。
    bool getFrame(FrameHandler handler, int timeoutMs = -1);
    bool getFrame(hi_vpss_chn chn, FrameHandler handler, int timeoutMs = -1);
    // 抓一帧存成文件：原始 YUV420SP，行按 stride 紧凑写出（默认通道是 YVU 平面顺序 = NV21，
    // 与 8.11 的 VPSS 输出一致；若把格式改成 YUV_SEMIPLANAR_420 那就是 NV12）。
    // 看的时候：ffplay -f rawvideo -pixel_format nv21 -video_size 2688x1520 shot.yuv
    bool saveFrame(const std::string &path, int timeoutMs = -1);
    bool saveFrame(hi_vpss_chn chn, const std::string &path, int timeoutMs = -1);
    Stats stats() const;

    // === 相机对象 ===
    RGBCamera *camera() { return m_camera.get(); }
    const RGBCamera *camera() const { return m_camera.get(); }
    bool cameraConnected() const { return m_camera && m_camera->isConnected(); }
    bool reconnectCamera();
    // 主动上报回调（变焦/聚焦位置、初始化完成等）；原样透传给 RGBCamera
    void setReportCallback(RGBCamera::ReportCallback cb) { if (m_camera) m_camera->setReportCallback(std::move(cb)); }
    // 把相机输出配成与 Config 一致（MIPI 接口 + 分辨率 + 标准格式档位）；
    // Config::configureCameraOutput = true 时 init() 会自动调用，也可以手动再调
    bool applyCameraOutput();

    // === 相机参数调节（转发 RGBCamera，true = 成功）===
    // 失败打错误日志；相机未连接时打告警并返回 false，不影响抓图。
    // 这里只包了常用项，其余接口用 camera()->xxx() 直接调。
    // 下面所有 uint8_t 取值范围都是 0~100（另有约定的在注释里写明）。

    // 变倍（speed 0~100，0 = 用相机默认档位）
    bool zoomIn(uint8_t speed = 0)  { return camCall("zoomIn",  [this, speed] { return m_camera->continuousZoom(RGBCamera::ZOOM_DIR_IN,  speed); }); }
    bool zoomOut(uint8_t speed = 0) { return camCall("zoomOut", [this, speed] { return m_camera->continuousZoom(RGBCamera::ZOOM_DIR_OUT, speed); }); }
    bool zoomStop()                 { return camCall("zoomStop", [this] { return m_camera->continuousZoom(RGBCamera::ZOOM_DIR_STOP, 0); }); }
    // 变倍到绝对位置（可见光 0x0000~0x4000）
    bool zoomTo(uint16_t position)  { return camCall("zoomTo", [this, position] { return m_camera->zoomToPosition(position); }); }
    bool zoomPosition(uint16_t &position) { return camCall("zoomPosition", [this, &position] { return m_camera->getZoomPosition(position); }); }
    // 变倍模式：0x03 仅光学 / 0x02 数字+光学
    bool setZoomMode(uint8_t mode)  { return camCall("setZoomMode", [this, mode] { return m_camera->setZoomMode(mode); }); }

    // 聚焦（speed 0~100）
    bool focusNear(uint8_t speed = 0) { return camCall("focusNear", [this, speed] { return m_camera->continuousFocus(RGBCamera::FOCUS_DIR_NEAR, speed); }); }
    bool focusFar(uint8_t speed = 0)  { return camCall("focusFar",  [this, speed] { return m_camera->continuousFocus(RGBCamera::FOCUS_DIR_FAR,  speed); }); }
    bool focusStop()                  { return camCall("focusStop", [this] { return m_camera->continuousFocus(RGBCamera::FOCUS_DIR_STOP, 0); }); }
    bool focusTo(uint16_t position)   { return camCall("focusTo", [this, position] { return m_camera->focusToPosition(position); }); }
    bool focusPosition(uint16_t &position) { return camCall("focusPosition", [this, &position] { return m_camera->getFocusPosition(position); }); }
    bool autoFocus()                  { return camCall("autoFocus", [this] { return m_camera->autoFocus(); }); }
    // 自动对焦模式：0 自动 / 1 半自动 / 2 手动 / 3 快速半自动 / 4 快速自动
    bool setAutoFocusMode(uint8_t mode) { return camCall("setAutoFocusMode", [this, mode] { return m_camera->setAutoFocusMode(mode); }); }
    // 最近聚焦距离限制：0~7（0 最远 / 7 最近）
    bool setFocusLimit(uint16_t limit)  { return camCall("setFocusLimit", [this, limit] { return m_camera->setFocusLimit(limit); }); }
    // 聚焦灵敏度：0 低 / 1 正常 / 2 高
    bool setFocusSensitivity(uint8_t sensitivity) { return camCall("setFocusSensitivity", [this, sensitivity] { return m_camera->setFocusSensitivity(sensitivity); }); }
    // ROI 区域对焦
    bool setRoiFocus(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) { return camCall("setRoiFocus", [this, x1, y1, x2, y2] { return m_camera->setRoiFocus(x1, y1, x2, y2); }); }

    // 图像参数（0~100，内部换算成协议值；mode 是兼容旧接口的遗留参数，当前实现忽略）
    bool setBrightness(uint8_t value, uint8_t mode = 0) { return camCall("setBrightness", [this, mode, value] { return m_camera->setBrightness(mode, value); }); }
    bool setContrast(uint8_t value, uint8_t mode = 0)   { return camCall("setContrast",   [this, mode, value] { return m_camera->setContrast(mode, value); }); }
    bool setSaturation(uint8_t value) { return camCall("setSaturation", [this, value] { return m_camera->setSaturation(value); }); }
    bool setSharpness(uint8_t value, uint8_t mode = 0)  { return camCall("setSharpness",  [this, mode, value] { return m_camera->setSharpness(mode, value); }); }
    bool setGamma(uint8_t value)      { return camCall("setGamma", [this, value] { return m_camera->setGamma(value); }); }
    // 图像风格：0 标准 / 1 华丽 / 2 温和
    bool setImageStyle(uint8_t style) { return camCall("setImageStyle", [this, style] { return m_camera->setImageStyle(style); }); }
    // 图像翻转：RGBCamera::FLIP_*（0 不翻转 / 1 水平 / 2 垂直 / 3 水平+垂直）
    bool setImageFlip(uint8_t mode)   { return camCall("setImageFlip", [this, mode] { return m_camera->setImageFlip(mode); }); }
    bool setVideoFreeze(bool freeze)  { return camCall("setVideoFreeze", [this, freeze] { return m_camera->setVideoFreeze(freeze); }); }
    // 降噪：mode 0 = 2D / 1 = 3D，level 0~100
    bool setNoiseReduction(uint8_t mode, uint8_t level) { return camCall("setNoiseReduction", [this, mode, level] { return m_camera->setNoiseReduction(mode, level); }); }
    // 宽动态：0 关 / 1 开
    bool setWDR(uint8_t mode)         { return camCall("setWDR", [this, mode] { return m_camera->setWDR(mode); }); }
    // 透雾：mode 0 自动 / 1 手动 / 2 关闭，level 0~100
    bool setDefog(uint8_t mode, uint8_t level) { return camCall("setDefog", [this, mode, level] { return m_camera->setDefog(mode, level); }); }
    // 防抖：0 关 / 1 电子防抖
    bool setEIS(uint8_t mode)         { return camCall("setEIS", [this, mode] { return m_camera->setEIS(mode); }); }
    // 强光抑制：0 关 / 其它 开
    bool setHLC(uint8_t mode)         { return camCall("setHLC", [this, mode] { return m_camera->setHLC(mode); }); }

    // 曝光 / 白平衡
    // 曝光模式：0x00 自动 / 0x01 手动 / 0x02 光圈优先 / 0x03 快门优先
    bool setExposureMode(uint8_t mode)  { return camCall("setExposureMode", [this, mode] { return m_camera->setExposureMode(mode); }); }
    bool getExposureMode(uint8_t &mode) { return camCall("getExposureMode", [this, &mode] { return m_camera->getExposureMode(mode); }); }
    // type 0~15：依次为 光圈±/复位/直接、增益±/复位/直接、快门±/复位/直接、补偿±/复位/直接
    bool setExposureControl(uint8_t type, uint8_t value = 0) { return camCall("setExposureControl", [this, type, value] { return m_camera->setExposureControl(type, value); }); }
    bool setAperture(uint8_t aperture)    { return camCall("setAperture", [this, aperture] { return m_camera->setAperture(aperture); }); }
    bool setShutterSpeed(uint8_t speed)   { return camCall("setShutterSpeed", [this, speed] { return m_camera->setShutterSpeed(speed); }); }
    bool setGain(uint8_t gain)            { return camCall("setGain", [this, gain] { return m_camera->setGain(gain); }); }
    // 防闪烁：0 户外(不防闪) / 1 50Hz / 2 60Hz
    bool setAntiFlickerMode(uint8_t mode) { return camCall("setAntiFlickerMode", [this, mode] { return m_camera->setAntiFlickerMode(mode); }); }
    // 白平衡模式：0 自动 / 1 室内 / 2 室外 / 3 ATW / 4 手动 …
    bool setWhiteBalanceMode(uint8_t mode) { return camCall("setWhiteBalanceMode", [this, mode] { return m_camera->setWhiteBalanceMode(mode); }); }
    // 手动白平衡增益（世辉只有 R/B 两路）
    bool setRBGain(uint8_t rGain, uint8_t bGain) { return camCall("setRBGain", [this, rGain, bGain] { return m_camera->setRBgain(rGain, bGain); }); }

    // 日夜（ICR）：0 关 / 其它 开
    bool setICR(uint8_t mode)     { return camCall("setICR", [this, mode] { return m_camera->setICR(mode); }); }
    // 自动 ICR：0x02 开 / 0x03 关
    bool setAutoICR(uint8_t mode) { return camCall("setAutoICR", [this, mode] { return m_camera->setAutoICR(mode); }); }
    bool setAutoICRThreshold(uint8_t threshold) { return camCall("setAutoICRThreshold", [this, threshold] { return m_camera->setAutoICRThreshold(threshold); }); }

    // OSD / 抓拍 / 录像（抓拍是相机自己存卡，与上面的 getFrame/saveFrame 无关）
    bool setOSD(uint8_t mode) { return camCall("setOSD", [this, mode] { return m_camera->setOSD(mode); }); }
    bool snap(uint8_t count = 1, uint8_t delay = 0) { return camCall("snap", [this, count, delay] { return m_camera->setSnap(count, delay); }); }
    bool snapOnePush()        { return camCall("snapOnePush", [this] { return m_camera->setSnapOnePush(); }); }
    bool setRecord(bool start) { return camCall("setRecord", [this, start] { return m_camera->setRecord(start); }); }

    // 系统
    bool cameraInitialize() { return camCall("cameraInitialize", [this] { return m_camera->setInitialize(); }); }
    bool imageInitialize()  { return camCall("imageInitialize", [this] { return m_camera->setImageInitialize(); }); }
    bool powerOff()         { return camCall("powerOff", [this] { return m_camera->setPowerOff(); }); }

private:
    // 统一的相机调用包装：查连接 → 调用 → 失败记日志
    template <typename Fn>
    bool camCall(const char *what, Fn &&fn)
    {
        if (!m_camera)
        {
            return false;
        }
        if (!m_camera->isConnected())
        {
            UAV_LOG_WARN_F("camera not connected, %s ignored\n", what);
            return false;
        }
        const int32_t ret = fn();
        if (ret != 1)
        {
            UAV_LOG_ERROR_F("%s failed (ret=%d)\n", what, ret);
            return false;
        }
        return true;
    }

    bool prepareCamera();  // 串口连接 + 下发输出格式
    bool buildMipi();      // MIPI RX 物理层（combo dev 属性）
    bool buildVi();        // VI dev → pipe → chn
    bool buildVpss();      // VPSS 组 + 各通道 + VI→VPSS 绑定
    bool buildPipeline();  // 上面 4 步串起来
    void teardown();       // 按相反顺序拆链路 + 串口断开（幂等）

    // 指定通道抓一帧（getFrame/saveFrame 的内部实现）
    bool getFrameOn(hi_vpss_chn chn, const FrameHandler &handler, int timeoutMs);

    Config m_cfg;

    std::unique_ptr<RGBCamera> m_camera;

    mipi_ctl::vi_cfg                    m_viCfg{};
    mipi_ctl::sample_venc_vpss_chn_attr m_vpssCfg{};

    bool m_viInited   = false;
    bool m_vpssInited = false;
    bool m_viBound    = false;
    bool m_inited     = false;

    std::atomic<uint64_t> m_frames{0};
    std::atomic<uint64_t> m_errors{0};
    std::atomic<uint64_t> m_lastPtsUs{0};
};
