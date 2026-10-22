#include"log/log.h"

#include<atomic>
#include<charconv>
#include<chrono>
#include<csignal>
#include<cstdlib>
#include<fstream>
#include<functional>
#include<iostream>
#include<optional>
#include<string>
#include<thread>

#if defined(UAV_HAS_GATEWAY)
#include"gateway/gateway.h"
#endif

#if defined(UAV_HAS_ASCEND)
#include"camera/rgb_camera_ctl.h"
#include"camera/thermal_camera_ctl.h"
#endif
#if defined(UAV_HAS_MEDIA_SERVICE)
#include"media/ascend_media_device.h"
#if defined(UAV_HAS_GATEWAY)
#include"webrtc/video_service.h"
#endif
#endif

// ===========================================================================
// UAV_detection 主程序
//
// 当前阶段：同时测试可见光和热成像出图。
//   世辉相机 ─MIPI─▶ MIPI RX ─▶ VI ─▶ VPSS ─┬─ chn0 2566x1520（主）
//                                            └─ chn1 1920x1080（子）
//   艾睿热成像 ─MIPI─▶ MIPI RX ─▶ VI ─▶ VPSS ── chn0 640x512
// config/gateway.json 配置接入凭据后启用 HTTP/WS 状态与控制接口（见 docs/gateway.md）。
// --encode 同时发布主通道、子通道与热成像；Gateway 启用时 /webrtc 可观看三路视频。
//
// 运行（板卡上）：
//   source /usr/local/Ascend/ascend-toolkit/set_env.sh
//   ./bin/uav_detection --duration 15    # 采集 15 秒；不带参数则 Ctrl+C 结束
// 抓到的第一帧会写到当前目录，直接能看：
//   ffplay -f rawvideo -pixel_format nv21 -video_size 2566x1520 main_2566x1520.yuv
//   ffplay -f rawvideo -pixel_format nv21 -video_size 1920x1080 sub_1920x1080.yuv
//   ffplay -f rawvideo -pixel_format nv21 -video_size 640x512 thermal_640x512.yuv
//
// Ctrl+C 退出：会先拆链路再退（否则 VI/VPSS 会留在半开状态）。
// ===========================================================================

namespace
{
std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop.store(true);   // 信号处理里只置标志，清理留给主循环
}

#if defined(UAV_HAS_ASCEND)
// 声明在控制器之前，确保相机和采集线程退出后才销毁公共媒体资源。
class MediaRuntime
{
public:
    bool init()
    {
        if (!check(aclInit(nullptr), "aclInit")) return false;
        aclReady_ = true;
        if (!check(aclrtSetDevice(0), "aclrtSetDevice")) return false;
        deviceReady_ = true;
        if (!check(aclrtCreateContext(&context_, 0), "aclrtCreateContext")) return false;
        if (!check(hi_mpi_sys_init(), "hi_mpi_sys_init")) return false;
        sysReady_ = true;
        // 与 8.11/VT vi_visible_start() 一致；只在两台相机启动前设置一次。
        return check(mipi_ctl::vi_set_mipi_hs_mode(LANE_DIVIDE_MODE_1), "MIPI lane divide mode");
    }
    ~MediaRuntime()
    {
        if (sysReady_) check(hi_mpi_sys_exit(), "hi_mpi_sys_exit");
        if (context_) check(aclrtDestroyContext(context_), "aclrtDestroyContext");
        if (deviceReady_) check(aclrtResetDevice(0), "aclrtResetDevice");
        if (aclReady_) check(aclFinalize(), "aclFinalize");
    }
    aclrtContext context() const { return context_; }
private:
    static bool check(int result, const char* operation)
    {
        if (result == 0) return true;
        UAV_LOG_ERROR_F("%s 失败：%#x", operation, static_cast<unsigned>(result));
        return false;
    }
    aclrtContext context_ = nullptr;
    bool aclReady_ = false, deviceReady_ = false, sysReady_ = false;
};

