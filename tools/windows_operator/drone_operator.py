#!/usr/bin/env python3
"""One-command Windows operator for the three isolated drone programs."""

from __future__ import annotations

import argparse
import json
import select
import shutil
import socket
import socketserver
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path


SERVER = "root@10.8.82.81"
SSH_OPTIONS = [
    "-o",
    "BatchMode=yes",
    "-o",
    "ConnectTimeout=8",
    "-o",
    "ServerAliveInterval=5",
    "-o",
    "ServerAliveCountMax=3",
]
LOCAL_PROXY_PORT = 18089
REMOTE_PROXY_PORT = 18088
LOCAL_API_KEY_FILE = Path.home() / "Desktop" / "识图API.txt"
MANUAL_API_KEY_FILE = (
    "/opt/iking/match_agent_manual/.secrets/dashscope_api_key"
)

STATUS_COMMAND = "/opt/iking/match_agent/codex/drone-status"
PARAMETER_READER = "/opt/iking/portable_performance_test/scripts/read-parameter.py"
MODE_SETTER = "/opt/iking/portable_performance_test/scripts/set-flight-mode.py"

CONTROLLERS = {
    "match": "/opt/iking/match_agent_flow_test/build/match_flow",
    "classmate": "/opt/iking/match_agent/match",
    "classmate-manual": "/opt/iking/match_agent_manual/build/match_manual",
    "test": "/opt/iking/portable_performance_test/build/portable_performance_test",
}

REQUIRED_REMOTE_FILES = [
    STATUS_COMMAND,
    PARAMETER_READER,
    MODE_SETTER,
    "/opt/iking/match_agent_flow_test/scripts/run.sh",
    "/opt/iking/match_agent_flow_test/scripts/trigger-round.sh",
    "/opt/iking/match_agent_flow_test/build/match_flow",
    "/opt/iking/match_agent/match",
    "/opt/iking/portable_performance_test/scripts/run.sh",
    "/opt/iking/portable_performance_test/build/portable_performance_test",
]

BUILD_TARGETS = {
    "match": {
        "binary": "/opt/iking/match_agent_flow_test/build/match_flow",
        "build_script": "/opt/iking/match_agent_flow_test/scripts/build.sh",
        "inputs": [
            "/opt/iking/match_agent_flow_test/CMakeLists.txt",
            "/opt/iking/match_agent_flow_test/src/match_flow.cpp",
            "/opt/iking/match_agent_flow_test/src/flow_logic.hpp",
            "/opt/iking/match_agent_flow_test/src/mission_sequence.hpp",
            "/opt/iking/match_agent_flow_test/src/portable_site.hpp",
            "/opt/iking/match_agent_flow_test/src/qwen_vision.hpp",
            "/opt/iking/match_agent_flow_test/src/recognize_image.hpp",
            "/opt/iking/match_agent_flow_test/tests/flow_logic_test.cpp",
        ],
    },
    "test": {
        "binary": "/opt/iking/portable_performance_test/build/portable_performance_test",
        "build_script": "/opt/iking/portable_performance_test/scripts/build.sh",
        "inputs": [
            "/opt/iking/portable_performance_test/CMakeLists.txt",
            "/opt/iking/portable_performance_test/src/portable_performance_test.cpp",
            "/opt/iking/portable_performance_test/src/performance_plan.hpp",
            "/opt/iking/portable_performance_test/tests/performance_plan_test.cpp",
        ],
    },
    "classmate-manual": {
        "binary": "/opt/iking/match_agent_manual/build/match_manual",
        "build_script": "/opt/iking/match_agent_manual/scripts/build.sh",
        "inputs": [
            "/opt/iking/match_agent_manual/CMakeLists.txt",
            "/opt/iking/match_agent_manual/src/match_manual.cpp",
            "/opt/iking/match_agent_manual/src/camera_preflight.cpp",
            "/opt/iking/match_agent_manual/src/recognize_image.hpp",
            "/opt/iking/match_agent_manual/scripts/run.sh",
            "/opt/iking/match_agent_manual/scripts/trigger-round.sh",
            "/opt/iking/match_agent_manual/tests/manual_trigger_contract_test.py",
        ],
    },
}


def ssh_arguments(remote_command: str, tty: bool = False) -> list[str]:
    arguments = ["ssh"]
    if tty:
        arguments.append("-tt")
    arguments.extend(SSH_OPTIONS)
    arguments.extend([SERVER, remote_command])
    return arguments


