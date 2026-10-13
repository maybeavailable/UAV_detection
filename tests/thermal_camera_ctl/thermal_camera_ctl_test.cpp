#include "camera/thermal_camera_ctl.h"
#include "log/log.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__)); } while (0)

namespace mock
{
std::vector<std::string> events;
std::string fail;
bool clock = false, vi = false, vpss = false, bound = false, frameHeld = false;
combo_dev_attr_t mipi{};
mipi_ctl::vi_cfg viConfig{};
mipi_ctl::sample_venc_vpss_chn_attr vpssConfig{};
hi_video_frame_info frame{};
hi_s32 getResult = HI_SUCCESS, releaseResult = HI_SUCCESS;
int gets = 0, releases = 0, timeout = 0;

hi_s32 step(const char* name)
{
    events.emplace_back(name);
    if (fail == name) { fail.clear(); return HI_FAILURE; }
    return HI_SUCCESS;
}
void empty()
{
    CHECK(!clock && !vi && !vpss && !bound && !frameHeld);
}
void reset()
{
    empty();
    events.clear(); fail.clear();
    mipi = {}; viConfig = {}; vpssConfig = {}; frame = {};
    getResult = releaseResult = HI_SUCCESS;
    gets = releases = timeout = 0;
}
}

namespace mipi_ctl
{
hi_s32 vi_enable_mipi_clock(combo_dev_t dev)
{
    CHECK(dev == 2 && !mock::clock);
    if (mock::step("clock+") != HI_SUCCESS) return HI_FAILURE;
    mock::clock = true; return HI_SUCCESS;
}
hi_s32 vi_disable_mipi_clock(combo_dev_t dev)
{
    CHECK(dev == 2 && mock::clock && !mock::vi && !mock::vpss);
    mock::clock = false; return mock::step("clock-");
}
hi_s32 vi_reset_mipi(combo_dev_t dev)
{
    CHECK(dev == 2 && mock::clock);
    return mock::step("reset");
}
hi_s32 vi_set_mipi_attr(combo_dev_attr_t attr)
{
    CHECK(mock::clock);
    mock::mipi = attr; return mock::step("attr");
}
hi_s32 vi_unreset_mipi(combo_dev_t dev)
{
    CHECK(dev == 2 && mock::clock);
    return mock::step("unreset");
}
hi_s32 vi_init(const vi_cfg* config)
{
    CHECK(mock::clock && !mock::vi);
    mock::viConfig = *config;
    if (mock::step("vi+") != HI_SUCCESS) return HI_FAILURE;
    mock::vi = true; return HI_SUCCESS;
}
hi_s32 vi_deinit(const vi_cfg* config)
{
    CHECK(mock::vi && !mock::vpss && !mock::bound);
    CHECK(config->pipe == mock::viConfig.pipe && config->vi_dev == mock::viConfig.vi_dev);
    mock::vi = false; return mock::step("vi-");
}
hi_s32 vpss_init(hi_vpss_grp grp, sample_venc_vpss_chn_attr* config)
{
    CHECK(grp == 257 && mock::vi && !mock::vpss);
    mock::vpssConfig = *config;
    if (mock::step("vpss+") != HI_SUCCESS) return HI_FAILURE;
    mock::vpss = true; return HI_SUCCESS;
}
hi_void venc_vpss_deinit(hi_vpss_grp grp, sample_venc_vpss_chn_attr* config)
{
    CHECK(grp == 257 && mock::vpss && !mock::bound && !mock::frameHeld);
    CHECK(config->enable[0] == HI_TRUE && config->enable[1] == HI_FALSE);
    mock::vpss = false; mock::step("vpss-");
}
hi_s32 vi_bind_vpss(hi_vi_pipe pipe, hi_vi_chn chn, hi_vpss_grp grp, hi_vpss_chn output)
{
    CHECK(pipe == 3 && chn == 0 && grp == 257 && output == 0);
    CHECK(mock::vi && mock::vpss && !mock::bound);
    if (mock::step("bind") != HI_SUCCESS) return HI_FAILURE;
    mock::bound = true; return HI_SUCCESS;
}
hi_s32 vi_un_bind_vpss(hi_vi_pipe pipe, hi_vi_chn chn, hi_vpss_grp grp, hi_vpss_chn output)
{
    CHECK(pipe == 3 && chn == 0 && grp == 257 && output == 0 && mock::bound);
    mock::bound = false; return mock::step("unbind");
}
}

