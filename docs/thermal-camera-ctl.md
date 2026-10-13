# ThermalCameraCtl 热成像采集控制器

`src/camera/thermal_camera_ctl.h/.cpp` 编排艾睿 F640 的串口与视频采集，复用 `ThermalCamera` 和 `mipi_ctl`，在 `UAV_ASCEND=ON` 时参与主工程编译。

`main.cpp` 已提供同时检查可见光和热成像的入口，板端运行 `./uav_detection --duration 15`，详见 [双相机出图测试](camera-capture-test.md)。

```text
艾睿 F640 ── MIPI YUV422 8bit ── MIPI RX dev2 ── VI dev2/pipe3/chn0
    ▲                                                      │
    │                                                      ▼
    └── ThermalCamera / AiruiCore              VPSS grp257/chn0
        /dev/ttyCH9344USB0，115200，8N1               640×512 NV21
                                                           │
                                                 getFrame / saveFrame
```

## 默认配置与参考来源

| 项目 | 配置 |
| --- | --- |
| 输入/输出分辨率 | 固定 640×512，配置为其它尺寸会拒绝初始化 |
| MIPI RX | dev2，YUV422 8bit，X1，WDR NONE |
| MIPI lane | 4、5，其余为 -1；可调整这两条 lane 的编号和顺序 |
| VI | dev2 / pipe3 / chn0，ISP bypass，pipe depth=3，chn depth=0 |
| VI→VPSS | offline，输出非压缩 NV21 |
| VPSS | grp257，仅启用 chn0，640×512；chn1 禁用 |
| 串口 | /dev/ttyCH9344USB0，115200，8N1，500ms 应答超时 |
| 抓帧超时 | 默认 1000ms；调用时 0 为非阻塞，负数使用配置值 |

接线取自 `/home/huangquan/code/8.11/VT/src/mpp/mpp_system.cpp` 的 `vi_thermal_start()`、`__THERMAL_AIRUI__` 分支；pipe、group 和单通道配置取自同项目 `src/input_node/input_node.cpp` 的 `generate_camera_mpp_cfg(..., false)`。其中多普分支的 lane 顺序为 5、4，本实现使用艾睿分支的 4、5。

## 启动与使用

应用应先完成全局 ACL/context 和 `hi_mpi_sys_init()`，在采集线程设置正确的 ACL context。MIPI 的全局 lane 分组也由应用在启动各相机前设置一次。本控制器不修改分组、不退出全局媒体运行时，便于与可见光相机同时使用。

参考工程在 `vi_visible_start()` 中设置 `LANE_DIVIDE_MODE_1`，随后启动热成像；其热成像 `__MIPI__` 分支不重复设置。共享分组必须按板卡驱动和实际接线确认，不能在其它相机运行期间切换。默认控制器初始化假定这些公共资源已经就绪。

```cpp
#include "camera/thermal_camera_ctl.h"

// 应用已经初始化 ACL/媒体运行时、设置好采集线程 context 和全局 lane 分组。
bool captureThermalFrame()
{
    ThermalCameraCtl thermal;
    ThermalCameraCtl::Config config;
    config.cameraRequired = true; // 串口也必须正常；默认 false 允许仅采集
    if (!thermal.init(config)) return false;

    if (!thermal.getFrame([](const hi_video_frame_info& frame) {
        // 640×512 NV21：virt_addr/phys_addr、width_stride 和 pts 均来自 VPSS。
        // 在这里同步处理，或拷贝后交给其它线程；离开回调立即归还帧。
    })) return false;

    if (!thermal.saveFrame("thermal_640x512.yuv")) return false;
    if (!thermal.setPseudoColor(4)) return false;
    // 完整串口接口：thermal.camera()->getEnvironment(...)
    return true; // 析构依次解绑、关闭 VPSS/VI、复位 MIPI/关闭时钟、断开串口
}
```

查看保存的帧：

```sh
ffplay -f rawvideo -pixel_format nv21 -video_size 640x512 thermal_640x512.yuv
```

`getFrame()` 只有 chn0 这一条路径，不提供通道选择。`saveFrame()` 检查线性、非压缩的 NV21/NV12 及两个可访问的虚拟地址，逐行去除填充，640×512 文件为 491520 字节。若 SDK 未提供可访问的虚拟平面，保存返回 false；`getFrame()` 仍可把原始帧交给使用物理地址的处理模块。

控制器不会自动修改机芯的视频输出、复位机芯或保存参数。艾睿协议没有此 MIPI 输出的分辨率/帧率配置指令，连接时只建立串口通信，实际输入须已经是 640×512。

## 生命周期与错误处理

- `init()` 验证配置、连接串口，然后依次创建 MIPI、VI、VPSS 并绑定。已初始化时返回 false，修改配置前须 `deinit()`。
- 初始化任一步失败会逆序回收已创建的资源，同时断开串口；同一对象可重新初始化。`deinit()` 可重复调用。清理步骤失败会记录日志，并继续清理其它资源。
- `cameraRequired=false` 时串口连接失败只记录告警，采集仍可启动；非法串口配置始终导致初始化失败。
- `reconnectCamera()` 只恢复串口，不重建采集链路。报警和原始协议帧回调可在 `init()` 前注册，初始化和重连后保留。
- `setReportCallback()` 接收报警；`setProtocolFrameCallback()` 接收未被事务消费的协议帧。它们运行在串口接收线程，只能复制和投递事件，不能同步调用相机控制或生命周期接口。
- 抓帧回调在调用线程执行。回调异常会被捕获并返回 false，取到的帧仍会归还；还帧错误也返回 false。`stats()` 返回本次初始化以来成功处理并归还的帧数、抓帧错误数和最后成功帧的 PTS，保存文件的错误不计入抓帧错误数。
- 生命周期操作由同一控制线程管理，操作期间须停止抓帧及串口命令。推荐仅一个采集线程调用 `getFrame()`/`saveFrame()`；析构前必须停止其它线程访问对象。

## 构建与验证

```sh
cmake --preset ascend-aarch64
cmake --build --preset ascend-aarch64 -j4

cmake -S tests/thermal_camera_ctl -B build/thermal-camera-ctl-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/thermal-camera-ctl-tests -j4
ctest --test-dir build/thermal-camera-ctl-tests --output-on-failure
```

控制器测试在主机使用真实 CANN 头文件，替换媒体函数并通过 Linux 伪终端模拟串口，无需板卡媒体库。非默认安装路径可指定 `-DTHERMAL_CANN_INCLUDE_DIR=/path/to/CANN/include`。

测试覆盖单通道与接线参数、各初始化阶段故障回收、重复关闭及失败后重试、非法配置、抓帧超时、回调异常、还帧失败、不同 stride 的 YUV 输出和写入错误，以及串口必需/可选、115200/8N1、重连、回调保留和视频初始化失败时关闭串口。真实板卡的 MIPI 时序、公共 lane 分组和实际出图仍需硬件联调。