def run_remote(
    remote_command: str,
    *,
    tty: bool = False,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ssh_arguments(remote_command, tty),
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=capture,
    )


def require_ssh() -> None:
    if shutil.which("ssh") is None:
        raise RuntimeError("Windows 未找到 ssh，请先安装 OpenSSH 客户端")


def load_local_api_key() -> str:
    try:
        api_key = LOCAL_API_KEY_FILE.read_text(encoding="utf-8-sig").strip()
    except OSError as error:
        raise RuntimeError(
            f"无法读取识图密钥文件：{LOCAL_API_KEY_FILE}"
        ) from error
    if not api_key or "\n" in api_key or "\r" in api_key:
        raise RuntimeError("识图密钥文件必须只包含一行非空密钥")
    return api_key


def install_manual_api_key() -> None:
    api_key = load_local_api_key()
    command = (
        "set -eu; umask 077; "
        "d=/opt/iking/match_agent_manual/.secrets; mkdir -p \"$d\"; "
        "t=\"$d/dashscope_api_key.tmp.$$\"; "
        "trap 'rm -f \"$t\"' EXIT; "
        "IFS= read -r key; [ -n \"$key\" ]; "
        "printf '%s\\n' \"$key\" > \"$t\"; chmod 600 \"$t\"; "
        f"mv \"$t\" {MANUAL_API_KEY_FILE}; trap - EXIT"
    )
    result = subprocess.run(
        ssh_arguments(command),
        input=api_key + "\n",
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"无法安全安装识图密钥：{detail}")


def read_remote_json(command: str, description: str) -> dict:
    result = run_remote(command, capture=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"无法读取{description}: {detail}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{description}不是有效 JSON") from error


def list_controllers() -> list[tuple[int, str]]:
    script = (
        "for d in /proc/[0-9]*; do "
        "p=${d##*/}; e=$(readlink -f \"$d/exe\" 2>/dev/null || true); "
        "case \"$e\" in "
        "/opt/iking/match_agent_flow_test/build/match_flow|"
        "/opt/iking/match_agent/match|"
        "/opt/iking/match_agent_manual/build/match_manual|"
        "/opt/iking/portable_performance_test/build/portable_performance_test) "
        "printf '%s\\t%s\\n' \"$p\" \"$e\";; esac; done"
    )
    result = run_remote(script, capture=True)
    if result.returncode != 0:
        raise RuntimeError("无法检查正在运行的控制程序")
    controllers: list[tuple[int, str]] = []
    for line in result.stdout.splitlines():
        pid, separator, executable = line.partition("\t")
        if separator and pid.isdigit():
            controllers.append((int(pid), executable))
    return controllers


def ensure_no_controllers(purpose: str = "启动") -> None:
    controllers = list_controllers()
    if controllers:
        names = ", ".join(f"PID {pid} {path}" for pid, path in controllers)
        raise RuntimeError(f"已有控制程序运行，不能{purpose}：{names}")


def relative_ground_altitude(position: dict) -> float:
    value = position.get("relative_dock_altitude")
    if value is None:
        value = position.get("altitude", 999)
    try:
        return float(value)
    except (TypeError, ValueError):
        return 999.0


def is_ground_ready(status: dict) -> bool:
    position = status.get("position") or {}
    return (
        str(status.get("flight_path", "")).startswith("STANDBY")
        and status.get("armed") is False
        and status.get("sdk_mode") is True
        and abs(relative_ground_altitude(position)) <= 0.10
    )


def is_mode_switch_safe(status: dict) -> bool:
    speed = status.get("speed") or {}
    try:
        total_speed = abs(float(speed.get("total", 999)))
    except (TypeError, ValueError):
        return False
    return (
        str(status.get("flight_path", "")).startswith("STANDBY")
        and status.get("armed") is False
        and total_speed <= 0.10
    )


def flight_mode_name(value: int) -> str:
    if value == 1:
        return "仿真"
    if value == 0:
        return "实飞"
    return f"未知({value})"


def build_status(name: str) -> tuple[bool, str]:
    target = BUILD_TARGETS[name]
    binary = target["binary"]
    inputs = " ".join(f"'{path}'" for path in target["inputs"])
    command = (
        f"b='{binary}'; "
        "[ -x \"$b\" ] || { echo 'binary missing'; exit 3; }; "
        f"for s in {inputs}; do "
        "[ -e \"$s\" ] || { echo \"input missing: $s\"; exit 4; }; "
        "[ \"$s\" -nt \"$b\" ] && { echo \"newer input: $s\"; exit 5; }; "
        "done; echo current"
    )
    result = run_remote(command, capture=True)
    detail = result.stdout.strip() or result.stderr.strip()
    return result.returncode == 0, detail


