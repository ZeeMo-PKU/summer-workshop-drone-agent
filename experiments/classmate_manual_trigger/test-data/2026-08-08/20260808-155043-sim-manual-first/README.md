# 2026-08-08 电脑手动触发第一轮仿真测试

## 测试目的

验证同学程序的独立手动触发版能够在不依赖裁判系统 `MATCH_STARTED` 的情况下，由 Windows CMD 命令触发第一轮；本次只验证起飞、前往 B 触发区、稳定悬停和退出返航，不触发题目识别及投球。

## 测试前状态

- 服务器：`10.8.82.81`
- `CFG_FLIGHTSIM=1`，仿真模式
- 飞行状态：`STANDBY`
- 解锁状态：`false`
- 高度：`0 m`
- SDK 控制：已启用
- 定位：RTK 4，卫星 25 颗
- 其他控制程序：无
- 相机预检：`ov48/light`、`itl612/infrared`、`fishEye/imx586`，通过

## 执行结果

1. 程序成功打开 `Front` 和 `PodVisibleLight` 两路帧池，并进入等待状态。
2. 电脑发送 `SIGUSR1` 后，程序记录 `requested MATCH_STARTED`。
3. 夹爪闭合、`takeOff(1.57m)` 和飞往 B 区的 `setPosition` 均返回成功。
4. 飞机到达 B 区时，日志距离为 `0.000000 m`，高度约 `1.565 m`，遥测速度为 `0 m/s`。
5. 到达后持续观察超过 3 秒，飞机保持 `POSITION` 悬停。
6. 发送正常停止信号后，程序调用 `returnToHome(on exit)`，返回值为成功。
7. 最终复核：`STANDBY`、未解锁、高度 `0 m`、无遗留控制程序。

结论：本次“电脑手动启动第一轮”仿真冒烟测试通过。由于没有发送题目或场景事件，本次没有调用 Qwen、没有拍照，也没有投球；这不是完整比赛闭环验收。

## 版本哈希

- `match_manual.cpp`: `0ec829ea0db8c945e265cb2723055430468e0043377a1d03728f047dabf17ab0`
- `match_manual`: `cddfe366d5682a3de845fd76f813e007d124e9f0b7e133590718b7341cc0f44a`
- `camera_preflight.cpp`: `a8b677aa70bc1264576e163f3daa5c9462adb85c4d174f89103b4871d4e8bd82`
- `camera_preflight`: `0e515c2d3cd86751e59c871ca3e7fb1275a536e2a9095f1c63820d74b4211480`

原始输出见 `raw/operator.log` 和 `raw/operator.stderr.log`。日志首行中文受 Windows 重定向编码影响，其余 SDK 与导航记录可读，不影响判定。
