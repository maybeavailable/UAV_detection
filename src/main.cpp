#include"log/log.h"

#include<atomic>
#include<chrono>
#include<csignal>
#include<cstdlib>
#include<iostream>
#include<optional>
#include<string>

#if defined(UAV_HAS_GATEWAY)
#include"gateway/gateway.h"
#endif

#if defined(UAV_HAS_ASCEND)
#include"camera/rgb_camera_ctl.h"
#endif

// ===========================================================================
// UAV_detection 主程序
//
// 当前阶段：把可见光相机的采集链路跑起来，并确认**能抓到图**。
//   世辉相机 ─MIPI─▶ MIPI RX ─▶ VI ─▶ VPSS ─┬─ chn0 2688x1520（主）
//                                            └─ chn1 1920x1080（子）
// 设置 UAV_GATEWAY_TOKEN 时启用 HTTP/WS 状态与控制接口（见 docs/gateway.md）。
// 编码（VENC）、RTSP/GB28181 推流都还没接。
//
// 运行（板卡上）：
//   source /usr/local/Ascend/ascend-toolkit/set_env.sh
//   ./bin/uav_detection
// 抓到的第一帧会写到当前目录，直接能看：
//   ffplay -f rawvideo -pixel_format nv21 -video_size 2688x1520 main_2688x1520.yuv
//   ffplay -f rawvideo -pixel_format nv21 -video_size 1920x1080 sub_1920x1080.yuv
//
// Ctrl+C 退出：会先拆链路再退（否则 VI/VPSS 会留在半开状态）。
// ===========================================================================

namespace
{
// 抓图循环：0 = 一直抓（Ctrl+C 退出）；改成 N 就抓 N 帧后自动退出
constexpr int kGrabCount = 0;

std::atomic<bool> g_stop{false};

void onSignal(int)
{
    g_stop.store(true);   // 信号处理里只置标志，清理留给主循环
}

#if defined(UAV_HAS_ASCEND)
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
#endif
    if (argc > 1)
    {
        if (std::string(argv[1]) == "--help")
        {
            std::cout << "Usage: uav_detection [--gateway-only [--bind ADDRESS] [--port PORT] [--config FILE]]\n"
                         "  Gateway options: --state-file FILE --web-root DIRECTORY --tls-cert FILE --tls-key FILE\n"
                         "Set UAV_GATEWAY_TOKEN to enable Gateway alongside board camera capture.\n";
            return 0;
        }
        UAV_LOG_ERROR << "未知参数：" << argv[1];
        return 2;
    }

    UAV_LOG_INFO << "================ UAV_detection 启动 ================";

#if !defined(UAV_HAS_ASCEND)
#if defined(UAV_HAS_GATEWAY)
    if (const char *token = std::getenv("UAV_GATEWAY_TOKEN"); token && *token)
        return uav::gateway::runStandalone(argc, argv);
#endif
    // 本机 x86_64 调试（UAV_ASCEND=OFF）没有海思媒体环境，采集链路编不进来
    UAV_LOG_ERROR << "本次构建未启用昇腾（UAV_ASCEND=OFF），采集链路不可用；"
                     "请用 cmake --preset ascend-aarch64 构建后到板卡上运行";
    return 1;
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ---- 1. 建链路 -------------------------------------------------------
    RGBCameraCtl ctl;
    RGBCameraCtl::Config cfg;   // 默认值即 8.11 __MIPI__ 那套：
                                // 2688x1520@50 MIPI dev0 / vi pipe2 chn0 / vpss grp256 chn0+chn1
    if (!ctl.init(cfg))
    {
        UAV_LOG_ERROR << "相机链路初始化失败，退出";
        return -1;
    }
    UAV_LOG_INFO_F("链路已就绪：相机串口 %s（%s）\n",
                   cfg.cameraPort.c_str(),
                   ctl.cameraConnected() ? "已连接，参数可调" : "未连接，只能抓图");

