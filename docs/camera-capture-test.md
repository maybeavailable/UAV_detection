# 双相机出图测试

`src/main.cpp` 同时启动可见光与热成像采集，用三个独立线程检查可见光主通道、可见光子通道和热成像 chn0。每路首次收到尺寸和格式正确的帧时打印帧信息、保存原始 NV21，此后每秒记录实际帧率、累计帧数、错误数和最后一帧 PTS。一路超时不会阻塞其它通道。

## 板端运行

```sh
# 开发机交叉编译
cmake --preset ascend-aarch64
cmake --build --preset ascend-aarch64 -j4

# 将程序及所需动态库放到板卡，再加载板端 CANN 环境
source /usr/local/Ascend/ascend-toolkit/set_env.sh
./uav_detection --duration 15
```

`--duration 15` 表示相机初始化完成后采集 15 秒；不传参数或传 `--duration 0` 持续采集，Ctrl+C/SIGTERM 结束。参数必须是非负整数。

程序负责 `aclInit`、device0、公共 ACL context、`hi_mpi_sys_init()` 和共享 MIPI lane 分组初始化。按参考工程 `8.11/VT` 使用 `LANE_DIVIDE_MODE_1`，只在启动相机前设置一次。各采集线程绑定同一个 context；退出时先停止 Gateway 和采集线程，再关闭相机，最后退出媒体系统、销毁 context 和释放 ACL。

| 输出 | 链路 | 尺寸 | 当前目录下的样帧 | 字节数 |
| --- | --- | --- | --- | --- |
| 可见光主通道 | MIPI dev0 / VI pipe2 / VPSS grp256 chn0 | 2688×1520 | `main_2688x1520.yuv` | 6128640 |
| 可见光子通道 | 同上，VPSS chn1 | 1920×1080 | `sub_1920x1080.yuv` | 3110400 |
| 热成像 | MIPI dev2，lane4/5 / VI pipe3 / VPSS grp257 chn0 | 640×512 | `thermal_640x512.yuv` | 491520 |

可见光默认串口 `/dev/ttyCH9344USB6`、9600；热成像默认 `/dev/ttyCH9344USB0`、115200、8N1。两者允许控制串口离线时继续检查视频。可见光保持控制器原有输出格式设置；热成像不额外发送格式、复位或保存参数命令。

样帧在 `getFrame()` 的回调内同步保存，逐行去除 stride 填充，然后才归还硬件缓冲区。每路只尝试保存一张有效帧；保存失败会记录错误，采集继续，最终测试判为失败。文件会覆盖同名旧样帧，判断本次结果应以日志中的保存结果和退出码为准。

```sh
ffplay -f rawvideo -pixel_format nv21 -video_size 2688x1520 main_2688x1520.yuv
ffplay -f rawvideo -pixel_format nv21 -video_size 1920x1080 sub_1920x1080.yuv
ffplay -f rawvideo -pixel_format nv21 -video_size 640x512 thermal_640x512.yuv
```

日志文件为 `logs/uav_detection.log`。结束时分别输出可见光和热成像的通过/失败结论：所有启用通道都收到至少一张尺寸正确、非压缩、线性 NV21 帧，并成功保存样帧时，退出码为 `0`；任一路初始化、出图或保存失败时为 `1`；命令行参数错误为 `2`。结束前的偶发取帧超时单独计数，不会抹去已出图的结果。

一路初始化失败时，程序继续验证另一台相机。未指定时长时需要手动结束；两台都无法初始化时直接返回失败。初始化成功仅说明链路已建立，是否出图由实际帧和样帧验证。

设置 `UAV_GATEWAY_TOKEN` 时仍可启用 Gateway。设备列表包含 RGB 和热成像的采集状态；热成像在这个测试入口只上报状态，可见光保留原有控制动作。`--gateway-only` 继续作为独立 Gateway 的入口。

## 主机回归测试

```sh
cmake -S tests/thermal_camera_ctl -B build/thermal-camera-ctl-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/thermal-camera-ctl-tests -j4
ctest --test-dir build/thermal-camera-ctl-tests --output-on-failure
```

`camera_capture_main` 执行真实 `main.cpp`，用控制器/ACL 替身输出合成帧。检查三个文件的首帧内容与大小、定时结束、一路无帧时其它通道继续采集、错误尺寸、保存失败、线程 context 失败、公共初始化失败回收、CLI 和 Ctrl+C 退出顺序。此测试验证程序编排，真实出图和图像内容需要在板卡上检查。
