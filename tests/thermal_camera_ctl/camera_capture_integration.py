#!/usr/bin/env python3
"""真实 main 的双相机编排测试；仅使用合成帧，不访问相机设备。"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
samples = {
    "main_2566x1520.yuv": (2566 * 1520 * 3 // 2, 11),
    "sub_1920x1080.yuv": (1920 * 1080 * 3 // 2, 21),
    "thermal_640x512.yuv": (640 * 512 * 3 // 2, 31),
}


def check_samples(root, expected):
    for name, (size, value) in samples.items():
        path = root / name
        if name in expected:
            data = path.read_bytes()
            assert len(data) == size, (name, len(data), size)
            assert data == bytes([value]) * size, name  # 首帧内容完整且去掉 stride padding
        else:
            assert not path.is_file(), name


def run_case(fault="", expected=None, code=0, arguments=None, block_sample=False):
    with tempfile.TemporaryDirectory(prefix="uav-capture-") as work:
        root = Path(work)
        trace = root / "trace.txt"
        environment = dict(os.environ, UAV_CAPTURE_TEST_TRACE=str(trace), UAV_CAPTURE_TEST_FAULT=fault)
        if block_sample:
            (root / "thermal_640x512.yuv").mkdir()
        result = subprocess.run([binary, *(arguments or ["--duration", "1"])], cwd=root,
                                env=environment, capture_output=True, text=True, timeout=6)
        assert result.returncode == code, (fault, result.returncode, result.stderr)
        events = trace.read_text().splitlines() if trace.exists() else []
        check_samples(root, set(samples) if expected is None else expected)
        if "sys_exit" in events:
            assert events.count("acl_init") == events.count("sys_init") == events.count("lane_mode") == 1
            assert events[-4:] == ["sys_exit", "context_destroy", "device_reset", "acl_finalize"]
            for camera in ("rgb", "thermal"):
                cleanup = camera + "_deinit"
                if cleanup in events:
                    assert events.index(cleanup) < events.index("sys_exit")
            if "rgb_deinit" in events and "thermal_deinit" in events:
                assert events.index("thermal_deinit") < events.index("rgb_deinit")
        return events


run_case()
rgb_samples = {"main_2566x1520.yuv", "sub_1920x1080.yuv"}
events = run_case("thermal", expected=rgb_samples, code=1)
assert events.count("get.rgb0") > 20 and events.count("get.rgb1") > 20  # 热成像超时不拖慢 RGB
run_case("rgb_init", expected={"thermal_640x512.yuv"}, code=1)
run_case("thermal_init", expected=rgb_samples, code=1)
run_case("bad_thermal_frame", expected=rgb_samples, code=1)
run_case(expected=rgb_samples, code=1, block_sample=True)
run_case("thread_context", expected=set(), code=1)
for fault, cleanup in (
    ("acl_init", []),
    ("device_set", ["acl_finalize"]),
    ("context_create", ["device_reset", "acl_finalize"]),
    ("sys_init", ["context_destroy", "device_reset", "acl_finalize"]),
    ("lane_mode", ["sys_exit", "context_destroy", "device_reset", "acl_finalize"]),
):
    events = run_case(fault, expected=set(), code=1)
    assert not any(event in events for event in ("rgb_init", "thermal_init"))
    if cleanup:
        assert events[-len(cleanup):] == cleanup
for arguments in (["--duration"], ["--duration", "-1"], ["--duration", "1x"],
                  ["--duration", "2147483648"], ["--unknown"]):
    assert run_case(expected=set(), code=2, arguments=arguments) == []
assert run_case(expected=set(), arguments=["--help"]) == []

with tempfile.TemporaryDirectory(prefix="uav-capture-signal-") as work:
    root = Path(work)
    trace = root / "trace.txt"
    environment = dict(os.environ, UAV_CAPTURE_TEST_TRACE=str(trace), UAV_CAPTURE_TEST_FAULT="")
    process = subprocess.Popen([binary], cwd=root, env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if all((root / name).exists() and (root / name).stat().st_size == size
                   for name, (size, _) in samples.items()):
                break
            assert process.poll() is None
            time.sleep(0.02)
        else:
            raise AssertionError("未及时保存三个通道的首帧")
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=3) == 0
        check_samples(root, set(samples))
        assert trace.read_text().splitlines()[-4:] == ["sys_exit", "context_destroy", "device_reset", "acl_finalize"]
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()

print("PASS: main dual-camera capture, first frames, independent timeouts, CLI, signals and runtime cleanup")