// 打印一帧的关键信息（frame 只在回调期间有效，别存下来）
void logFrame(const char *tag, const hi_video_frame_info &frameInfo)
{
    const hi_video_frame &f = frameInfo.v_frame;
    UAV_LOG_INFO_F("%s：%ux%u pixel_format=%d stride=%u virt=%p phys=%#llx pts=%llu\n",
                   tag,
                   f.width, f.height, static_cast<int>(f.pixel_format), f.width_stride[0],
                   f.virt_addr[0],
                   static_cast<unsigned long long>(f.phys_addr[0]),
                   static_cast<unsigned long long>(f.pts));
}

// 保存 getFrame 回调中的同一帧，不再额外取一帧；回调返回前完成文件写入。
bool saveSample(const char* path, const hi_video_frame& frame)
{
    if (!frame.virt_addr[0] || !frame.virt_addr[1] ||
        frame.width_stride[0] < frame.width || frame.width_stride[1] < frame.width)
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    for (unsigned plane = 0; plane < 2; ++plane)
    {
        const auto* data = static_cast<const char*>(frame.virt_addr[plane]);
        const auto rows = plane == 0 ? frame.height : frame.height / 2;
        for (hi_u32 row = 0; row < rows; ++row)
        {
            output.write(data + static_cast<std::size_t>(row) * frame.width_stride[plane], frame.width);
            if (!output) return false;
        }
    }
    output.close();
    return output.good();
}

struct CaptureProbe
{
    const char* name;
    const char* samplePath;
    hi_u32 width, height;
    std::atomic<std::uint64_t> frames{0}, errors{0}, lastPtsUs{0};
    std::atomic<bool> saved{false}, running{false};
    std::uint64_t lastCount = 0; // 仅主线程读取和更新
    std::thread worker;

    CaptureProbe(const char* label, const char* path, hi_u32 w, hi_u32 h)
        : name(label), samplePath(path), width(w), height(h) {}
    ~CaptureProbe() { g_stop.store(true); join(); }
    void join() { if (worker.joinable()) worker.join(); }
    bool passed() const { return frames.load() > 0 && saved.load(); }