def ensure_build_current(name: str) -> None:
    current, detail = build_status(name)
    if current:
        return
    target = BUILD_TARGETS[name]
    print(f"检测到程序需要更新（{detail}），正在自动编译并运行测试……")
    result = run_remote(target["build_script"], capture=True)
    if result.returncode != 0:
        output = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"服务器自动编译失败：{output}")
    current, detail = build_status(name)
    if not current:
        raise RuntimeError(f"编译后版本复查失败：{detail}")
    print("编译与测试通过。")


def show_status() -> int:
    status = read_remote_json(STATUS_COMMAND, "无人机状态")
    cfg = read_remote_json(f"{PARAMETER_READER} CFG_FLIGHTSIM", "飞行模式")
    cfg_value = int(cfg.get("integer", -1))
    position = status.get("position") or {}
    navigation = status.get("navigation") or {}
    print("SSH 连接：正常")
    print(f"飞行模式：{flight_mode_name(cfg_value)}")
    print(f"地面预检：{'通过' if is_ground_ready(status) else '未通过'}")
    print(
        "定位状态："
        f"RTK={navigation.get('rtk_status', '未知')}，"
        f"卫星={navigation.get('satellite_count', '未知')}"
    )
    print(f"当前绝对高度：{position.get('altitude', '未知')} 米")
    print(
        "相对起降点高度："
        f"{position.get('relative_dock_altitude', '未知')} 米"
    )
    print("\n完整状态：")
    print(json.dumps(status, ensure_ascii=False, indent=2))
    controllers = list_controllers()
    if controllers:
        print("\n正在运行的控制程序：")
        for pid, executable in controllers:
            print(f"  PID {pid}: {executable}")
    else:
        print("\n当前没有比赛或性能测试控制程序。")
    return 0


def check_setup() -> int:
    required = " ".join(f"'{path}'" for path in REQUIRED_REMOTE_FILES)
    files = run_remote(
        f"for f in {required}; do [ -x \"$f\" ] || echo \"$f\"; done",
        capture=True,
    )
    if files.returncode != 0:
        raise RuntimeError("无法检查服务器程序文件")

    missing = [line for line in files.stdout.splitlines() if line.strip()]
    cfg = read_remote_json(f"{PARAMETER_READER} CFG_FLIGHTSIM", "飞行模式")
    status = read_remote_json(STATUS_COMMAND, "无人机状态")
    controllers = list_controllers()
    match_build_current, match_build_detail = build_status("match")
    test_build_current, test_build_detail = build_status("test")
    cfg_value = int(cfg.get("integer", -1))
    navigation = status.get("navigation") or {}
    navigation_ready = (
        int(navigation.get("rtk_status", 0)) >= 4
        and int(navigation.get("satellite_count", 0)) >= 10
    )

    checks = [
        ("SSH 连接", True),
        ("服务器程序文件", not missing),
        ("飞机落地、未解锁、SDK 控制", is_ground_ready(status)),
        ("没有其他控制程序", not controllers),
        ("实飞定位（仿真可忽略）", navigation_ready),
    ]
    print(f"当前模式：{flight_mode_name(cfg_value)}")
    for label, passed in checks:
        print(f"[{'通过' if passed else '未通过'}] {label}")
    print(
        f"[{'通过' if match_build_current else '启动时自动更新'}] "
        "Codex 比赛程序版本"
    )
    print(
        f"[{'通过' if test_build_current else '启动时自动更新'}] "
        "性能测试程序版本"
    )
    if missing:
        print("缺少或不可执行的服务器文件：")
        for path in missing:
            print(f"  {path}")
    if controllers:
        print("正在运行的控制程序：")
        for pid, executable in controllers:
            print(f"  PID {pid}: {executable}")
    if not match_build_current:
        print(f"Codex 比赛程序：{match_build_detail}")
    if not test_build_current:
        print(f"性能测试程序：{test_build_detail}")

    ready = (
        not missing
        and is_ground_ready(status)
        and not controllers
    )
    if cfg_value == 0:
        ready = ready and navigation_ready
    if cfg_value not in (0, 1):
        ready = False
    print(
        "\n结论："
        + (
            "可以继续选择对应模式的启动命令；待更新版本会先自动编译测试。"
            if ready
            else "暂时不要启动，请先处理未通过项。"
        )
    )
    return 0 if ready else 1


