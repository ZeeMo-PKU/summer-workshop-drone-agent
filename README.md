# Summer Workshop Drone Agent

无人机比赛控制程序，运行在 Jetson Orin NX 上，通过 iKing Drone SDK 接收裁判事件、控制飞行、操作夹爪并获取相机画面。

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

稳定版 `src/match.cpp` 尚未实现：

- 题目识别与求解。
- 下摄图片中的 A/B/C 排列识别。
- 正确答案到物理位置的动态映射。

当前源码仍把目标硬编码为 `PhysicalAnswerZone::A`。下摄实时流已于 2026-08-05 恢复并通过 SDK 连续取帧验证，但“向下”预置位仍拍到天花板，需要继续校准吊舱方向、观察点和构图。

## 隔离优化版

`experiments/match_agent_flow_test/` 已在不修改稳定版的前提下实现并验证：

- 常驻等待裁判事件：`MATCH_STARTED` 从 B 区开始新比赛，续轮在 A/B 区间交替。
- 每轮识别区连续 3 秒悬停，并缓存乱序到达的题目事件。
- 前置照片通过 Qwen 识别语义答案。
- 下摄照片通过 Qwen 识别 `A/B/C` 随机物理布局。
- 语义答案与布局合成目标位置后投递，并自动返回、降落；落地后继续等待下一轮。
- 下摄后重新确认夹爪闭合，并修复本地裁判桥对“夹爪状态暂时未知”的位置跟踪，避免把投放误记在观察位。
- 投放后先低空回到启动点，再使用 `returnToAnyPosition` 以 1.57 米定点返航降落，避免默认返航高度爬升；比赛结束发生在 `TAKEOFF` 时会等待进入可返航模式并重试。
- 任务高度超过 2.50 米立即熔断；失败落地后锁定比赛，重复的 `MATCH_STARTED` 不会再次起飞。
- 返航命令受飞行所有权保护，常驻启动器持续检测其他控制器，避免两个程序同时操控。
- OpenRouter 与原 DashScope 兼容配置，Key 只从环境变量或权限 `600` 的文件读取。
- 仿真真值与 Qwen 结果对照、失败关闭、双相机按需开启及完整运行归档。

该实验版尚未合并到 `src/match.cpp`。真实比赛前仍需用能同时拍到真实题面和真实
`A/B/C` 标记的场景完成视觉验收。

## 目录

```text
src/match.cpp                 当前服务器源码基线
experiments/match_agent_flow_test/  隔离的 Qwen 双图识别闭环
experiments/match_agent_flow_test/patches/  学生练习裁判桥补丁
experiments/match_agent_flow_test/test-data/  隔离流程的时间戳测试归档
archive/teammate-code/        同学代码与测试资料的只读时间戳存档
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