    void start(aclrtContext context,
               std::function<bool(RGBCameraCtl::FrameHandler, int)> grab)
    {
        running.store(true);
        worker = std::thread([this, context, grab = std::move(grab)] {
            const auto result = aclrtSetCurrentContext(context);
            if (result != ACL_SUCCESS)
            {
                ++errors;
                UAV_LOG_ERROR_F("%s：设置采集线程 context 失败：%d", name, result);
                running.store(false);
                return;
            }
            bool seen = false, sampleAttempted = false;
            try
            {
                while (!g_stop.load())
                {
                    bool valid = false;
                    const bool got = grab([&](const hi_video_frame_info& info) {
                        const auto& frame = info.v_frame;
                        if (!seen) logFrame(name, info);
                        valid = frame.width == width && frame.height == height &&
                            frame.pixel_format == HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420 &&
                            frame.compress_mode == HI_COMPRESS_MODE_NONE &&
                            frame.video_format == HI_VIDEO_FORMAT_LINEAR;
                        if (!seen && !valid)
                            UAV_LOG_ERROR_F("%s：帧格式与预期 %ux%u NV21 不符", name, width, height);
                        seen = true;
                        if (!valid) return;
                        lastPtsUs.store(frame.pts);
                        if (!sampleAttempted)
                        {
                            sampleAttempted = true;
                            saved.store(saveSample(samplePath, frame));
                            if (saved.load()) UAV_LOG_INFO_F("%s：首帧已保存至 %s", name, samplePath);
                            else UAV_LOG_ERROR_F("%s：首帧保存失败：%s", name, samplePath);
                        }
                    }, 200); // 一路离线不会阻塞其它通道，退出时最多等待本次超时
                    if (got && valid) ++frames;
                    else ++errors;
                    if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            catch (const std::exception& error)
            {
                ++errors;
                UAV_LOG_ERROR_F("%s：采集线程异常：%s", name, error.what());
            }
            catch (...)
            {
                ++errors;
                UAV_LOG_ERROR_F("%s：采集线程发生未知异常", name);
            }
            running.store(false);
        });
    }
    void logStats(double elapsedSeconds)
    {
        const auto count = frames.load();
        UAV_LOG_INFO_F("%s：%.1f fps，累计 %llu 帧，失败 %llu 次，pts=%llu，样帧=%s", name,
                       (count - lastCount) / elapsedSeconds,
                       static_cast<unsigned long long>(count),
                       static_cast<unsigned long long>(errors.load()),
                       static_cast<unsigned long long>(lastPtsUs.load()), saved.load() ? "已保存" : "未保存");
        lastCount = count;
    }
};
#endif
}   // namespace

int main(int argc, char **argv)
{
    // 日志：必须在第一次打日志之前配置
    LogConfig logCfg;
    logCfg.filePath = "logs/uav_detection.log";
    logCfg.level    = LogLevel::INFO;
    LogManager::configure(logCfg);

#if defined(UAV_HAS_GATEWAY)
    if (argc > 1 && std::string(argv[1]) == "--gateway-only")
        return uav::gateway::runStandalone(argc - 1, argv + 1);
    std::string gatewaySettings;
    std::optional<uav::gateway::Config> gatewayConfig;
#endif
    int durationSeconds = 0;
#if defined(UAV_HAS_MEDIA_SERVICE)
    bool enableEncoding = false; unsigned short rtspPort = 8554;
#endif
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument(argv[i]);
        if (argument == "--help")
        {
            std::cout << "Usage: uav_detection [--duration SECONDS]\n"
                         "  Capture RGB main/sub and thermal; 0 seconds (default) runs until Ctrl+C.\n"
                         "  Save first NV21 frames: main_2566x1520.yuv, sub_1920x1080.yuv, thermal_640x512.yuv.\n"
                         "  --gateway-config FILE selects Gateway startup settings (default config/gateway.json).\n"
                         "  uav_detection --gateway-only [--gateway-config FILE] [--bind ADDRESS] [--port PORT] [--config FILE]\n"
                         "  Gateway options: --state-file FILE --web-root DIRECTORY --tls-cert FILE --tls-key FILE\n"
                         "Set admin_token in config/gateway.json to enable Gateway alongside board camera capture.\n";
#if defined(UAV_HAS_MEDIA_SERVICE)
            std::cout << "  --encode publishes RGB main/sub and thermal via RTSP and WebRTC.\n"
                         "  --rtsp-port PORT selects RTSP listen port (default 8554), implies --encode.\n"
                         "  With Gateway credentials configured, /webrtc serves three WebRTC video sources.\n";
#endif
            return 0;
        }
#if defined(UAV_HAS_GATEWAY)
        if (argument == "--gateway-config" && i + 1 < argc)
        {
            gatewaySettings = argv[++i];
            continue;
        }
#endif
#if defined(UAV_HAS_MEDIA_SERVICE)
        if (argument == "--encode") { enableEncoding = true; continue; }
        if (argument == "--rtsp-port" && i + 1 < argc)
        {
            const std::string value(argv[++i]); unsigned port = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
            if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && port > 0 && port <= 65535)
            { rtspPort = static_cast<unsigned short>(port); enableEncoding = true; continue; }
        }
#endif
        if (argument == "--duration" && i + 1 < argc)
        {
            const std::string value(argv[++i]);
            int seconds = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && seconds >= 0)
            {
                durationSeconds = seconds;
                continue;
            }
        }
        UAV_LOG_ERROR << "参数无效：" << argument << "；使用 --help 查看用法";
        return 2;
    }

    UAV_LOG_INFO << "================ UAV_detection 启动 ================";