extern "C" hi_s32 hi_mpi_vpss_get_chn_frame(hi_vpss_grp grp, hi_vpss_chn chn,
                                          hi_video_frame_info* frame, hi_s32 timeout)
{
    CHECK(grp == 257 && chn == 0 && mock::bound && !mock::frameHeld);
    ++mock::gets; mock::timeout = timeout;
    if (mock::getResult != HI_SUCCESS) return mock::getResult;
    *frame = mock::frame; mock::frameHeld = true; return HI_SUCCESS;
}
extern "C" hi_s32 hi_mpi_vpss_release_chn_frame(hi_vpss_grp grp, hi_vpss_chn chn,
                                              const hi_video_frame_info* frame)
{
    CHECK(grp == 257 && chn == 0 && mock::frameHeld && frame->v_frame.pts == mock::frame.v_frame.pts);
    ++mock::releases; mock::frameHeld = false; return mock::releaseResult;
}

ThermalCameraCtl::Config offlineConfig()
{
    ThermalCameraCtl::Config config;
    // 一个确定不存在的串口，不触碰开发机实际设备。
    config.camera.port = "/dev/uav-thermal-camera-ctl-test-nonexistent";
    return config;
}

void testLifecycle()
{
    mock::reset();
    ThermalCameraCtl ctl;
    auto config = offlineConfig();
    CHECK(ctl.init(config));
    CHECK(ctl.isInited() && !ctl.cameraConnected());
    CHECK(mock::mipi.devno == 2 && mock::mipi.input_mode == INPUT_MODE_MIPI);
    CHECK(mock::mipi.img_rect.width == 640 && mock::mipi.img_rect.height == 512);
    CHECK(mock::mipi.mipi_attr.input_data_type == DATA_TYPE_YUV422_8BIT);
    CHECK(mock::mipi.mipi_attr.lane_id[0] == 4 && mock::mipi.mipi_attr.lane_id[1] == 5);
    for (int i = 2; i < MIPI_LANE_NUM; ++i) CHECK(mock::mipi.mipi_attr.lane_id[i] == -1);
    CHECK(mock::viConfig.vi_dev == 2 && mock::viConfig.pipe == 3 && mock::viConfig.chn == 0);
    CHECK(mock::viConfig.vpss_offline == HI_TRUE && mock::viConfig.chn_depth == 0);
    CHECK(mock::vpssConfig.pixel_format == HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420);
    for (unsigned i = 0; i < HI_VPSS_MAX_CHN_NUM; ++i)
        CHECK(mock::vpssConfig.enable[i] == (i == 0 ? HI_TRUE : HI_FALSE));
    CHECK(mock::vpssConfig.output_size[0].width == 640 && mock::vpssConfig.output_size[0].height == 512);
    const auto active = mock::events;
    CHECK(!ctl.init(config) && mock::events == active && ctl.isInited());
    ctl.deinit(); mock::empty();
    CHECK((mock::events == std::vector<std::string>{"clock+", "reset", "attr", "unreset", "vi+", "vpss+", "bind",
                                                   "unbind", "vpss-", "vi-", "reset", "clock-"}));
    const auto stopped = mock::events;
    ctl.deinit(); CHECK(mock::events == stopped);
    std::swap(config.laneIds[0], config.laneIds[1]);
    CHECK(ctl.init(config));
    CHECK(mock::mipi.mipi_attr.lane_id[0] == 5 && mock::mipi.mipi_attr.lane_id[1] == 4);
    ctl.deinit(); mock::empty();
}

void testRollback()
{
    for (const char* stage : {"clock+", "reset", "attr", "unreset", "vi+", "vpss+", "bind"})
    {
        mock::reset(); mock::fail = stage;
        ThermalCameraCtl ctl;
        CHECK(!ctl.init(offlineConfig()) && !ctl.isInited());
        mock::empty();
        const auto stopped = mock::events;
        ctl.deinit(); CHECK(mock::events == stopped);
        CHECK(ctl.init(offlineConfig())); // 失败后可在同一实例上重新初始化
        ctl.deinit(); mock::empty();
    }
    // 清理中一个步骤失败，不影响后续步骤执行。
    for (const char* stage : {"unbind", "vi-", "reset", "clock-"})
    {
        mock::reset();
        ThermalCameraCtl ctl;
        CHECK(ctl.init(offlineConfig()));
        mock::fail = stage; ctl.deinit(); mock::empty();
        CHECK(mock::events.back() == "clock-");
    }
}

