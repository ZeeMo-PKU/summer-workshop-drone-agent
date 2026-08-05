# 飞机通用控制 SDK 说明书

| 项 | 说明 |
|----|------|
| 产品名 | `iking_drone_sdk` |
| 通信 | FastDDS + Protocol 2.0 JSON |
| 头文件 | `iking_drone_sdk.h` |
| 动态库 | `libiking_drone_sdk.so` |


---

# 1. 通用介绍

## 1.1 SDK 简介

`iking_drone_sdk` 是面向 **同机（或同 DDS Domain）第三方进程** 的 C++ 控制客户端库。


## 1.2 适用场景

| 场景 | 说明 |
|------|------|
| 第三方 C++ 业务进程 | 与应用解耦提供统一的内部控制接口，简化第三方应用开发 |

## 1.3 主要功能

- 连接管理：`connect` / `disconnect` / `isConnected`
- 系统与设备查询：`getSystemStatus`、`listDevices`、`getDevice`、`getPodCapability`
- 飞行与任务：起飞、返航、模式切换、航线任务、暂停/恢复/强制结束、点动速度/位置
- 吊舱控制：通用控制指令和封装的快捷控制指令
- 异步信息：`onStatus` / `onEvent` / `onHeartbeat`
- 媒体帧：`Client` 取帧 API（`openFramePool` / 回调或 `acquireFrame`）；见示例 `05`/`06`

## 1.4 版本说明

- 安装包版本以 `DRONE_SDK_*.deb` / pkg-config `Version` 为准，可以使用dpkg -l 查看具体SDK的版本


## 1.5 文档约定

| 约定 | 说明 |
|------|------|
| 命名空间 | `iking::drone` |


---

# 2. SDK 概览

## 2.1 架构介绍

```text
第三方 Application
        │
        ▼
 iking::drone::Client                     ← 公开 API（含取帧）
        │
        ▼
 Session（统一封装：RPC pending + 多路 FramePool）
        │
        ├─ DdsTransport（FastDDS，默认静态打进 .so）
        └─ FramePool × N（按 StreamChannelType，内部）
        │
        ▼  Topic: /iking/internal/{control,control_reply,status,event,heartbeat}
        │
 iking_drone_app  (DdsChannel → ddsCmdDispatch → ApiGateway → DroneAPISurface)
        │
        ├─ Business / HSM / DeviceManager
        └─ media_pipeline_worker / frame_share（帧经 SHM）
```

**数据流（控制）**：

1. `Client` 组装 Protocol 2.0 `command`
2. 发布到 `control` Topic（RELIABLE + VOLATILE）
3. App 处理后写 `control_reply`（RELIABLE + TRANSIENT_LOCAL）
4. SDK 按 `requestId` 匹配，填入 `Result`

**调用流程**：`Config` → `Client` → `connect()` → API / 回调 → `disconnect()`。

## 2.2 目录结构（deb 安装后）

```text
/opt/iking/drone_sdk/
├── include/
│   └── iking_drone_sdk.h              # 主 API（含取帧）
├── lib/
│   └── libiking_drone_sdk.so          # 唯一需链接的 .so（默认内嵌 FastDDS）
├── bin/                               # 预编译示例（与 share/example 源码对应）
│   ├── 01_connect
│   ├── 02_observe_callbacks
│   ├── 03_flight_basic
│   ├── 04_pod_basic
│   ├── 05_pod_frame_callback
│   ├── 06_pod_frame_acquire
│   ├── 07_front_camera_frame_callback
│   ├── 08_front_camera_frame_acquire
│   ├── 09_fisheye_depth_callback
│   └── 10_fisheye_depth_acquire
├── env/
│   └── iking_drone_sdk.sh             # IKING_DRONE_SDK_ROOT / LD_LIBRARY_PATH / PKG_CONFIG_PATH
└── share/
    ├── iking_drone_sdk/
    │   ├── iking_drone_sdk.pc
    │   ├── version.txt
    │   ├── README_SDK.md
    │   ├── iking_drone_sdk_manual.md   # English manual
    │   └── 飞机通用控制SDK说明书.md   # 中文说明书
    └── example/                       # 示例源码 + CMakeLists.txt + README.md
```

`postinst` 会软链头文件/库到 `/usr/local/include`、`/usr/local/lib`，并可选执行 `ldconfig`。

