# iking_drone_sdk

DDS client for `iking_drone_app` internal control channel (`/iking/internal/control`).

## Deliverables

| Artifact | Path (deb 安装后) |
|----------|------------------|
| C++ header | `/opt/iking/drone_sdk/include/iking_drone_sdk.h` |
| C++ library | `/opt/iking/drone_sdk/lib/libiking_drone_sdk.so`（**唯一 .so**，内嵌 FastDDS） |
| pkg-config | `/opt/iking/drone_sdk/share/iking_drone_sdk/iking_drone_sdk.pc` |
| env 脚本 | `/opt/iking/drone_sdk/env/iking_drone_sdk.sh` |
| C++ example | `/opt/iking/drone_sdk/bin/01_connect` … `10_fisheye_depth_acquire`（源码见 `share/example/`） |
| 说明书 | `/opt/iking/drone_sdk/share/iking_drone_sdk/飞机通用控制SDK说明书.md`（中文） |
| Manual | `/opt/iking/drone_sdk/share/iking_drone_sdk/iking_drone_sdk_manual.md`（English） |

`postinst` 会软链到 `/usr/local/include` 与 `/usr/local/lib`。

## Build

```bash
./build.sh 2 third_party       # 必须先编（app 用 shared FastDDS；同时安装 static .a 供 SDK）
./build.sh 2 iking_protocol
./build.sh 2 iking_drone_sdk   # 默认 IKING_DRONE_SDK_BUNDLE_DDS=ON，链接 third_party 预编 static .a
```

`third_party` 一次编译 FastDDS，同时安装 `libfastrtps.so` / `libfastcdr.so` 与对应 `.a`（供 SDK whole-archive 内嵌）。

`iking_protocol` 已静态链入；帧通道复用 media_app 同款 v1 SHM（编译进 SDK）。系统依赖仍为 `jsoncpp`、`libcurl`。

跨进程 DDS SHM 与 `iking_drone_app` 互通要求：**同源 `third_party` + 同平台编译**（不必共享同一 `.so` 文件）。

开发机快速迭代可临时关闭内嵌：`-DIKING_DRONE_SDK_BUNDLE_DDS=OFF`（回退动态 `install/third_party/lib/libfastrtps.so`）。

## 系统安装（DRONE_SDK deb）

```bash
./build.sh 2 package_sdk V1.2.3.4
sudo dpkg -i packaging/output/DRONE_SDK_V1.2.3.4.orin.deb
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
```

与 `DRONE_APP` / `DRONE_MODELS` 独立 OTA；升应用 deb **不会**清除 `/opt/iking/drone_sdk`。

## C++ usage

```cpp
#include "iking_drone_sdk.h"

iking::drone::Client client;
client.connect();
auto r = client.getSystemStatus();
if (isOk(r.status)) { /* r.result_json, r.request_id */ }

// Frame (PodVisibleLight): set callback before open for push mode
client.setFrameCallback(iking::drone::StreamChannelType::PodVisibleLight, [](iking::drone::FrameView& f) {
    (void)f; /* thin process; auto-released after return */
});
auto fr = client.openFramePool(iking::drone::StreamChannelType::PodVisibleLight);
client.disconnect();
```

Short samples: `01_connect` … `10_fisheye_depth_acquire` under `sdk/example/`（deb 安装后为 `/opt/iking/drone_sdk/share/example/`，可用该目录下 `CMakeLists.txt` 独立重编）。

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
g++ app.cpp $(pkg-config --cflags --libs iking_drone_sdk)
```

或重编全部示例：

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
cd /opt/iking/drone_sdk/share/example
cmake -S . -B build && cmake --build build -j
```

NFS 开发（未装 deb）：

```bash
export LD_LIBRARY_PATH=/root/drone_app/install/lib:$LD_LIBRARY_PATH
g++ app.cpp -I/root/drone_app/install/include -L/root/drone_app/install/lib -liking_drone_sdk -ljsoncpp -lcurl
```

## Python usage

```bash
export LD_LIBRARY_PATH=/opt/iking/drone_sdk/lib:$LD_LIBRARY_PATH
export PYTHONPATH=/path/to/install/lib/python3/dist-packages:$PYTHONPATH
python3 share/example/simple_control.py
```

（Python bindings 默认未启用。）

## Protocol 2.0 commands (DDS)

`get_system_status`, `list_devices`, `get_device`, `arm`, `takeoff`, `land`, `server_command`, `drone_control`, `pod_control`, `update_dock_status`

Request envelope (command.name = method, 1:1 mapping):

```json
{
  "version": "2.0",
  "type": "command",
  "needResponse": true,
  "session": "iking_drone_sdk",
  "requestId": "...",
  "command": {
    "version": "1.0",
    "name": "get_system_status",
    "data": {}
  }
}
```

Response uses `response.status` (0 = success), `response.data`, `response.message`.

### Pod control (圣2025 protocol command.name)

DDS `command.name` = pod command name (single layer, no `setPod*` mapping). Unregistered system commands fallback to pod passthrough.

```cpp
client.podCtrl("cameraIspExposureSet", params);
client.gimbalControlSpeedSet(10.f, 0.f, 0.f);
```

**Tier-1** convenience methods: `gimbalControlSpeedSet`, `gimbalControlAngleSet`, `gimbalCenterSet`, `gimbalDownSet`, `gimbalStopSet`, `gripperOpen` / `gripperClose` (yaw ±90 via angle set), `cameraZoom`, `capturePhoto`, `recordOn`, `recordOff`, `cameraTempPointSet`, `cameraPlateSet`.

**Tier-2**: any command in `圣2025吊舱协议(2.0)规范.md` via `podCtrl`.

Server rate limit: 25 req/s per `session` (burst 50).

## Requirements

- `libjsoncpp-dev`, `libcurl`
- `iking_drone_app` running with `dds_internal` channel enabled
- Same DDS domain (default `0`) and topic names
- `Config.local_only` must match app `channels[].config.local_only` (default `true`：SHM + 本机 UDP；非纯 SHM)
- 若开机自启后 SDK `PublishFailed`、软重启 app 后又正常：已在 transport 加本机 UDP 兜底；仍异常时可 `rm -f /dev/shm/fastrtps*` 再起 app

## Config

| Field | Default | Description |
|-------|---------|-------------|
| `local_only` | `true` | SHM transport only; blocks cross-device DDS |