def preflight(expected_environment: str) -> None:
    ensure_no_controllers("重复启动")

    cfg = read_remote_json(f"{PARAMETER_READER} CFG_FLIGHTSIM", "飞行模式")
    expected_value = 1 if expected_environment == "sim" else 0
    if int(cfg.get("integer", -1)) != expected_value:
        current = "仿真" if int(cfg.get("integer", -1)) == 1 else "实飞"
        wanted = "仿真" if expected_environment == "sim" else "实飞"
        raise RuntimeError(f"当前是{current}模式，但你复制的是{wanted}命令")

    status = read_remote_json(STATUS_COMMAND, "无人机状态")
    if not is_ground_ready(status):
        raise RuntimeError(
            "启动前检查失败：必须落地待机、未解锁、相对起降点高度为 0 且处于 SDK 模式"
        )

    if expected_environment == "real":
        navigation = status.get("navigation") or {}
        if (
            int(navigation.get("rtk_status", 0)) < 4
            or int(navigation.get("satellite_count", 0)) < 10
        ):
            raise RuntimeError("实飞定位检查失败：RTK 状态或卫星数量不足")


def mode_switch_preflight() -> None:
    ensure_no_controllers("切换模式")

    status = read_remote_json(STATUS_COMMAND, "无人机状态")
    if not is_mode_switch_safe(status):
        raise RuntimeError(
            "切换模式前必须处于 STANDBY、未解锁、速度接近 0 且没有控制程序"
        )


def set_environment(environment: str) -> int:
    mode_switch_preflight()
    result = run_remote(f"{MODE_SETTER} {environment}", capture=True)
    if result.stdout.strip():
        print(result.stdout.strip())
    if result.returncode != 0:
        detail = result.stderr.strip() or "服务器拒绝切换模式"
        raise RuntimeError(detail)

    cfg = read_remote_json(f"{PARAMETER_READER} CFG_FLIGHTSIM", "飞行模式")
    expected = 1 if environment == "sim" else 0
    if int(cfg.get("integer", -1)) != expected:
        raise RuntimeError("切换后的独立复查失败")
    name = "仿真" if environment == "sim" else "实飞"
    print(f"已确认切换为{name}模式；本命令没有启动飞行程序。")
    return 0


class ConnectProxyHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        self.request.settimeout(10)
        request = bytearray()
        while b"\r\n\r\n" not in request and len(request) < 65536:
            chunk = self.request.recv(4096)
            if not chunk:
                return
            request.extend(chunk)

        first_line = bytes(request).split(b"\r\n", 1)[0]
        parts = first_line.decode("ascii", "replace").split()
        if len(parts) != 3 or parts[0].upper() != "CONNECT":
            self.request.sendall(b"HTTP/1.1 405 Method Not Allowed\r\n\r\n")
            return

        host, separator, port_text = parts[1].rpartition(":")
        if not separator or not host:
            self.request.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return

        try:
            upstream = socket.create_connection((host, int(port_text)), timeout=10)
        except (OSError, ValueError):
            self.request.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
            return

        with upstream:
            self.request.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            sockets = [self.request, upstream]
            while True:
                readable, _, exceptional = select.select(sockets, [], sockets, 30)
                if exceptional or not readable:
                    return
                for source in readable:
                    try:
                        data = source.recv(65536)
                    except OSError:
                        return
                    if not data:
                        return
                    target = upstream if source is self.request else self.request
                    target.sendall(data)


class ThreadingConnectProxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