源码树（开发）：`drone_app/sdk/{include,src,example,python,CMakeLists.txt,README_SDK.md,iking_drone_sdk_manual.md,飞机通用控制SDK说明书.md}`。

示例程序重编示例（装 deb 后）：

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
cd /opt/iking/drone_sdk/share/example
mkdir build && cd build && cmake .. && make -j6
cmake -S . -B build && cmake --build build -j
# 二进制在 build/01_connect …
```

---

# 3. 安装与部署

## 3.1 安装（dpkg）

```bash
sudo dpkg -i DRONE_SDK_V1.2.3.4.orin.deb
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
```

安装路径为 `/opt/iking/drone_sdk/`（目录结构见 2.2）。

## 3.2 环境变量

| 变量 | 作用 |
|------|------|
| `LD_LIBRARY_PATH` | 找到 `libiking_drone_sdk.so` |
| `PKG_CONFIG_PATH` | 找到 `iking_drone_sdk.pc`（env 脚本通常已设置） |


## 3.3 验证安装

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
# 确认机载 iking_drone_app_node 已运行且 dds_internal 已起来
/opt/iking/drone_sdk/bin/01_connect
```

预期：`connect()` 成功并能 `getSystemStatus()` 返回 `StatusCode::Ok`。

---

# 4. 快速开始（Quick Start）

## 4.1 Hello World

```cpp
#include "iking_drone_sdk.h"
#include <iostream>

int main() {
    iking::drone::Config cfg;          // 一般使用默认配置
    iking::drone::Client client(cfg);
    if (!client.connect()) {
        std::cerr << "connect failed\n";
        return 1;
    }
    auto r = client.getSystemStatus();
    if (iking::drone::isOk(r.status)) {
        std::cout << r.result_json << "\n";
    } else {
        std::cerr << "error=" << r.error << " code=" << static_cast<int>(r.status) << "\n";
    }
    client.disconnect();
    return 0;
}
```

编译：

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
g++ -std=c++14 hello.cpp -o hello $(pkg-config --cflags --libs iking_drone_sdk)
./hello
```

## 4.2 自带示例程序清单

安装后预编译二进制在 `/opt/iking/drone_sdk/bin/`，源码在 `/opt/iking/drone_sdk/share/example/`（与仓库 `sdk/example/` 一致）。

```bash
source /opt/iking/drone_sdk/env/iking_drone_sdk.sh
```

| 程序 | 能做什么 | 怎么用 |
|------|----------|--------|
| `01_connect` | 连上飞机主程序，查一下系统是否正常 | 装好后先跑这个，确认能通 |
| `02_observe_callbacks` | 持续听飞机上报的状态、事件、心跳 | 默认可听约 10 秒；看控制台有没有消息刷出来 |
| `03_flight_basic` | 用菜单操作飞行：起飞、移动、航线、返航等 | 启动后按提示选数字；可开关是否打印状态 |
| `04_pod_basic` | 用菜单操作吊舱：转云台、变焦、拍照录像、红外、夹手 | 启动后按提示选数字；输入 `h` 看全部命令 |
| `05_pod_frame_callback` | 接收吊舱可见光画面，并可在画面上叠英文字 | 菜单里设字或清空；需要编进图像库才能叠字 |
| `06_pod_frame_acquire` | 主动拉取吊舱可见光画面，可按需存成图片 | 菜单里可拉一帧或连拉多帧，也可打开存图 |
| `07_front_camera_frame_callback` | 接收机头前视相机画面，可叠英文字 | 用法类似 `05`，通道是前视相机 |
| `08_front_camera_frame_acquire` | 主动拉取前视相机画面，可按需存图 | 用法类似 `06` |
| `09_fisheye_depth_callback` | 接收四周深度感知数据，可按需存文件 | 菜单里可打开存盘；有图像库时还能存彩色预览图 |
| `10_fisheye_depth_acquire` | 主动拉取深度数据，并打印远近统计 | 菜单里可拉一帧或连拉多帧，也可打开存盘 |

建议顺序：先 `01` 确认连通 → `02` 看有没有推送 → 再按需跑飞控 `03` 或吊舱 `04` → 要看画面/深度时再跑 `05`–`10`。带菜单的示例启动后输入 `h` 看帮助、`q` 退出。画面类示例还需要机上媒体相关服务已就绪。

索引与重编说明见 `sdk/example/README.md`；装 deb 后也可在 `share/example/` 用自带 `CMakeLists.txt` 重编。

---

# 5. 核心概念

## 5.1 生命周期

```text
构造 Client(Config)
        ↓
   connect()          ← 创建 Transport，等待 control Reader matched（约数秒）
        ↓
   RPC / 回调运行中
        ↓
   disconnect()
        ↓
   析构