void testValidation()
{
    const std::vector<std::function<void(ThermalCameraCtl::Config&)>> changes{
        [](auto& c) { c.width = 384; }, [](auto& c) { c.height = 640; },
        [](auto& c) { c.mipiDev = MIPI_RX_MAX_DEV_NUM; }, [](auto& c) { c.viDev = -1; },
        [](auto& c) { c.viDev = 0; }, [](auto& c) { c.viPipe = HI_VI_MAX_PHY_PIPE_NUM; },
        [](auto& c) { c.viChn = 1; }, [](auto& c) { c.vpssGrp = HI_VPSS_MAX_GRP_NUM; },
        [](auto& c) { c.pipeDepth = 0; }, [](auto& c) { c.chnDepth = 9; },
        [](auto& c) { c.laneIds[1] = c.laneIds[0]; }, [](auto& c) { c.laneIds[0] = -1; },
        [](auto& c) { c.laneIds[1] = MIPI_LANE_NUM; }, [](auto& c) { c.getFrameTimeoutMs = 0; },
        [](auto& c) { c.camera.baud = 0; }, [](auto& c) { c.camera.responseTimeoutMs = 0; }
    };
    for (const auto& change : changes)
    {
        mock::reset(); ThermalCameraCtl ctl;
        auto config = offlineConfig(); change(config);
        CHECK(!ctl.init(config) && mock::events.empty()); mock::empty();
    }
    mock::reset(); ThermalCameraCtl ctl;
    auto config = offlineConfig(); config.cameraRequired = true;
    CHECK(!ctl.init(config) && !ctl.isInited() && mock::events.empty()); mock::empty();
}

void testFrames()
{
    mock::reset(); ThermalCameraCtl ctl;
    CHECK(!ctl.getFrame([](const auto&) {}) && mock::gets == 0);
    auto config = offlineConfig(); config.getFrameTimeoutMs = 42;
    CHECK(ctl.init(config) && ctl.stats().errors == 0);
    CHECK(!ctl.getFrame({}) && mock::gets == 0);
    mock::frame.v_frame.pts = 123456;
    CHECK(ctl.getFrame([](const auto& f) { CHECK(mock::frameHeld && f.v_frame.pts == 123456); }));
    CHECK(mock::timeout == 42 && mock::releases == 1 && !mock::frameHeld);
    CHECK(ctl.stats().frames == 1 && ctl.stats().lastPtsUs == 123456);
    mock::getResult = HI_FAILURE;
    CHECK(!ctl.getFrame([](const auto&) { CHECK(false); }, 0));
    CHECK(mock::timeout == 0 && mock::releases == 1);
    mock::getResult = HI_SUCCESS;
    CHECK(!ctl.getFrame([](const auto&) { throw std::runtime_error("consumer failed"); }, 7));
    CHECK(mock::timeout == 7 && mock::releases == 2 && !mock::frameHeld);
    CHECK(!ctl.getFrame([](const auto&) { throw 123; }));
    CHECK(mock::releases == 3 && !mock::frameHeld);
    mock::releaseResult = HI_FAILURE;
    CHECK(!ctl.getFrame([](const auto&) {}));
    CHECK(mock::releases == 4 && ctl.stats().frames == 1 && ctl.stats().errors == 5);
    mock::releaseResult = HI_SUCCESS;
    CHECK(ctl.getFrame([](const auto&) {}, -2) && mock::timeout == 42);
    ctl.deinit(); mock::empty();
}

struct TemporaryFile
{
    char path[64] = "/tmp/uav-thermal-ctl-XXXXXX";
    TemporaryFile() { const int fd = mkstemp(path); CHECK(fd >= 0); close(fd); }
    ~TemporaryFile() { unlink(path); }
};

void testSaveFrame()
{
    mock::reset(); ThermalCameraCtl ctl; TemporaryFile file;
    CHECK(ctl.init(offlineConfig()));
    // 两个平面的 stride 不同，且每行填充哨兵，验证保存时没有混入 padding。
    const unsigned yStride = 672, uvStride = 704;
    std::vector<unsigned char> y(yStride * 512, 0xEE), uv(uvStride * 256, 0xDD), expected;
    for (unsigned plane = 0; plane < 2; ++plane)
    {
        auto& data = plane == 0 ? y : uv;
        const auto stride = plane == 0 ? yStride : uvStride;
        for (unsigned row = 0; row < (plane == 0 ? 512u : 256u); ++row)
            for (unsigned col = 0; col < 640; ++col)
            {
                const auto value = static_cast<unsigned char>((row + col + plane * 77) % 200);
                data[row * stride + col] = value; expected.push_back(value);
            }
    }
    auto& frame = mock::frame.v_frame;
    frame.width = 640; frame.height = 512;
    frame.width_stride[0] = yStride; frame.width_stride[1] = uvStride;
    frame.virt_addr[0] = y.data(); frame.virt_addr[1] = uv.data();
    frame.pixel_format = HI_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    frame.compress_mode = HI_COMPRESS_MODE_NONE; frame.video_format = HI_VIDEO_FORMAT_LINEAR;
    CHECK(ctl.saveFrame(file.path));
    std::ifstream input(file.path, std::ios::binary);
    const std::vector<unsigned char> actual{std::istreambuf_iterator<char>(input), {}};
    CHECK(actual == expected && actual.size() == 491520 && mock::releases == 1);
    frame.pixel_format = HI_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    CHECK(ctl.saveFrame(file.path) && mock::releases == 2);
    frame.virt_addr[1] = nullptr;
    CHECK(!ctl.saveFrame(file.path) && mock::releases == 3);
    frame.virt_addr[1] = uv.data(); frame.width_stride[1] = 639;
    CHECK(!ctl.saveFrame(file.path) && mock::releases == 4);
    frame.width_stride[1] = uvStride;
    CHECK(!ctl.saveFrame("/dev/full") && mock::releases == 5);
    const auto gets = mock::gets;
    CHECK(!ctl.saveFrame("") && mock::gets == gets);
    CHECK(!ctl.saveFrame("/dev/uav-nonexistent-directory/frame.yuv") && mock::gets == gets);
    ctl.deinit(); mock::empty();
}