@dataclass
class ProxyTunnel:
    server: ThreadingConnectProxy | None = None
    thread: threading.Thread | None = None
    tunnel: subprocess.Popen[str] | None = None

    def start(self, probe_url: str) -> None:
        self.server = ThreadingConnectProxy(
            ("127.0.0.1", LOCAL_PROXY_PORT), ConnectProxyHandler
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        self.tunnel = subprocess.Popen(
            [
                "ssh",
                *SSH_OPTIONS,
                "-N",
                "-o",
                "ExitOnForwardFailure=yes",
                "-R",
                f"{REMOTE_PROXY_PORT}:127.0.0.1:{LOCAL_PROXY_PORT}",
                SERVER,
            ],
            creationflags=flags,
        )
        time.sleep(1.5)
        if self.tunnel.poll() is not None:
            raise RuntimeError("识图隧道启动失败，远端 18088 端口可能被占用")

        probe = run_remote(
            "curl -sS -o /dev/null -m 10 "
            f"-x http://127.0.0.1:{REMOTE_PROXY_PORT} "
            f"{probe_url}"
        )
        if probe.returncode != 0:
            raise RuntimeError("同学程序的识图网络检查失败")

    def close(self) -> None:
        if self.tunnel is not None and self.tunnel.poll() is None:
            self.tunnel.terminate()
            try:
                self.tunnel.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.tunnel.kill()
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
        if self.thread is not None:
            self.thread.join(timeout=2)


def run_foreground(command: str) -> int:
    try:
        return run_remote(command, tty=True).returncode
    except KeyboardInterrupt:
        print("\n已请求停止，正在等待程序处理返航或退出。")
        return 130


def run_performance(environment: str, altitude: float) -> int:
    if not 1.0 <= altitude <= 20.0:
        raise RuntimeError("测试高度必须在 1 到 20 米之间")
    preflight(environment)
    ensure_build_current("test")
    altitude_text = f"{altitude:g}"
    battery = " --confirm-battery-ready" if environment == "real" else ""
    command = (
        "cd /opt/iking/portable_performance_test && "
        f"exec ./scripts/run.sh --execute --environment {environment} "
        f"--confirm-site-clear{battery} --altitude {altitude_text} "
        "--leg 3 --speed 1 --hover 2 --site-radius 20 "
        "--site-altitude-limit 20 --camera both"
    )
    return run_foreground(command)


def run_match(dry_run: bool, use_simulation_oracle: bool = False) -> int:
    if dry_run:
        ensure_no_controllers("启动只读监听")
        ensure_build_current("match")
        return run_foreground(
            "cd /opt/iking/match_agent_flow_test && "
            "exec ./scripts/run.sh --dry-run"
        )

    preflight("sim")
    ensure_build_current("match")
    proxy = ProxyTunnel()
    try:
        proxy.start("https://openrouter.ai/")
        print("Qwen 识图网络已准备，正在启动 Codex 比赛程序。")
        oracle_environment = (
            "IKING_ALLOW_SIM_ORACLE=1 " if use_simulation_oracle else ""
        )
        command = (
            "cd /opt/iking/match_agent_flow_test && "
            f"HTTPS_PROXY=http://127.0.0.1:{REMOTE_PROXY_PORT} "
            f"HTTP_PROXY=http://127.0.0.1:{REMOTE_PROXY_PORT} "
            f"{oracle_environment}"
            "exec ./scripts/run.sh --execute"
        )
        return run_foreground(command)
    finally:
        proxy.close()


def run_classmate(environment: str, dry_run: bool) -> int:
    if dry_run:
        ensure_no_controllers("启动只读监听")
        return run_foreground(
            "cd /opt/iking/match_agent && exec ./match --dry-run"
        )

    preflight(environment)
    proxy = ProxyTunnel()
    try:
        proxy.start(
            "https://ws-sxeumotzb6ouodsm.cn-beijing.maas.aliyuncs.com/"
        )
        print("识图代理已准备，正在调用同学原来的 ./match --execute。")
        command = (
            "cd /opt/iking/match_agent && "
            f"HTTPS_PROXY=http://127.0.0.1:{REMOTE_PROXY_PORT} "
            f"HTTP_PROXY=http://127.0.0.1:{REMOTE_PROXY_PORT} "
            "exec ./match --execute"
        )
        return run_foreground(command)
    finally:
        proxy.close()


def run_classmate_manual(environment: str, dry_run: bool) -> int:
    if dry_run:
        ensure_no_controllers("启动只读监听")
        ensure_build_current("classmate-manual")
        return run_foreground(
            "cd /opt/iking/match_agent_manual && "
            "exec ./scripts/run.sh --dry-run"
        )

    preflight(environment)
    ensure_build_current("classmate-manual")
    install_manual_api_key()
    proxy = ProxyTunnel()
    try:
        proxy.start(
            "https://ws-sxeumotzb6ouodsm.cn-beijing.maas.aliyuncs.com/"
        )
        print("识图代理已准备，正在启动同学程序的电脑手动触发版。")
        command = (
            "cd /opt/iking/match_agent_manual && "
            f"HTTPS_PROXY=http://127.0.0.1:{REMOTE_PROXY_PORT} "
            f"HTTP_PROXY=http://127.0.0.1:{REMOTE_PROXY_PORT} "
            "exec ./scripts/run.sh --execute --confirm-preflight"
        )
        return run_foreground(command)
    finally:
        proxy.close()


def stop_controller(name: str) -> int:
    executable = CONTROLLERS[name]
    command = (
        "found=0; for d in /proc/[0-9]*; do "
        "p=${d##*/}; e=$(readlink -f \"$d/exe\" 2>/dev/null || true); "
        f"if [ \"$e\" = \"{executable}\" ]; then "
        "kill -TERM \"$p\"; printf '已发送停止信号给 PID %s\\n' \"$p\"; found=1; fi; "
        "done; if [ \"$found\" = 0 ]; then echo '该程序当前没有运行'; fi"
    )
    return run_remote(command).returncode


def trigger_match_round(round_name: str) -> int:
    if round_name not in {"first", "next"}:
        raise RuntimeError("未知的比赛轮次触发命令")
    command = (
        "cd /opt/iking/match_agent_flow_test && "
        f"exec ./scripts/trigger-round.sh {round_name}"
    )
    return run_remote(command).returncode


def trigger_classmate_round(round_name: str) -> int:
    if round_name not in {"first", "next"}:
        raise RuntimeError("未知的同学程序轮次触发命令")
    command = (
        "cd /opt/iking/match_agent_manual && "
        f"exec ./scripts/trigger-round.sh {round_name}"
    )
    return run_remote(command).returncode


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Windows CMD 一键启动无人机程序（同学原程序保持不变）"
    )
    parser.add_argument(
        "action",
        choices=[
            "check",
            "status",
            "set-sim",
            "set-real",
            "test-sim",
            "test-real",
            "match-sim",
            "match-demo-sim",
            "match-dry",
            "match-first",
            "match-next",
            "classmate-sim",
            "classmate-real",
            "classmate-dry",
            "classmate-manual-sim",
            "classmate-manual-real",
            "classmate-manual-dry",
            "classmate-first",
            "classmate-next",
            "stop-test",
            "stop-match",
            "stop-classmate",
            "stop-classmate-manual",
        ],
    )
    parser.add_argument(
        "altitude",
        nargs="?",
        help="性能测试高度，默认 3 米，范围 1 到 20 米",
    )
    return parser