```

**Note**：`connect` / `disconnect` 与业务 `call` 的线程模型见 5.4；起飞/返航等为机上异步任务时，RPC **成功只表示命令已处理**，最终态请用 `onEvent` / 飞控事件观察。

## 5.2 对象关系

```text
Client
 ├── Config          # DDS和topic基础配置,缺省值已经够用
 ├── Session(client内部)    # 命令、回复、心跳、状态、事件
 └── 回调注册         # status / event / heartbeat / 帧回调
```

## 5.3 数据流

**控制 RPC**：

```text
App 线程 call()
 → 发 control(JSON)
 → 阻塞等 control_reply(requestId)
 → Result
```

**状态推送**：

```text
App Writer(status/event/heartbeat)
 → SDK Reader
 → 用户回调（实现方线程由 DDS 接收路径触发，回调内勿做长时间阻塞）
```

**视频帧**：

```text
media APP(IPC)
 → SDK receive(IPC) → setFrameCallback / acquireFrame → FrameView（堆拷贝）
 → release（回调方式自动处理release、 主动获取+释放模式需要显式调用）
 → SDK send(IPC) 写回帧 → 下游帧处理
```

## 5.4 线程安全

| 接口 | 线程安全说明 |
|------|----------------|
| `connect` / `disconnect` | **不要**与同实例的控制指令调用并发随意交叉；先停调用再断连 |
| `飞行及调用控制接口`  | 同一`Client`上调用由内部session串行化；多线程多client同时调用需自行理解处理串行并发调用 |
| `onStatus` / `onEvent` / `onHeartbeat` | 回调内建议只投递队列，勿在回调里再死锁式调用或者回调内部执行耗时操作|
| 取帧回调:`setFrameCallback` | 回调内处理需快速,不要有耗时操作，回到函数内处理时间不要超过((1000/fps)-2)ms，否则可能导致丢帧。 不要保留回调函数输入的帧数据指针，回调接收后帧数据指针失效 |
| 取帧调用:`acquire+release` | 取用帧后，必须调用释放接口。否则可能导致帧阻塞进而丢帧 |

---

# 6. API 使用说明

以下均在命名空间 `iking::drone`。默认 RPC 超时 `timeout_ms = 5000`。

## 6.1 类型与结果

### Config

| 字段 | 默认 | 说明 |
|------|------|------|
| `domain_id` | `0` | 须与 app `dds_internal.config.domain_id` 一致 |
| `client_id` | `"iking_drone_sdk"` | 写入信封 `session`，并用于服务端按 session 控制 |
| `control_topic` | `/iking/internal/control` | |
| `control_reply_topic` | `/iking/internal/control_reply` | |
| `status_topic` | `/iking/internal/status` | 空则 SDK 使用该默认，与 app `dds_internal` 一致 |
| `event_topic` | `/iking/internal/event` | 同上 |
| `heartbeat_topic` | `/iking/internal/heartbeat` | 同上 |

### StatusCode

| 值 | 含义 |
|----|------|
| `Ok` (0) | 成功（且 wire `response.status == 0`） |
| `Failed` (-1) | 业务失败；见 `Result.error` / message |
| `NotConnected` (1001) | 未连接 |
| `PublishFailed` (1002) | 发布失败 |
| `Timeout` (1003) | RPC 等待 reply 超时 |
| `InvalidArgument` (1004) | 参数非法（如 JSON 解析失败） |
| `DeviceOffline` (1005) | 吊舱/设备离线 |
| `StreamUnavailable` (1006) | 无对应 `StreamChannelType` 通道 |
| `StreamNotReady` (1007) | worker 未就绪 |
| `FrameOpenFailed` (1008) | SHM 打开失败 |
| `FrameBusy` (1009) | 推/拉互斥或未 release 再 acquire |
| `FrameTimeout` (1010) | `acquireFrame` 等待帧超时 |

`isOk(code)`：是否为 `Ok`。

### Result

| 字段 | 说明 |
|------|------|
| `status` | `StatusCode` |
| `result_json` | 成功时 `response.data` 的 JSON 字符串 |
| `error` | 失败信息 |
| `request_id` | 本次 `requestId` |
| `msg` | `response.data` 解析后的 `Json::Value`（吊舱等） |

### DRONE_MODE_STATUS

`STANDBY` / `LANDING` / `POSITION` / `MISSION` / `TAKEOFF`。

## 6.2 连接

| 方法 | 说明 |
|------|------|
| `explicit Client(const Config& = {})` | 构造 |
| `bool connect()` | 连接 DDS，等待配对 |
| `void disconnect()` | 断开 |
| `bool isConnected() const` | 是否已连接 |

## 6.3 系统与设备

| 方法 | 说明 |
|------|------|
| `getSystemStatus` | 系统状态 |
| `listDevices` | 设备列表 |
| `getDevice(device_id)` | 单设备 |
| `getPodCapability` | 吊舱能力（含 streamChannels / stream_name / shm 等 enrich） |

## 6.4 飞行与任务

| 方法 | 说明 | 注意 |
|------|------|------|
| `takeOff(height)` | 起飞，高度米 | |
| `returnToHome` | 返航机巢降落 | 异步任务；成功=命令受理 |
| `returnToAnyDock(...)` | 返航任意机库 | 含 dockSN/经纬高/方向 |
| `returnToAnyPosition(...)` | 返航任意点 | 异步 |
| `getModeStatus(mode)` | 查询模式 | |
| `setPositionMode` | 进位置模式 | 任务中调用约等于暂停任务 |
| `setMissionMode(route_id)` | 任务模式，按航线 id 拉云端 | **须已起飞完成** |
| `setMissionModeByFile` | 本地航线文件 | 同上 |
| `setMissionModeByRoute` | JSON 航线 | 接口预留语义见头文件 todo |
| `pauseMission` / `resumeMission` | 暂停/恢复任务 | 暂停后偏 Position 悬停 |
| `forceStopTask` | 强制结束任务 | |
| `stopMove` | 停速度/相对点动 | Position 且已解锁 |

### 位置模式下移动

| 方法 | 说明 |
|------|------|
| `setSpeed(x,y,z,yaw)` | 机体轴速度 m/s、航向 °/s |
| `setPosition(lon,lat,alt, yaw=NaN)` | 绝对位置；`yaw` 绝对航向（度，北=0 顺时针正）；NaN/未传保持当前航向 |
| `setRelativePosition(x,y,z, yaw=NaN, timeout, speed)` | 相对位移；`yaw` 相对当前航向偏移（度）；`x=y=z=0` 且传 `yaw` 时为原地转航向 |

**Warning**：移动类接口仅在 Position 且解锁后按 app 侧实现生效；误用可能导致非预期位移。

## 6.5 吊舱控制

### 通用方法podCtrl

`command` = 通用2.0协议 `command.name`；`data` = `command.data`。  
未注册的系统名由 app **fallback 透传**吊舱。

重载：

- `podCtrl(command, data)`
- `podCtrl(command, data, targets)`
- `podCtrl(command, data, targets, pod_class)` — **class 优先于 targets**；`mask`：0 全部 / 1 可见光 / 2 红外
- `podCtrl(command, data_json_string)`


### 功能封装方法

| 方法 | 功能说明 |
|------|----------|
| `gimbalControlSpeedSet` | 按角速度转动云台（偏航/俯仰/横滚，单位：度/秒） |
| `gimbalControlAngleSet` | 按目标角度转到指定姿态（横滚/俯仰/偏航，单位：度） |
| `gimbalCenterSet` | 云台回中 |
| `gimbalDownSet` | 云台朝下 |
| `gimbalStopSet` | 云台停止转动 |
| `gripperOpen` / `gripperClose` | 夹手张开 / 闭合（仅夹手吊舱） |
| `cameraZoom` | 变焦 |
| `capturePhoto` | 拍照；可指定本地路径由 SDK 下载保存 |
| `recordOn` / `recordOff` | 开始/停止录像；`recordOff` 可指定本地路径下载 |
| `cameraTempPointSet` | 红外画面点测温（归一化坐标，一般 0~1） |
| `cameraPlateSet` | 切换红外伪彩色板 |
**Note**：`capturePhoto`/`recordOff` 带本地路径时 SDK 可经 HTTP 下载；无路径时调用方自行处理 `httpPath`。服务端对同一 `session` 约 **10 req/s**。

## 6.6 回调说明

主应用会持续往外推状态、事件、心跳；业务侧通过回调函数接收，而不是主动轮询。

### 回调函数类型

| 类型 | 签名 | 含义 |
|------|------|------|
| `StatusCallback` | `void(const std::string& json, void* userdata)` | 收到状态推送 |
| `EventCallback` | 同上 | 收到事件推送 |
| `HeartbeatCallback` | 同上 | 收到心跳推送 |
| `FrameCallback` | `void(FrameView& frame, void* userdata)` | 收到一帧画面（见 6.7） |

`json` 为 UTF-8 字符串；`userdata` 为注册时传入的自定义指针，可为 `nullptr`。

### 如何注册

在 `connect()` **之后**注册：

```cpp
void onEvent(const std::string& json, void* /*userdata*/) {
    // 建议：只做轻量处理或丢进队列，勿长时间阻塞
    std::cout << "[event] " << json << std::endl;
}
void onStatus(const std::string& json, void* /*userdata*/) { /* ... */ }
void onHeartbeat(const std::string& json, void* /*userdata*/) { /* ... */ }