struct SerialEndpoint
{
    int fd = -1;
    int guard = -1;
    std::string port;
    SerialEndpoint()
    {
        fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        CHECK(fd >= 0 && grantpt(fd) == 0 && unlockpt(fd) == 0);
        const char* name = ptsname(fd); CHECK(name); port = name;
        guard = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK); CHECK(guard >= 0);
    }
    ~SerialEndpoint() { if (fd >= 0) close(fd); if (guard >= 0) close(guard); }
    // PTY 主端/测试 guard 一直持有设备，重连前从 guard 清除测试设备的独占标记。
    void allowReopen() { CHECK(ioctl(guard, TIOCNXCL) == 0); }
};

void testSerialLifecycle()
{
    mock::reset(); SerialEndpoint serial;
    std::atomic<int> reports{0}, frames{0};
    ThermalCameraCtl ctl;
    ctl.setReportCallback([&](const auto& report) { CHECK(report.type == airui::AlarmType::Fire); ++reports; });
    ctl.setProtocolFrameCallback([&](const auto&) { ++frames; });
    auto config = offlineConfig(); config.camera.port = serial.port; config.cameraRequired = true;
    CHECK(ctl.init(config) && ctl.cameraConnected());
    CHECK(ctl.camera()->config().baud == 115200 && ctl.camera()->config().port == serial.port);
    termios settings{}; CHECK(tcgetattr(serial.guard, &settings) == 0);
    CHECK(cfgetispeed(&settings) == B115200 && cfgetospeed(&settings) == B115200);
    CHECK((settings.c_cflag & CSIZE) == CS8 && !(settings.c_cflag & (PARENB | CSTOPB)));
    // 建采集链路时不发送其它机型的格式/复位/保存命令。
    pollfd descriptor{serial.fd, POLLIN, 0};
    CHECK(poll(&descriptor, 1, 30) == 0);
    const auto sendReports = [&](int count) {
        std::vector<std::uint8_t> alarm{0xAA, 25, 1, 8, 1, 3};
        alarm.resize(26, 0);
        std::uint8_t sum = 0; for (const auto byte : alarm) sum += byte;
        alarm.insert(alarm.end(), {sum, 0xEB, 0xAA});
        const std::uint8_t reply[]{0x55, 4, 0x42, 0x33, 4, 0xD2, 0xEB, 0xAA};
        alarm.insert(alarm.end(), std::begin(reply), std::end(reply));
        CHECK(write(serial.fd, alarm.data(), alarm.size()) == static_cast<ssize_t>(alarm.size()));
        for (int i = 0; i < 100 && (reports.load() < count || frames.load() < count * 2); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(reports.load() == count && frames.load() == count * 2);
    };
    sendReports(1); // init 前注册的回调在 ThermalCamera 重建内核后仍有效
    const auto events = mock::events;
    serial.allowReopen();
    CHECK(ctl.reconnectCamera() && ctl.cameraConnected() && mock::events == events);
    sendReports(2);
    ctl.deinit(); CHECK(!ctl.cameraConnected()); mock::empty();
    serial.allowReopen();
    CHECK(ctl.init(config)); sendReports(3);
    ctl.deinit(); mock::empty();
    serial.allowReopen(); mock::fail = "bind";
    CHECK(!ctl.init(config) && !ctl.cameraConnected()); mock::empty();
    config.camera.port = "/dev/uav-thermal-camera-ctl-test-nonexistent";
    CHECK(!ctl.init(config) && !ctl.cameraConnected()); mock::empty();
}

int main()
{
    LogConfig logging; logging.filePath = "/tmp/uav-thermal-camera-ctl-test.log";
    logging.level = LogLevel::FATAL; LogManager::configure(logging);
    try
    {
        testLifecycle(); testRollback(); testValidation(); testFrames(); testSaveFrame(); testSerialLifecycle();
        std::cout << "thermal camera controller: all tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