#if defined(UAV_HAS_GATEWAY)
    try { gatewayConfig = uav::gateway::loadConfig(gatewaySettings); }
    catch (const std::exception &error)
    { UAV_LOG_ERROR << "Gateway 配置读取失败，网页/API 入口不启动：" << error.what(); }
#endif

#if !defined(UAV_HAS_ASCEND)
    (void)durationSeconds;
#if defined(UAV_HAS_GATEWAY)
    if (gatewayConfig && !gatewayConfig->adminToken.empty())
    {
        char option[] = "--gateway-config";
        char *gatewayArgs[]{argv[0], option, gatewaySettings.data()};
        return uav::gateway::runStandalone(gatewaySettings.empty() ? 1 : 3, gatewayArgs);
    }
#endif
    // 本机 x86_64 调试（UAV_ASCEND=OFF）没有海思媒体环境，采集链路编不进来
    UAV_LOG_ERROR << "本次构建未启用昇腾（UAV_ASCEND=OFF），采集链路不可用；"
                     "请用 cmake --preset ascend-aarch64 构建后到板卡上运行";
    return 1;
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- 1. 公共媒体环境与两台相机 ---------------------------------------
    MediaRuntime mediaRuntime;
    if (!mediaRuntime.init()) return 1;
    RGBCameraCtl ctl;
    ThermalCameraCtl thermal;
    RGBCameraCtl::Config cfg;   // 默认值即 8.11 __MIPI__ 那套：
                                // 2688x1520@50 MIPI dev0 / vi pipe2 chn0 / vpss grp256 chn0+chn1
    ThermalCameraCtl::Config thermalCfg;
    cfg.vpssChnCfg[0] = {true, 2566, 1520};
    cfg.vpssChnCfg[1] = {true, 1920, 1080};
    const bool rgbReady = ctl.init(cfg);
    const bool thermalReady = thermal.init(thermalCfg);
    if (!rgbReady) UAV_LOG_ERROR << "可见光链路初始化失败，继续检查热成像";
    if (!thermalReady) UAV_LOG_ERROR << "热成像链路初始化失败，继续检查可见光";
    if (!rgbReady && !thermalReady) return 1;
    UAV_LOG_INFO_F("可见光：链路=%s，串口 %s（%s）\n", rgbReady ? "就绪" : "失败",
                   cfg.cameraPort.c_str(),
                   ctl.cameraConnected() ? "已连接，参数可调" : "未连接，只能抓图");
    UAV_LOG_INFO_F("热成像：链路=%s，串口 %s（%s）\n", thermalReady ? "就绪" : "失败",
                   thermalCfg.camera.port.c_str(), thermal.cameraConnected() ? "已连接，参数可调" : "未连接，只能抓图");

#if defined(UAV_HAS_MEDIA_SERVICE)
    // Exception unwinding joins capture before freeing the pipeline.
    std::unique_ptr<media::AscendMediaDevice> video;
#if defined(UAV_HAS_GATEWAY)
    std::array<std::shared_ptr<webrtc::VideoService>, 3> webRtc;
#endif
#endif
    CaptureProbe rgbMain("可见光主通道", "main_2566x1520.yuv", cfg.vpssChnCfg[0].width, cfg.vpssChnCfg[0].height);
    CaptureProbe rgbSub("可见光子通道", "sub_1920x1080.yuv",
                        cfg.vpssChnCfg[1].width ? cfg.vpssChnCfg[1].width : cfg.width,
                        cfg.vpssChnCfg[1].height ? cfg.vpssChnCfg[1].height : cfg.height);
    CaptureProbe thermalMain("热成像", "thermal_640x512.yuv", thermalCfg.width, thermalCfg.height);