client.onEvent(onEvent, nullptr);
client.onStatus(onStatus, nullptr);
client.onHeartbeat(onHeartbeat, nullptr);
```

也可使用 `std::function` / lambda。同一 `Client` 上重复注册会覆盖上一次。完整示例见 `02_observe_callbacks`。

画面帧回调用 `setFrameCallback`，须在 `openFramePool` **之前**设置，细节见 6.7。

### 各回调推送内容

数据由 `iking_drone_app` 的 `DdsChannel`（`dds_internal`）发布到 Topic，SDK 订阅后回调：

| 回调 | 注册方法 | Topic（默认） | 推什么、多久一次 |
|------|----------|---------------|------------------|
| 状态 | `onStatus` | `/iking/internal/status` | Protocol 2.0 `type=status`：飞机 `flight` + 吊舱 `pod`（可选 `flightPath`）；约 **500ms**（2 次/秒） |
| 事件 | `onEvent` | `/iking/internal/event` | Protocol 2.0 `type=event`：设备/告警/裁判指引等异步事件（见下表） |
| 心跳 | `onHeartbeat` | `/iking/internal/heartbeat` | 轻量 keepalive；约 **1s** 一次 |
| 画面 | `setFrameCallback` | （SHM，非上述 Topic） | 帧到达时给出 `FrameView` |

### 心跳示例（`onHeartbeat`）

`DdsChannel` 每秒调用 `ObserveStatusBuilder::buildHeartbeat`，字段固定：

```json
{
  "version": "2.0",
  "type": "heartbeat",
  "ts": 1722067200123,
  "source": "iking_drone_app",
  "deviceId": "DRONE_SN_xxx"
}
```

| 字段 | 说明 |
|------|------|
| `ts` | Unix 毫秒时间戳 |
| `source` | 固定 `iking_drone_app` |
| `deviceId` | 设备 SN（无 SN 时可能省略） |

### 状态示例（`onStatus`）

周期组包：`collectFlightControlTelemetry` + `collectPodTelemetry` + `getActiveStatePath` → Protocol 2.0 status。信封示意：

```json
{
  "version": "2.0",
  "type": "status",
  "timestamp": "2026-07-27 13:49:00",
  "requestId": "...",
  "status": {
    "version": "1.0",
    "name": "observe",
    "state": "periodic",
    "data": {
      "flight": {
        "isArmed": false,
        "flightMode": "FLY_MODE_POSITION",
        "sdkMode": false,
        "positionStatus": {
          "useRTK": true,
          "longitude": 116.39,
          "latitude": 39.90,
          "altitude": 50.0,
          "relativeDockAltitude": 0.0,
          "satelliteCount": 18,
          "rtkStatus": 0,
          "relativeGroundDistance": 1.2,
          "roundRadarDistance": null
        },
        "speed": { "x": 0.0, "y": 0.0, "z": 0.0, "total": 0.0 },
        "attitude": { "roll": 0.0, "pitch": 0.0, "yaw": 90.0 },
        "windInfo": { "theta_deg": 0.0, "speed": 0.0 }
      },
      "pod": {
        "camStatus": { "podType": 1, "zoomLevel": 1, "record": 0, "recordTime": 0.0 },
        "camera": { "light": { "zoom": 1, "...": "..." }, "ir": { "...": "..." } },
        "laserRanging": { "type": "continuous", "distance": 0.0, "lat": 0.0, "lon": 0.0, "alt": 0.0 },
        "infrared": { "currentTemp": 0.0, "highTemp": 0.0, "lowTemp": 0.0 },
        "gimbal": { "roll": 0.0, "pitch": 0.0, "yaw": 0.0 },
        "lidarStatus": { "isWork": 0, "imuIsWork": 0 },
        "lightStatus": { "...": "..." },
        "gasStatus": { "...": "..." }
      },
      "flightPath": "/mission/idle"
    }
  }
}
```

`flightPath` 仅在 HSM 当前路径非空时出现；`pod` 子树随吊舱型号有缺省/零值字段。

### 支持的事件类型（`onEvent`）

`DdsChannel` 写入 `/iking/internal/event` 后，SDK 以 JSON 回调。**事件类型**看 `event.name`（除 `taskGuidance` 外，与 `event.data.event_type` 相同）。当前支持：

| 事件类型（`event.name`） | 含义 |
|--------------------------|------|
| `flight_event` | 飞行生命周期 / 自检失败 / 故障告警（见下方 code 表） |
| `device_lidar` | 激光雷达数据 |
| `device_remote_id` | Remote ID / WiFi 模块状态 |
| `device_extension` | 扩展告警 |
| `taskGuidance` | 裁判 / 任务指引文案 |

除 `taskGuidance` 外，载荷为 `event.data.body`；`taskGuidance` 的业务字段直接在 `event.data`。

#### `flight_event` 的 `body.code`（业务码）

`body.level` 为 `I` / `W` / `E`。码表来自主应用 `ErrorCatalog`（生命周期 + 自检映射）：

**生命周期 / 运维**

| code | level | 说明 |
|------|-------|------|
| `1002` | I | 无人机自检成功 |
| `1012` | I | 无人机正在起飞 |
| `1013` | I | 无人机起飞完成 |
| `1021` | I | 开始执行航线 |
| `1022` | I | 无人机到达航点（航线 mission） |
| `1034` | I | 任务完成 |
| `1145` | I | 位置移动开始（点动 setPosition / setRelativePosition） |
| `1146` | I | 飞行到位（点动到达目标） |
| `1147` | I | 进入SDK模式 / 退出SDK模式（飞控 sdkMode 边沿，文案即当前模式） |
| `1037` | I | 暂停任务命令成功 |
| `1038` | E | 暂停任务命令失败 |
| `1039` | I | 继续任务命令成功 |
| `1040` | E | 继续任务命令失败 |
| `1061` | I | 开始返航 |
| `1069` | I | 无人机返航航线已完成 |
| `1076` | I | 无人机降落完成 |
| `1080` | I | 无人机开始降落 |
| `1090` | E | 无人机降落在机库 但位置偏差超过阈值 |
| `1139` | I | 进入执行航点动作命令 |
| `1140` | I | 航点动作命令执行完成 |
| `1142` | W | 机库 MQTT 未连接（本地约定，起飞自检跳过对频时上报） |
| `1143` | E | 机库 MQTT 未连接 无法机库联动降落 |
| `1144` | I | BMS 强制下电 |
| `1301` | E | 无人机起飞执行失败 |
| `2206` | E | 触发智能返航 |
| `2207` | E | 触发低电量返航 |
| `2210` | W | 超过三分钟无动作 开始返航 |
| `2221` | E | 触发风速过大返航 |

**自检失败（起飞自检未通过时上报）**

| code | level | 说明 |
|------|-------|------|
| `0919` | E | 无人机自检失败 无人机令牌接口连接失败 |
| `1004` | E | 无人机自检失败 卫星数少于16颗 |
| `1005` | E | 无人机自检失败 未在SDK模式 |
| `1006` | E | 无人机自检失败 未进固定解 |
| `1008` | E | 无人机自检失败 吊舱卡扣未夹住 |
| `1009` | E | 无人机自检失败 电池卡扣未夹住 |
| `1019` | E | 无人机自检失败 起飞电量低于设定值 |
| `1020` | E | 无人机自检失败 起飞点与机库点不一致 |
| `1024` | E | 请求航线失败 没有正确的分析出结果 |
| `1030` | E | 未识别到吊舱类型 |
| `1086` | E | 无人机自检失败 总航线长度超过12千米 |
| `1087` | E | 无人机自检失败 没有备降点 |
| `1131` | E | 无人机自检失败 0.5s内位置误差高于30CM |
| `1132` | E | 无人机自检失败 飞机姿态倾斜超过15度 |
| `1133` | E | 无人机自检失败 具体原因查看飞机状态 |
| `1135` | E | 飞机与机库未对频 |
| `1143` | E | 无人机自检失败 机库模式下机库MQTT未连接 |

#### JSON 示例

**`flight_event`：**

```json
{
  "version": "2.0",
  "type": "event",
  "timestamp": "2026-07-27 13:49:00",
  "requestId": "...",
  "event": {
    "version": "1.0",
    "name": "flight_event",
    "severity": 0,
    "message": "ok",
    "data": {
      "event_type": "flight_event",
      "body": {
        "deviceId": "DRONE_SN_xxx",
        "deviceName": "DRONE_SN_xxx",
        "missionId": "",
        "nxLogtimestamp": "2026-07-27 13:49:00.123",
        "code": "2207",
        "level": "W",
        "logData": "开始返航",
        "logMsg": "2207,W,开始返航"
      }
    }
  }
}
```

**`taskGuidance`：**

```json
{
  "version": "2.0",
  "type": "event",
  "timestamp": "2026-07-27 13:49:00",
  "requestId": "...",
  "event": {
    "version": "1.0",
    "name": "taskGuidance",
    "severity": 0,
    "message": "ok",
    "data": {
      "message": "请飞往目标点"
    }
  }
}
```

`device_lidar` / `device_remote_id` / `device_extension` 信封同 `flight_event`（`data.event_type` + `data.body`），`body` 为设备原始 JSON。

解析：先看 `event.name`；`flight_event` 再读 `body.code` / `body.level` / `body.logData`。

### 注意事项

- 先 `connect`，再依赖 status/event/heartbeat
- 回调里避免长时间占用；不要在回调里对同一 `Client` 做可能死锁的同步控制调用
- `disconnect` / 析构前，回调仍可能在跑，注意 `userdata` 指向对象的生命周期
- 可用心跳超时（如连续约 3 秒无心跳）判断链路异常
- status / event 为 Protocol 2.0 信封；heartbeat 为独立轻量 JSON（无 `status`/`event` 子对象）


## 6.7 取帧（Client）

取帧能力在 `Client` 上；内部按 `StreamChannelType` 管理多路（可见光 / 红外可同时 open）。

| 方法 | 说明 |
|------|------|
| `openFramePool(type, opts)` | 内部 `getPodCapability`，挂 `share_stream_name`（send）与 `share_recv_stream_name`（recv） |
| `openFramePoolByStream(type, name, opts)` | 高级：已知 send 通道 id；recv 由后缀推导 |
| `closeFramePool(type)` / `isFramePoolOpen(type)` | |
| `setFrameCallback(type, cb)` | 推模式；**须在 open 前设置**；回调返回后自动 release |
| `acquireFrame(type, view, timeout_ms)` | 拉模式；超时 → `FrameTimeout` |
| `releaseFrame(type, view)` | 拉模式归还 |

`StreamChannelType::PodVisibleLight` ↔ `camera_type=="light"`；`PodInfrared` ↔ `"infrared"`；`Front` ↔ `"imx586"`（兼容旧 `"fisheye"`）；`FisheyeDepth` ↔ `"fisheye_depth"`（`one_way`，用 `stream_name`）。

同路推/拉互斥。示例：`05_pod_frame_callback`、`06_pod_frame_acquire`、`07_front_camera_frame_callback`、`08_front_camera_frame_acquire`。

**`FrameView` 元数据**（IPC v2，经 `frame_share` 时可用）：

| 字段 | 说明 |
|------|------|
| `frame_index` | Pipeline 帧序号（`frame_unit.index`）；经 `frame_share` 或 `shm-push` 注入 |
| `timestamp_ns` | UTC 墙钟纳秒（发送侧由 BOOTTIME 转换） |
| `sequence` | IPC 双通道往返配对序号（单向 shm-push 仅为发送侧序号） |

拉模式在 `open` 及每次 `acquireFrame` 前会 drain 积压旧帧，尽量返回最新一帧。

`FisheyeDepth`（示例 09/10）走单向 `shm-push`（与 mediasoup 相同 IPC 实现），meta 同样来自 pipeline `frame_unit_t`。

---

# 7. 配置说明

SDK 运行时以 `Config` 结构体为准， 一般不需要修改，已在设备中通应用信息对齐：

```json
{
  "id": "dds_internal",
  "enabled": true,
  "impl": "dds",
  "config": {
    "domain_id": 0,
    "local_only": true,
    "control_topic": "/iking/internal/control",
    "control_reply_topic": "/iking/internal/control_reply",
    "status_topic": "/iking/internal/status",
    "event_topic": "/iking/internal/event",
    "heartbeat_topic": "/iking/internal/heartbeat"
  }
}
```

| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| domain_id | int | 0 | 须与 SDK 一致 |
| local_only | bool | true | 须与 SDK 一致 |
| *_topic | string | 见上 | 可覆盖，但 App 与 SDK 必须一致 |

---


# 8. 错误码

| 错误码 | 描述 | 常见原因 | 解决方案 |
|--------|------|----------|----------|
| 0 (`Ok`) | 成功 | — | — |
| -1 (`Failed`) | 业务失败 | app 拒绝、自检失败等 | 读 `Result.error` / `msg` |
| 1001 | 未连接 | 未 `connect` 或已断开 | 先 connect；查 app 是否在跑 |
| 1002 | 发布失败 | Writer 未匹配、传输异常 | 查 topic、domain、`local_only`、QoS |
| 1003 | 超时 | app 未回、Reader 未匹配 | 加大超时；确认 `dds_internal` 日志 |
| 1004 | 参数非法 | JSON 解析失败等 | 检查入参 |

Wire 层 `response.status != 0` 时，SDK 侧一般为 `Failed`，文本在 `error`。

---

# 9. 示例程序

| 程序 | 位置 | 功能 |
|------|------|------|
| `01_connect` | `sdk/example/01_connect.cpp` | 连接 + 系统状态 |
| `02_observe_callbacks` | `sdk/example/02_observe_callbacks.cpp` | event/status/heartbeat |
| `03_flight_basic` | `sdk/example/03_flight_basic.cpp` | 交互飞控菜单 |
| `04_pod_basic` | `sdk/example/04_pod_basic.cpp` | 交互吊舱菜单 |
| `05_pod_frame_callback` | `sdk/example/05_pod_frame_callback.cpp` | 交互：吊舱可见光帧回调 + overlay |
| `06_pod_frame_acquire` | `sdk/example/06_pod_frame_acquire.cpp` | 交互：吊舱拉模式 + 可选存 JPG |
| `07_front_camera_frame_callback` | `sdk/example/07_front_camera_frame_callback.cpp` | 交互：前向 camera 帧回调 |
| `08_front_camera_frame_acquire` | `sdk/example/08_front_camera_frame_acquire.cpp` | 交互：前向 camera 拉模式 |
| `09_fisheye_depth_callback` | `sdk/example/09_fisheye_depth_callback.cpp` | 交互：鱼眼深度帧回调 |
| `10_fisheye_depth_acquire` | `sdk/example/10_fisheye_depth_acquire.cpp` | 交互：鱼眼深度拉模式 |

索引见 `sdk/example/README.md`。`03`–`10` 启动后 `h` 看菜单、`q` 退出。随超级构建编出 `bin/`；装 deb 后也可在 `share/example/` 重编。运行前确保 app 已起。

---


# 10. 故障排查

| 现象 | 可能原因 | 解决方案 |
|------|----------|----------|
| 无法 connect | app 未起 / Domain 不一致 | 起 app；对齐 Config |
| 有发送无 reply | QoS 不兼容、topic 名错 | 通APP对齐dds配置|
| 限流失败 | session 请求过猛 | 降频 |
| 取帧 open 失败 | worker 未起、stream 名错 | 需要排查数据生产测应用情况 |
| 进程退出崩溃 | FastDDS 静态析构等 | 先 `disconnect`/`close`,尽量不要直接关闭 |
| 链不上库 | 未 source env | `LD_LIBRARY_PATH` / pkg-config |

---

# 11. 版本历史（Release Notes）

| 版本 | 日期 | 内容 |
|------|------|------|
| V0.0.9.4 | 2026.07.17 | 文档初稿，对齐当前实现 |


---