    // ---- 2. 两路各抓一帧，确认真的有图 ------------------------------------
    if (ctl.getFrame(0, [](const hi_video_frame_info &f) { logFrame("主通道第一帧", f); }))
    {
        ctl.saveFrame(0, "main_2688x1520.yuv");
    }
    else
    {
        UAV_LOG_ERROR << "主通道没抓到帧（检查顺序：相机是否在出图 → MIPI 接线与 lane 数 → "
                         "vi pipe2 / vpss grp256 是否与硬件对得上）";
    }

    if (cfg.vpssChnCfg[1].enable &&
        ctl.getFrame(1, [](const hi_video_frame_info &f) { logFrame("子通道第一帧", f); }))
    {
        ctl.saveFrame(1, "sub_1920x1080.yuv");
    }

#if defined(UAV_HAS_GATEWAY)
    // 设置 Token 时启用网页接入。独立诊断采集仍可在未设置 Token 时运行。
    std::unique_ptr<uav::gateway::Gateway> gateway;
    auto updateGatewayStatus = [&] {
        if (!gateway) return;
        const auto s = ctl.stats();
        gateway->updateDeviceStatus("rgb", {{"state", "capturing"},
            {"camera_control_connected", ctl.cameraConnected()}, {"frames", s.frames},
            {"errors", s.errors}, {"last_pts_us", s.lastPtsUs}});
    };
    if (const char *token = std::getenv("UAV_GATEWAY_TOKEN"); token && *token)
    {
        try
        {
            gateway = std::make_unique<uav::gateway::Gateway>(uav::gateway::configFromEnvironment());
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
            gateway->registerStream({"rgb.main", "rgb", "h264", {"webrtc", "rtsp", "gb28181"}}, {});
            if (const char *file = std::getenv("UAV_GATEWAY_CONFIG")) uav::gateway::loadRegistry(*gateway, file);
            updateGatewayStatus();
            gateway->start();
            UAV_LOG_INFO_F("Gateway 已启动，端口 %u\n", static_cast<unsigned>(gateway->port()));
        }
        catch (const std::exception &error)
        {
            UAV_LOG_ERROR << "Gateway 启动失败，仅关闭网页/API 入口，采集继续：" << error.what();
            gateway.reset();
        }
    }
#endif

    // ---- 3. 连续抓图，每秒打一次统计（Ctrl+C 退出）-------------------------
    if (kGrabCount > 0)
    {
        UAV_LOG_INFO_F("开始抓图：抓满 %d 帧后退出\n", kGrabCount);
    }
    else
    {
        UAV_LOG_INFO << "开始连续抓图（Ctrl+C 退出）";
    }

    uint64_t grabbed   = 0;
    uint64_t lastCount = 0;
    auto     lastLog   = std::chrono::steady_clock::now();

    while (!g_stop.load())
    {
        if (ctl.getFrame(0, [](const hi_video_frame_info &) {}))
        {
            ++grabbed;
        }

        if (kGrabCount > 0 && grabbed >= static_cast<uint64_t>(kGrabCount))
        {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(1))
        {
            const RGBCameraCtl::Stats s = ctl.stats();
            UAV_LOG_INFO_F("抓图统计：本秒 %llu 帧，累计 %llu 帧，失败 %llu 次，最后一帧 pts=%llu\n",
                           static_cast<unsigned long long>(grabbed - lastCount),
                           static_cast<unsigned long long>(s.frames),
                           static_cast<unsigned long long>(s.errors),
                           static_cast<unsigned long long>(s.lastPtsUs));
            lastLog   = now;
            lastCount = grabbed;
#if defined(UAV_HAS_GATEWAY)
            updateGatewayStatus();
#endif
        }
    }

    // ---- 4. 拆链路 -------------------------------------------------------
    // 析构里也会 deinit，这里显式调一次是为了日志顺序清楚
#if defined(UAV_HAS_GATEWAY)
    // 先等待所有设备命令完成，保证 handler 不会访问已经拆除的 ctl。
    if (gateway) gateway->stop();
#endif
    ctl.deinit();

    UAV_LOG_INFO_F("退出：共抓到 %llu 帧\n", static_cast<unsigned long long>(grabbed));
    UAV_LOG_INFO << "================ UAV_detection 退出 ================";
    return 0;
#endif
}
