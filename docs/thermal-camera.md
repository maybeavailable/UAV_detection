# ThermalCamera 热成像相机控制

`src/camera/thermal_camera.h/.cpp` 是 `AiruiCore` 上的业务控制层，与 `RGBCamera` 并列。它封装艾睿 F384/F640 的串口控制，提供统一的方向、速度、图像参数、测温和报警接口；视频采集、编码和推流由其它模块负责。

默认配置为 `/dev/ttyCH9344USB0`、115200 baud、8N1、成像型机芯、500ms 应答超时。构造对象和 `init()` 不打开串口，`connect()` 才开始接收。

640×512 单通道视频采集和串口统一管理使用 [ThermalCameraCtl](thermal-camera-ctl.md)。

## 使用

```cpp
#include "camera/thermal_camera.h"

bool configureThermalCamera(ThermalCamera& camera)
{
    if (!camera.connect()) return false;
    if (camera.setPseudoColor(4) != 1 ||       // 铁红
        camera.setBrightness(50) != 1 ||
        camera.setContrast(50) != 1 ||
        camera.setDigitalZoom(2.0) != 1)       // 自动查询实际画幅后计算 ROI
        return false;
    return true;
}

// 控制器中持有对象，持续接收报警；析构时自动关闭串口。
// ThermalCamera camera;
// if (!configureThermalCamera(camera)) { /* 查看 camera.lastResult() */ }
```

生命周期操作返回 `bool`，控制和查询接口返回 `int32_t`，`1` 成功、`0` 失败。`lastResult()` 提供本层最近一次操作的错误码、设备错误及已收到的应答帧。协议细节、物理量比例及文档勘误见 [艾睿协议说明](airui-protocol.md)。

接口覆盖基础参数、背景/快门校正、图像与视频输出、镜头/预置位/焦距、测温、火点报警、强光保护、盲元与镜头标定。标定和保存均由调用方明确触发，连接时不自动下发这些命令。

## 方向、速度和图像参数

| 接口 | 业务参数 | 实际行为 |
| --- | --- | --- |
| `continuousZoom(direction, speed)` | 0 停止、1 放大、2 缩小 | 转换为艾睿停止、Tele=2、Wide=1 |
| `continuousFocus(direction, speed)` | 0 停止、1 近焦、2 远焦 | 转换为艾睿停止、Near=1、Far=2 |
| 以上运动的 `speed` | 0 保持当前速度，1～100 为业务速度 | 非零时按 `round(speed*32/100)` 换算，最小为 1；先写速度、成功后启动运动 |
| `setImageFlip(mode)` | 0 不翻转、1 水平、2 垂直、3 同时翻转 | 对应协议 `01/02/04/08` |
| `setPseudoColor(color)` | 0～19 | 与协议伪彩编号一致，也可用类型化 `setPalette()` |
| `setNoiseReduction(mode, level)` | 0 空域、1 时域，level 0～100 | 对应空域/时域滤波 |
| `setDigitalZoom(ratio)` | 1.0～8.0、步进 0.1 | 查询焦平面宽高后计算中心裁剪，不固定使用 640×512 |
| `setDigitalZoom(Rectangle)` | 原始传感器坐标，右下角含边界 | 查询画幅并检查 ROI 边界 |

可使用 `zoomIn/zoomOut/zoomStop`、`focusNear/focusFar/focusStop` 和 `autoFocus()`。连续运动不会自动停止，停止由调用方发送；停止操作忽略速度和微调参数。非法方向、非停止运动的非法速度或微调枚举会直接失败。

`driveZoom/driveFocus` 使用 `airui` 类型化协议方向，`setZoomSpeed/setFocusSpeed` 接收原始档位 0～32；它们供需要直接控制协议档位的调用使用，区别于上述业务侧的 0～100 速度。

数字变倍每次查询实际宽高，适配不同画幅。ROI 和测温点使用机芯协议坐标，本层按焦平面宽高检查边界。文档没有明确说明翻转、数字裁剪后显示坐标与测温坐标的关系，需通过真机确认；网页显示坐标的转换应由上层根据确认结果处理。

## 测温与环境参数

默认成像型机芯拒绝测温专用命令，火点报警和强光保护仍可使用。实际硬件支持测温时，通过配置声明：

```cpp
ThermalCamera::Config config;
config.type = airui::DeviceType::Radiometric;
ThermalCamera camera(config);
if (!camera.connect()) return false;
if (camera.setTemperatureUnit(airui::TemperatureUnit::Celsius) != 1) return false;

ThermalCamera::Environment env;
env.reflectedTemperature = 25;
env.atmosphericTemperature = 25;
env.emissivity = 0.95;
env.targetDistanceMeters = 100;
env.humidity = 0.4; // 比例，代表 40%
if (camera.setEnvironment(env) != 1) return false;

double temperature = 0;
if (camera.getPointTemperature({320, 256}, temperature) != 1) return false;
```

`setEnvironment()` 在写串口前检查全部输入，再依次设置反射温度、大气温度、发射率、距离、湿度，最后发送环境参数生效指令。温度采用设备当前单位；本方法不会自动切换单位。单独设置环境字段后仍需调用 `applyEnvironment()`。

`getEnvironment()` 和 `getSensorSize()` 仅在全部字段读取成功后更新输出。点温查询会检查机芯能力、查询画幅、校验坐标，然后读取温度，失败时保留原输出。返回的中心温度和火点中心同样保留机芯协议坐标。

多步操作由本层互斥锁整体保护，不与本层其它命令交错。若某一步失败，停止后续指令并记录该步错误；已经成功执行的设备设置不会回滚。对于运动命令，速度设置失败时不发送启动命令。

## 生命周期和报警

- `init(Config)` 更换串口、波特率、机芯类型和超时，断开旧串口并保留回调，随后需显式 `connect()`；非法配置字段不会断开旧连接。
- `init(port, baud)` 保留原机芯类型及超时。`config()` 返回配置副本，`setResponseTimeout()` 更新应答超时。
- 所有生命周期操作由同一个控制线程管理。`disconnect()` 先取消底层等待，再等待业务操作退出；析构前应停止其它线程访问对象。
- `setReportCallback()` 接收 `airui::AlarmReport`；`setFrameCallback()` 接收未被事务消费的原始帧。回调及超时配置在 `init()`、重连后保留。
- 回调运行在接收线程，只应复制事件并投递业务线程，不可同步控制相机、重连、析构对象或调用需要等待业务锁的查询接口。回调参数引用仅在本次调用有效。
- 超时返回 `Timeout`，后续命令返回 `NeedsReconnect`。确认机芯执行状态后再调用 `reconnect()`，不自动重发运动或标定操作。
- `core()` 是维护入口，直接调用会绕过本层多步串行化及错误记录；应在没有本层并发操作时使用，并读取内核自己的 `lastResult()`。`init()` 后旧内核指针失效。

重连只能恢复本地串口接收状态，不能取消机芯已经执行的命令；具体行为及限制见协议说明。

## 验证

主工程 CMake 已包含 `thermal_camera.cpp`。业务层测试与协议测试共用 Linux 伪终端：

```sh
cmake -S tests/airui -B build/airui-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/airui-tests -j4
ctest --test-dir build/airui-tests --output-on-failure
```

在原有 418 条协议示例测试基础上，新增方向与速度换算、翻转、画幅变化、ROI/点温边界、组合操作失败、环境参数预检查、多线程命令分组、报警回调、更换串口、超时恢复、断开取消等待及机芯能力限制测试。伪终端不能验证实际电气连接、镜头运动和温度精度，仍需真机联调。
