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

尚未实现：

- 题目识别与求解。
- 下摄图片中的 A/B/C 排列识别。
- 正确答案到物理位置的动态映射。

当前源码仍把目标硬编码为 `PhysicalAnswerZone::A`。下摄取帧已经工作，但测试照片没有完整覆盖 A/B/C，需要先修正吊舱朝向、观察点和构图。

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
