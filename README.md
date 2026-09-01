# 云圣智能实习项目：无人机操控与 AI 图像识别

本仓库记录云圣智能实习期间开发的无人机操控与 AI 图像识别项目。系统运行在 Jetson Orin NX 上，基于 iKing Drone SDK 接收任务事件、执行飞行和夹爪动作，并通过前视相机与吊舱可见光相机采集图像，为后续的题目识别、场景布局识别和自主决策提供输入。

## 项目目标

- 建立从任务事件到飞行动作的自动化控制流程。
- 完成双摄像头画面获取、测试数据归档和视觉管线接口设计。
- 使用 AI 图像识别任务题目与 A/B/C 空间布局。
- 将识别结果映射为无人机航点选择与投放动作。

## 当前状态

当前基线来自服务器 `/opt/iking/match_agent/match.cpp`，下载时 SHA-256：

```text
b73940bbc3ef83678e2f2d9219d2b930c54e53b15ae5d0a90be10cdb795790f1
```

已实现：

- `MATCH_STARTED` 后起飞并进入题目区。
- 题目区使用 `StreamChannelType::Front` 拍照。
- 答题区使用 `StreamChannelType::PodVisibleLight` 拍摄 A/B/C 布局。
- 支持三个物理答题位置的坐标导航。
- 先到中间观察点拍布局，再前往目标位置投球。
- 下一轮在题目区 A/B 之间切换。
- 比赛结束或安全线违规时返航。

尚未实现：

- 题目识别与求解。
- 下摄图片中的 A/B/C 排列识别。
- 正确答案到物理位置的动态映射。

当前源码仍把目标硬编码为 `PhysicalAnswerZone::A`。下摄实时流已于 2026-08-05 恢复并通过 SDK 连续取帧验证，但“向下”预置位仍拍到天花板，需要继续校准吊舱方向、观察点和构图。

## 目录

```text
src/match.cpp                 当前服务器源码基线
docs/current-state.md         已验证事实、问题和下一步
docs/reference/               比赛规则、场地坐标和原始 SDK 说明
docs/sdk/                     SDK Markdown 说明和示例索引
test-data/                    按时间戳归档的测试数据规范
tools/archive-test-run.ps1    测试结束后的拉取、提交和推送脚本
```

仓库是公开的。测试数据可以提交，但拍摄前必须清场，不能上传含可识别人员、账号、密钥或其他隐私信息的画面。

## 依赖

- iKing Drone SDK `V0.0.9.8.orin`
- OpenCV 4
- JsonCpp
- C++17 编译器

## 编译

在无人机服务器上执行：

```bash
export PKG_CONFIG_PATH=/opt/iking/drone_sdk/share/iking_drone_sdk:/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  src/match.cpp -o build/match \
  $(pkg-config --cflags --libs iking_drone_sdk opencv4) \
  -pthread -Wl,-rpath,/opt/iking/drone_sdk/lib
```

## 运行安全

当前基线存在一个已知问题：帮助文字说默认 `--dry-run`，但源码中的 `execute` 默认值实际为 `true`。在修复前，不要无参数运行。

只读测试必须显式使用：

```bash
./build/match --dry-run
```

只有在仿真环境、状态检查和场地确认完成后，才允许显式使用 `--execute`。

## 后续开发

视觉模块应与飞行控制解耦，计划新增：

```text
src/vision.hpp
src/vision.cpp
tests/vision_test.cpp
```

先使用离线照片完成题目识别、布局识别和纯逻辑映射测试，再接入自动投球流程。当前问题和推荐顺序见 [实现审计](docs/current-state.md)。

## 测试数据归档

每次测试结束后，在仓库根目录运行：

```powershell
.\tools\archive-test-run.ps1 -StartedAt "2026-08-05 10:00:00" -Notes "B 场景双摄测试"
```

脚本只拉取 `StartedAt` 之后生成的照片，并保存状态、服务器文件哈希和本地文件清单。默认自动创建 Git 提交并推送；使用 `-NoPush` 可只在本地归档。详细格式见 [测试数据说明](test-data/README.md)。