#if defined(UAV_HAS_MEDIA_SERVICE)
    if (enableEncoding)
    {
        try
        {
            media::AscendMediaDevice::Config options; options.context = mediaRuntime.context(); options.rtspPort = rtspPort;
            options.sources[0].enabled = options.sources[1].enabled = rgbReady; options.sources[2].enabled = thermalReady;
            video = std::make_unique<media::AscendMediaDevice>(options); video->start();
        }
        catch (const std::exception& e) { UAV_LOG_ERROR << "编码链路启动失败：" << e.what(); return 1; }
    }
#endif

#if defined(UAV_HAS_GATEWAY)
    // 从启动配置读取凭据；没有配置凭据时仍可运行独立诊断采集。
    std::unique_ptr<uav::gateway::Gateway> gateway;
    auto updateGatewayStatus = [&] {
        if (!gateway) return;
        const auto s = ctl.stats();
        gateway->updateDeviceStatus("rgb", {{"state", rgbReady ? (rgbMain.frames.load() > 0 ? "capturing" : "waiting_frames") : "init_failed"},
            {"camera_control_connected", ctl.cameraConnected()}, {"frames", s.frames},
            {"errors", s.errors}, {"last_pts_us", s.lastPtsUs}});
        const auto t = thermal.stats();
        gateway->updateDeviceStatus("thermal", {{"state", thermalReady ? (thermalMain.frames.load() > 0 ? "capturing" : "waiting_frames") : "init_failed"},
            {"camera_control_connected", thermal.cameraConnected()}, {"frames", t.frames},
            {"errors", t.errors}, {"last_pts_us", t.lastPtsUs}});
    };
    if (gatewayConfig && !gatewayConfig->adminToken.empty())
    {
        try
        {
            gateway = std::make_unique<uav::gateway::Gateway>(std::move(*gatewayConfig));
            uav::gateway::Device rgb;
            rgb.id = "rgb";
            rgb.type = "camera";
            rgb.name = "Visible camera";
            rgb.config = {{"serial_port", cfg.cameraPort}, {"width", cfg.width}, {"height", cfg.height}};
            gateway->registerDevice(std::move(rgb),
                {"autofocus", "zoom_to", "focus_to", "brightness", "contrast", "saturation"},
                [&ctl](const std::string &action, const boost::json::object &parameters) {
                    using uav::gateway::CommandResult;
                    if (!ctl.cameraConnected())
                        return CommandResult{false, "camera_disconnected", "camera control serial port is unavailable", {}};
                    auto value = [&](int maximum) -> std::optional<int> {
                        const auto *p = parameters.if_contains("value");
                        if (!p || !p->is_int64() || p->as_int64() < 0 || p->as_int64() > maximum) return {};
                        return static_cast<int>(p->as_int64());
                    };
                    bool ok = false;
                    if (action == "autofocus") ok = ctl.autoFocus();
                    else
                    {
                        const auto number = value(action == "zoom_to" ? 0x4000 : action == "focus_to" ? 65535 : 100);
                        if (!number) return CommandResult{false, "invalid_parameters", "value is missing or out of range", {}};
                        if (action == "zoom_to") ok = ctl.zoomTo(static_cast<uint16_t>(*number));
                        else if (action == "focus_to") ok = ctl.focusTo(static_cast<uint16_t>(*number));
                        else if (action == "brightness") ok = ctl.setBrightness(static_cast<uint8_t>(*number));
                        else if (action == "contrast") ok = ctl.setContrast(static_cast<uint8_t>(*number));
                        else if (action == "saturation") ok = ctl.setSaturation(static_cast<uint8_t>(*number));
                    }
                    return CommandResult{ok, ok ? "command_sent" : "camera_command_failed",
                        ok ? "command written to camera serial port" : "camera command could not be written",
                        {{"device_confirmed", false}}, uav::gateway::CommandCompletion::dispatched};
                });
            uav::gateway::Device thermalDevice;
            thermalDevice.id = "thermal";
            thermalDevice.type = "camera";
            thermalDevice.name = "Thermal camera";
            thermalDevice.config = {{"serial_port", thermalCfg.camera.port},
                {"width", thermalCfg.width}, {"height", thermalCfg.height}};
            gateway->registerDevice(std::move(thermalDevice));
#if defined(UAV_HAS_MEDIA_SERVICE)
            for (std::size_t i = 0; i < webRtc.size(); ++i)
            {
                const auto source = video ? video->source(i) : media::AscendMediaDevice::Config{}.sources[i];
                if (video && source.enabled)
                {
                    webrtc::VideoService::Config live;
                    live.encoder = media::MediaService::Config::forVideo(source.width, source.height, source.fps).encoders[1];
                    webRtc[i] = std::make_shared<webrtc::VideoService>(video->service(i), live); webRtc[i]->start();
                }
                gateway->registerStream({source.id, source.deviceId, "h264", {"webrtc", "rtsp"}},
                    [service = webRtc[i], source](const std::string& protocol) -> std::optional<uav::gateway::PlaybackEndpoint> {
                        if (service && protocol == "webrtc")
                        {
                            const auto fs = ((source.width + 15) / 16) * ((source.height + 15) / 16);
                            return uav::gateway::PlaybackEndpoint{"/api/v1/webrtc/" + source.id, true,
                                {{"width", source.width}, {"height", source.height}, {"fps", source.fps}, {"max_fs", fs}, {"max_mbps", fs * source.fps}, {"signaling", "complete-sdp"}}};
                        }
                        return {};
                    });
                if (webRtc[i]) gateway->registerWebRtc(source.id, [service = webRtc[i]](auto request, auto done) {
                    auto complete = [done = std::move(done)](webrtc::VideoService::Reply result) mutable {
                        done({result.status, std::move(result.sessionId), std::move(result.sdp), std::move(result.error)});
                    };
                    if (request.sessionId.empty()) service->offer(std::move(request.sdp), std::move(request.credential), std::move(request.authorized), std::move(complete));
                    else service->close(std::move(request.sessionId), std::move(request.credential), std::move(complete));
                });
            }
#else
            gateway->registerStream({"rgb.main", "rgb", "h264", {"webrtc", "rtsp"}}, {});
#endif
            if (const char *file = std::getenv("UAV_GATEWAY_CONFIG")) uav::gateway::loadRegistry(*gateway, file);
            updateGatewayStatus();
            gateway->start();
            UAV_LOG_INFO_F("Gateway 已启动，端口 %u\n", static_cast<unsigned>(gateway->port()));
#if defined(UAV_HAS_MEDIA_SERVICE)
            if (webRtc[0] || webRtc[2]) UAV_LOG_INFO << "WebRTC viewer: /webrtc (Gateway port " << gateway->port() << ')';
#endif
        }
        catch (const std::exception &error)
        {
            UAV_LOG_ERROR << "Gateway 启动失败，仅关闭网页/API 入口，采集继续：" << error.what();
            gateway.reset();
#if defined(UAV_HAS_MEDIA_SERVICE)
            for (auto& service : webRtc) if (service) { service->stop(); service.reset(); }
#endif
        }
    }
