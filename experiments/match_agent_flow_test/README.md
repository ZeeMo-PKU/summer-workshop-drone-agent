# 隔离单轮仿真流程

该目录是 `/opt/iking/match_agent` 的隔离实验副本，不覆盖正式源码、二进制或抓图目录。

## 单轮行为

```text
MATCH_STARTED
  -> 起飞至 1.57 m
  -> B 触发区悬停
SCENE_TRIGGER_SUCCEEDED + question
  -> 前向拍照并识别 A/B/C
  -> 物理位 2 悬停、云台向下、下摄拍照
  -> A/B/C 固定映射到物理位 1/2/3 并投递
  -> 返回启动点
  -> returnToHome 并等待 STANDBY
```

任何当次拍照、识别或导航失败都会取消投递并返航。只有在启动器实时确认 `CFG_FLIGHTSIM=1` 后，缺失的实体夹爪或云台动作才允许降级为有日志的仿真动作；非仿真环境仍将其视为失败。程序只支持单轮，忽略 `NEXT_ROUND_STARTED`。

执行模式在地面通过 `getPodCapability` 要求前摄 `imx586` 和下摄 `light` 同时在线。帧池不再从启动保持到任务结束，而是在到达拍摄点后打开、保存当次照片并立即关闭，以降低双路媒体常驻造成的资源占用和发热。程序不会读取固定旧照片作为本轮输入。

## 构建

```bash
cd /opt/iking/match_agent_flow_test
./scripts/build.sh
```

## 运行

默认只读监听：

```bash
./scripts/run.sh --dry-run
```

仿真执行前必须确保没有其他比赛控制进程，飞机处于 `STANDBY`、未解锁、高度 0，并在权限为 `600` 的 `.secrets.env` 中设置：

```bash
export DASHSCOPE_API_KEY="..."
export IKING_SIMULATION_CONFIRMED="1"
```

然后显式运行：

```bash
./scripts/run.sh --execute
```

### Simulation-specific behavior

- Scene B is accepted only after telemetry remains inside the arrival
  tolerance for 3000 ms. Questions that arrive before
  `SCENE_TRIGGER_SUCCEEDED` are cached once and consumed after the trigger.
- Recognition uses only the current run's `scene_B.jpg` and makes at most two
  attempts. Invalid responses still fail closed.
- A hybrid setup may combine physical cameras with a virtual referee image. In
  that case, `IKING_ALLOW_SIM_ORACLE=1` permits a fallback to the explicit
  `[SIM_ORACLE expected=X]` marker, but only when
  `IKING_SIMULATION_CONFIRMED=1`. Never enable this fallback for real flight.
- If the NX cannot reach the vision endpoint directly, inject `HTTPS_PROXY`
  and `HTTP_PROXY` at launch. Do not store proxy credentials or API keys in
  tracked files.

启动器还会通过无人机本机 WebSocket 只读查询 `CFG_FLIGHTSIM`；只有实时返回 `integer=1` 才允许执行。环境变量本身不能绕过此检查。

若仿真飞机在控制器异常退出后仍悬停，可在再次确认仿真模式且没有其他控制器后运行独立的 `build/recovery_return`。它只调用一次 `returnToHome` 并等待 `STANDBY`。

## 云台校准门槛

```bash
./scripts/calibrate-gimbal.sh
```

必须人工检查 `center`、`down`、`pitch-negative`、`pitch-positive` 四张照片。只有至少一个姿态明确朝向地面，才可将对应名称写入 `IKING_GIMBAL_PRESET`，并将同一个名称单独写入服务器本地、被 Git 忽略的 `.gimbal-preset-verified`。启动器要求两者严格一致；否则执行测试保持阻塞。

每次运行的数据保存到 `runs/<时间戳>/`。泄露过的临时 API 密钥只用于本次隔离测试，测试结束后必须轮换。
