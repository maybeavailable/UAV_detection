# 艾睿 F384/F640 串口控制协议

依据《F384F640 非制冷红外机芯组件用户操作指令说明书 V1.0.2（20251107）》实现，下面的页码均为正文页码，PDF 阅读器页码需加 3。

`src/camera/airui_protocol.h` 定义协议枚举、数据类型、编解码接口和 `AiruiCore`；`airui_protocol.cpp` 实现 81 个命令字的收发与查询。主工程 CMake 已包含此源文件。协议层可独立使用，不依赖 Gateway，也不负责视频采集、编码或网络推流。

上层业务控制可使用 [ThermalCamera](thermal-camera.md)，提供与 `RGBCamera` 一致的成功返回值、运动方向和速度约定，以及画幅检查、环境参数组合设置。

## 接入示例

```cpp
#include "camera/airui_protocol.h"

bool configureThermalCamera()
{
    // 默认 /dev/ttyCH9344USB0、115200 baud、8N1，成像型机芯。
    AiruiCore camera;
    if (!camera.connect()) return false;

    std::uint16_t width = 0, height = 0;
    if (!camera.getSensorWidth(width) || !camera.getSensorHeight(height))
        return false;

    if (!camera.setPalette(airui::Palette::WhiteHot) ||
        !camera.setBrightness(50) || !camera.setContrast(50) ||
        !camera.setDigitalZoom(2.0, width, height))
        return false;

    // 写入成功仅代表本次命令完成；持久保存使用单独的指令。
    return camera.saveSettings();
}
```

上例展示一次控制操作。持续接收报警时应将 `AiruiCore` 保存为控制器成员，让对象及串口保持存活。默认串口为 `/dev/ttyCH9344USB0`，波特率为 `115200`；如需更换接线，可通过构造参数覆盖串口节点和波特率。构造对象不会打开串口，调用 `connect()` 时才应用配置并启动接收。

测温型机芯需显式声明能力：

```cpp
AiruiCore camera("/dev/ttyCH9344USB0", 115200, airui::DeviceType::Radiometric);
if (!camera.connect()) return false;
if (!camera.setTemperatureUnit(airui::TemperatureUnit::Celsius) ||
    !camera.setEmissivity(0.95) || !camera.setTargetDistance(100.0) ||
    !camera.setHumidity(0.4) || !camera.applyEnvironment())
    return false;

double temperature = 0;
if (!camera.getPointTemperature({320, 256}, temperature)) return false;
```

文档第 29 页明确限制成像型机芯使用测温指令，因此默认 `Imaging` 会拒绝测温、温度报警、等温线及测温镜头修正命令，返回 `UnsupportedDevice`，不会写串口。火点报警、火点阈值和强光保护允许两类机芯使用。声明 `Radiometric` 前需要确认实际机芯型号，此选项不会自动检测硬件能力。

## 支持范围

| 类别 | 已实现功能 |
| --- | --- |
| 基础设置 | SN、焦平面宽高、背景/快门校正、自动快门及三种间隔、机芯/FPA 温度、保存与恢复出厂 |
| 图像与视频 | ROI/倍率数字变倍、翻转、模拟/数字视频、视频源、冻结、开机标志、20 种伪彩、警示色阈值、场景模式、亮度/对比度/细节/空域/时域滤波 |
| 镜头 | 镜头类型、手动报警、变焦后自动聚焦、温差触发聚焦、自动聚焦、聚焦/变焦粗调与微调、停止、位置/转速/行程、10 组预置位、焦距/焦距显示、水平/垂直视场角 |
| 测温 | 量程、显示、单位、环境参数及生效、指定点/中心温度、火点报警、温宽拉伸、温度报警、等温线、低/高阈值 |
| 校准与维护 | 测温镜头修正数据查询/擦除/标定/保存/生效、盲元光标/移动/编辑/保存、镜头 K 值标定、图像再补偿、同步模式与频率 |
| 主动上报 | 火点报警的 5 个区域中心坐标、强光保护关闭快门状态 |

`airui::commandCatalog()` 提供全部命令的操作类型、参数长度、查询应答长度、机芯能力限制与正文页码。类型化接口检查文档规定的枚举与取值范围；实际镜头的机械行程、焦距范围和传感器坐标范围由上层根据设备参数约束。

## 报文规则

```text
命令：    AA LEN GROUP CMD OP PARAM... SUM EB AA
普通应答：55 LEN       CMD 33 DATA...  SUM EB AA
扩展应答：55 LEN GROUP CMD 33 DATA...  SUM EB AA
```