#endif

    // ---- 2. 独立采集线程，每路首次出图时保存样帧 -------------------------
#if defined(UAV_HAS_MEDIA_SERVICE)
#if defined(UAV_HAS_GATEWAY)
    if (enableEncoding && !webRtc[0] && !webRtc[2]) UAV_LOG_INFO << "WebRTC 未启动；在 config/gateway.json 配置 admin_token 后启用 /webrtc 入口";
#else
    if (enableEncoding) UAV_LOG_INFO << "WebRTC 网页入口未构建；启用 UAV_GATEWAY 后可接入实时视频";
#endif
#endif
    try
    {
        if (rgbReady)
        {
            rgbMain.start(mediaRuntime.context(), [&](auto handler, int timeout) {
                return ctl.getFrame(0, [&](const hi_video_frame_info& frame) {
                    handler(frame);
#if defined(UAV_HAS_MEDIA_SERVICE)
                    if (video) video->submitBorrowed(0, frame);
#endif
                }, timeout);
            });
            if (cfg.vpssChnCfg[1].enable)
                rgbSub.start(mediaRuntime.context(), [&](auto handler, int timeout) {
                    return ctl.getFrame(1, [&](const hi_video_frame_info& frame) {
                        handler(frame);
#if defined(UAV_HAS_MEDIA_SERVICE)
                        if (video) video->submitBorrowed(1, frame);
#endif
                    }, timeout);
                });
        }
        if (thermalReady)
            thermalMain.start(mediaRuntime.context(), [&](auto handler, int timeout) {
                return thermal.getFrame([&](const hi_video_frame_info& frame) {
                    handler(frame);
#if defined(UAV_HAS_MEDIA_SERVICE)
                    if (video) video->submitBorrowed(2, frame);
#endif
                }, timeout);
            });
    }
    catch (const std::exception& error)
    {
        UAV_LOG_ERROR << "无法启动采集线程：" << error.what();
        g_stop.store(true);
    }
    const auto started = std::chrono::steady_clock::now();
    auto lastLog = started;
    if (durationSeconds > 0) UAV_LOG_INFO_F("开始双相机出图测试，%d 秒后结束", durationSeconds);
    else UAV_LOG_INFO << "开始双相机连续出图测试（Ctrl+C 结束）";

    // ---- 3. 监控每路实际帧率，定时结束或等待 Ctrl+C -----------------------
    while (!g_stop.load())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(1))
        {
            const double elapsed = std::chrono::duration<double>(now - lastLog).count();
            rgbMain.logStats(elapsed);
            if (cfg.vpssChnCfg[1].enable) rgbSub.logStats(elapsed);
            thermalMain.logStats(elapsed);
            lastLog = now;
#if defined(UAV_HAS_GATEWAY)
            updateGatewayStatus();
#endif
        }
        if (durationSeconds > 0 && now - started >= std::chrono::seconds(durationSeconds)) break;
        if (!rgbMain.running.load() && !rgbSub.running.load() && !thermalMain.running.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // ---- 4. 先停止所有使用方，再逆序拆链路 -------------------------------
    // 析构里也会 deinit，这里显式调一次是为了日志顺序清楚
#if defined(UAV_HAS_GATEWAY)
    // 先等待所有设备命令完成，保证 handler 不会访问已经拆除的 ctl。
    if (gateway) gateway->stop();
#endif
    g_stop.store(true);
    rgbMain.join();
    rgbSub.join();
    thermalMain.join();
#if defined(UAV_HAS_MEDIA_SERVICE)
#if defined(UAV_HAS_GATEWAY)
    for (auto& service : webRtc) if (service) service->stop();
#endif
    if (video) video->stop(); // Return every VENC input lease before VPSS and the ACL context are destroyed.
#endif
    const bool rgbPassed = rgbReady && rgbMain.passed() && (!cfg.vpssChnCfg[1].enable || rgbSub.passed());
    const bool thermalPassed = thermalReady && thermalMain.passed();
    UAV_LOG_INFO_F("出图测试结果：可见光=%s（主 %llu 帧，子 %llu 帧），热成像=%s（%llu 帧）",
                   rgbPassed ? "通过" : "失败", static_cast<unsigned long long>(rgbMain.frames.load()),
                   static_cast<unsigned long long>(rgbSub.frames.load()), thermalPassed ? "通过" : "失败",
                   static_cast<unsigned long long>(thermalMain.frames.load()));
    thermal.deinit();
    ctl.deinit();

    UAV_LOG_INFO << "================ UAV_detection 退出 ================";
    return rgbPassed && thermalPassed ? 0 : 1;
#endif
}
