# SDK C++ examples

Short, one-topic samples for `iking_drone_sdk`. Build with the SDK target; binaries land next to the library.

| Binary | What it shows |
|--------|----------------|
| `01_connect` | connect + `getSystemStatus` |
| `02_observe_callbacks` | `onEvent` / `onStatus` / `onHeartbeat`（可选秒数）；status≈2Hz、heartbeat≈1Hz |
| `03_flight_basic` | **交互菜单**：takeOff/点动/任务/返航；15/16/17 开关打印；`taskGuidance` 经 event 始终打印 |
| `04_pod_basic` | **交互菜单**：云台/夹手/变焦/拍照录像/红外；`h` 看命令 |
| `05_pod_frame_callback` | **交互菜单**：吊舱 `PodVisibleLight` 回调 + overlay；`1` 设字 / `2` 清除 |
| `06_pod_frame_acquire` | **交互菜单**：吊舱拉模式；`1` 单帧 / `2` 循环；可 toggle 存 JPG |
| `07_front_camera_frame_callback` | **交互菜单**：前向视频流（`Front`）回调 + overlay |
| `08_front_camera_frame_acquire` | **交互菜单**：前向 camera 拉模式 + 可选存 JPG |
| `09_fisheye_depth_callback` | **交互菜单**：鱼眼深度回调；可 toggle 存 `.bin`/`.png` |
| `10_fisheye_depth_acquire` | **交互菜单**：鱼眼深度拉模式 + min/mean/max；可 toggle 落盘 |

`03`–`10` 启动后输入 `h` 看菜单，`q` 退出。`05`–`10` 帧示例需 OpenCV 才支持 overlay/存 JPG/伪彩（未编 OpenCV 时仍可收帧）。

Requires `iking_drone_app` (and for frame samples, media pipeline / frame_share) on the same host/DDS domain.

Pod infrared uses the same frame APIs with `StreamChannelType::PodInfrared`. Front camera RGB uses `StreamChannelType::Front`. Fisheye depth uses `StreamChannelType::FisheyeDepth` (one-way). Samples stay single-stream for readability.

## Build from installed SDK (deb)

安装后源码在 `/opt/iking/drone_sdk/share/example/`（含本目录 `CMakeLists.txt`）：

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
cd /opt/iking/drone_sdk/share/example
cmake -S . -B build && cmake --build build -j
# 二进制在 build/01_connect …
```

依赖：`cmake`、`pkg-config`、`libjsoncpp-dev`、`libcurl`；05–10 可选 OpenCV 4。