- 命令及扩展应答 `LEN = 4 + 参数长度`，普通应答 `LEN = 3 + 参数长度`；整帧长度都是 `LEN + 4`。
- 校验 `SUM` 是从帧头到校验位之前所有字节之和，取低 8 位，包含帧头与长度。
- 16/32 位整数及 float32 都使用小端。图像模式使用 `02` 命令组，其应答仍采用省略组号的普通格式。
- 写入/执行应答 `01` 为成功，`00` 为失败；读取应答按每个命令的实际数据长度解析，不把数据 `00` 当作失败。
- 命令字 `FF` 的设备协议错误保存在 `Result::deviceError`：`F1` 超时、`FB` 缺命令字、`FD` 校验错误、`FF` 帧头错误。
- 变焦位置 `08/33` 的应答为 `55 06 08 33 33 ...`，先识别扩展格式，避免误当成普通 `08` 强光保护应答。

`encodeCommand()` 检查命令、操作及参数长度；错误抛出 `std::invalid_argument`。`decodeFrame()` 检查整帧长度、帧头、帧尾、校验及操作格式，失败时保留输出对象原值。`FrameParser` 支持分包、粘包和坏帧丢弃，按长度切帧，允许数据中出现 `EB AA`；内部最多保存一个最长报文（259 字节）。若噪音碰巧构成合法帧头和长度，必须等够该长度才能验证，命令超时后的重连会清空未完成报文。

低层原始事务接口用于核对固件差异：

```cpp
auto result = camera.transact(airui::Command::SensorWidth, airui::Operation::Read);
if (!result) {
    // airui::errorString(result.error) 可用于日志；result.frame 保留已收到的原始报文。
}
```

`transact()` 同样检查机芯能力、操作、参数长度和应答长度，但不解释原始参数的物理含义；优先使用类型化接口设置参数。`lastResult()` 是对象最近一次记录的结果，多线程调用时需要准确关联具体命令的错误信息，应使用 `transact()` 的返回值。

## 物理量与精度

类型化接口接收实际物理量，编码时四舍五入到协议精度，查询时自动除以对应比例：

| 字段 | 协议表示 | API 单位 |
| --- | --- | --- |
| 自动快门时间间隔 | uint8，分钟 | 分钟 |
| 自动快门 FPA/机芯温差间隔 | uint8，实际值 ×10 | ℃ |
| 机芯/FPA 温度 | 16 位，实际值 ×100 | ℃ |
| 自动聚焦温差间隔 | uint16，实际值 ×100 | ℃ |
| 焦距 | uint16，实际值 ×10 | mm |
| 视场角 | IEEE-754 float32 | 度 |
| 反射/大气温度、拉伸低/高阈值 | 32 位，实际值 ×10000 | 当前温度单位 |
| 湿度、发射率 | uint32，实际值 ×10000 | 比例 0～1；湿度 0.4 表示 40% |
| 目标距离 | uint32，实际值 ×10000 | m |
| 点温、中心温度、等温线/温度报警阈值 | 32 位，实际值 ×10 | 当前温度单位 |
| 镜头测温标定目标值 | 16 位整数，直接写入 | 整数温度，例：10℃ 写入 `0A 00` |
| 火点阈值 | uint16，1～16383 | 原始阈值，无温度比例换算 |

文档没有明确说明温度整数的有符号编码；实现将可为负的温度按二进制补码解释和写入，机芯/FPA 为 int16，其余上述温度为 int32，标定目标为 int16。正温示例已按 PDF 验证，负温编码仍需真机确认；可通过原始 `transact()` 查看未换算数据。

数字变倍 `setDigitalZoom(ratio, width, height)` 支持 1.0～8.0、步进 0.1，按第 10 页中心裁剪公式计算，右下角包含边界。左/上坐标四舍五入，右/下坐标向下取整；文档个别倍率示例存在 1 像素取整差异。需要指定精确裁剪窗口时使用 `setDigitalZoom(Rectangle)`。尺寸可由 `getSensorWidth/Height()` 获取。

## 生命周期、回调与超时

`AiruiCore` 复用项目 `Urt`，打开串口即启动后台接收线程，所有命令通过互斥锁依次完成，可从多个工作线程发起。`connect()`、`disconnect()` 由同一个控制线程管理；`disconnect()` 会唤醒正在等待的命令。析构前应停止其他线程对对象的访问。