def parse_arguments(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = build_parser()
    args = parser.parse_args(arguments)
    if args.action in {"test-sim", "test-real"}:
        try:
            args.altitude = 3.0 if args.altitude is None else float(args.altitude)
        except ValueError:
            parser.error("性能测试高度必须是数字")
    elif args.altitude is not None:
        parser.error(f"{args.action} 命令后面不能再加参数")
    return args


def main() -> int:
    args = parse_arguments()
    require_ssh()
    if args.action == "check":
        return check_setup()
    if args.action == "status":
        return show_status()
    if args.action == "set-sim":
        return set_environment("sim")
    if args.action == "set-real":
        return set_environment("real")
    if args.action == "test-sim":
        return run_performance("sim", args.altitude)
    if args.action == "test-real":
        return run_performance("real", args.altitude)
    if args.action == "match-sim":
        return run_match(False)
    if args.action == "match-demo-sim":
        return run_match(False, use_simulation_oracle=True)
    if args.action == "match-dry":
        return run_match(True)
    if args.action == "match-first":
        return trigger_match_round("first")
    if args.action == "match-next":
        return trigger_match_round("next")
    if args.action == "classmate-sim":
        return run_classmate("sim", False)
    if args.action == "classmate-real":
        return run_classmate("real", False)
    if args.action == "classmate-dry":
        return run_classmate("sim", True)
    if args.action == "classmate-manual-sim":
        return run_classmate_manual("sim", False)
    if args.action == "classmate-manual-real":
        return run_classmate_manual("real", False)
    if args.action == "classmate-manual-dry":
        return run_classmate_manual("sim", True)
    if args.action == "classmate-first":
        return trigger_classmate_round("first")
    if args.action == "classmate-next":
        return trigger_classmate_round("next")
    if args.action.startswith("stop-"):
        return stop_controller(args.action.removeprefix("stop-"))
    raise RuntimeError("未知操作")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"错误：{error}", file=sys.stderr)
        raise SystemExit(1)