`setAlarmCallback()` 接收火点和强光上报；`setFrameCallback()` 接收未被当前事务消费的报文，包括主动报警和迟到应答。两者均在接收线程执行，参数引用只在回调期间有效，需留存时复制。回调应快速将事件投递给业务线程，不可同步发命令、断开串口或析构对象，避免阻塞接收线程或等待自身退出。

主动报警使用 `AA` 帧头、`01/08/01` 命令体和 21 字节参数。火点类型字节是 `03`，后面是 5 组 uint16 `(x,y)`；文档以 `(0,0)` 表示未使用区域，没有有效区域数量字段。强光类型字节是 `01`，紧随的状态字节 `01` 表示快门关闭。报警不会被当作命令应答。

应答默认等待 500ms，可用 `setResponseTimeout()` 调整，原始 `transact()` 也可指定单次超时。耗时标定应根据真机响应时间设置更长超时。串口写入另受 `Urt` 自身超时约束。

协议没有事务序号，应答也不回显读/写操作。超时或写入失败后，本对象暂停后续命令，返回 `NeedsReconnect`，以避免把迟到的同命令应答归到下一次请求；不会自动重发电机运动、出厂恢复和标定命令。确认设备执行状态后，在控制线程调用 `reconnect()` 恢复。重开 UART 会清理本地缓冲，但不能取消机芯已开始执行的操作；如设备还可能继续回包，应先等待设备恢复空闲。发生链路错误后也需重新连接。

## 文档勘误与采用依据

| 正文页 | 不一致之处 | 实现采用 |
| --- | --- | --- |
| 17～18 | 总格式主要列出 `01/07/08`，场景模式使用 `02` | 按场景模式收发示例，命令组 `02`、普通应答、查询返回 4 字节，首字节是模式 |
| 25、27 | 电机转速“读取”表格操作字写作 `01` | 按读取示例使用 `00`，保留一个 `00` 参数 |
| 27 | 变焦电机行程返回长度写作 6 | 按示例及字段说明使用 4：最小/最大位置各 uint16 |
| 34 | 中心温度返回长度写作 6 | 按报文 `LEN=0C` 和说明使用 8：温度 4、x 2、y 2 |
| 35 | 火点阈值查询返回长度写作 1 | 按示例使用 uint16、2 字节 |
| 35 | 阈值 10000 的写入示例校验为 `E8` | 按规则计算为 `20`：`AA 06 07 31 01 10 27 20 EB AA` |
| 36 | 拉伸低阈值表格命令字为 `2D` | 按所有收发示例使用 `1D`；高阈值为 `1E` |
| 36 | 低阈值成功/失败应答长度为 `08`，读取命令长度为 `05` | 分别修正为 `05`、`04`，与字段数量及原示例校验一致 |
| 38 | 等温线表格命令字为 `2D`，读取操作为 `01` | 按收发示例使用 `20`、读取 `00` |
| 40 | 是否保存镜头修正数据查询表格有一个 `00` 参数 | 按完整示例使用零参数：`AA 04 07 6A 00 1F EB AA` |
| 42 | 强光主动上传的表格标为读取、1 字节参数 | 按实际 29 字节报文使用操作 `01`、21 字节参数 |

以上是针对本版文档的实现判断，报文解析仍严格检查长度和校验，不接受错误示例。真机若存在固件差异，应根据抓取报文及厂商确认结果修改对应命令定义。

## 验证

主机独立测试仅依赖 Linux 伪终端、C++17、线程库与仓库内 Boost 头文件：

```sh
cmake -S tests/airui -B build/airui-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/airui-tests -j4
ctest --test-dir build/airui-tests --output-on-failure
```

`tests/airui/pdf_vectors.txt` 保存 418 条有效文档示例报文，包含普通/扩展应答、控制报文及两类完整主动报警。测试核对编码、逐字节解包、坏校验、内嵌帧尾、报警与应答混合、全部 81 个命令字的已定义操作、物理量缩放、float32、错误应答、能力限制、并发串行化、超时后禁止继续发命令、主动断开和对端拔出。

测试也检查 UART 的 115200/8N1 配置和重连。本次修正了共用 `Urt` 将 `INPCK` 错写入 `c_cflag` 的问题，改为 `c_iflag`，避免改变 `CS8` 的数据位配置。

内存与未定义行为检查：

```sh
cmake -S tests/airui -B build/airui-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined -no-pie'
cmake --build build/airui-sanitize -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/airui-sanitize --output-on-failure
```

伪终端验证协议和软件收发行为，不能验证实际波特率、电气接线、镜头机械范围、标定效果或固件兼容性；这些仍需接机芯联调。
