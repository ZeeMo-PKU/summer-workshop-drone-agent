# 隔离常驻多轮仿真流程

该目录是 `/opt/iking/match_agent` 的隔离实验副本，不覆盖正式源码、二进制或抓图目录。

## 常驻行为

```text
MATCH_STARTED
  -> 第 1 轮：起飞至 1.57 m，前往 B 触发区并执行识别、投递、返航、降落
NEXT_ROUND_STARTED
  -> 第 2 轮：重新起飞，前往 A 触发区并执行完整闭环
NEXT_ROUND_STARTED
  -> 第 3 轮：重新起飞，前往 B 触发区；后续继续 A/B 交替
每轮落地后
  -> 进程不退出，继续等待 NEXT_ROUND_STARTED 或新的 MATCH_STARTED
新的 MATCH_STARTED
  -> 重置为新比赛第 1 轮，从 B 触发区重新开始
```

每轮在识别区完成 3 秒稳定悬停后，依次执行前向拍照与语义答案识别、答题区下摄布局识别、目标位置投递和自动返航降落。投放后直接调用 SDK 的 `returnToHome`，由飞控返回启动区并降落，不再先额外发送一次 `setPosition(start)`。任何当次拍照、识别或导航失败都会取消投递、返航并终止当前比赛，但进程会在确认落地后继续等待新的 `MATCH_STARTED`。`MATCH_FINISHED` 与安全线违规可抢占任意空中状态并返航；若事件发生在 `TAKEOFF`，程序会等待飞控进入可返航模式后发送并按间隔重试返航命令。

只有在启动器实时确认 `CFG_FLIGHTSIM=1` 后，缺失的实体夹爪或云台动作才允许降级为有日志的仿真动作；非仿真环境仍将其视为失败。每次起飞前都会重新确认 `STANDBY` 和两路相机能力，不只依赖进程启动时的首次预检。

执行模式在地面通过 `getPodCapability` 要求前摄 `imx586` 和下摄 `light` 同时在线。帧池不再从启动保持到任务结束，而是在到达拍摄点后打开、保存当次照片并立即关闭，以降低双路媒体常驻造成的资源占用和发热。程序不会读取固定旧照片作为本轮输入。每次飞行使用 `captures/flight-XXXX-round-XXXX-scene-X/` 独立目录，新的比赛或续轮不会覆盖旧照片。

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
export IKING_SIMULATION_CONFIRMED="1"
```

视觉提供方使用单独的、权限为 `600` 且被 Git 忽略的 `.vision.env`。OpenRouter
配置示例见 `.vision.env.example`；Key 本体存于同样被忽略的只读文件：

```bash
export VISION_API_KEY_FILE=/opt/iking/match_agent_flow_test/.openrouter-key
export VISION_API_URL=https://openrouter.ai/api/v1/chat/completions
export VISION_MODEL=qwen/qwen3-vl-30b-a3b-instruct
```

客户端仍兼容 `VISION_API_KEY` 和旧的 `DASHSCOPE_API_KEY`，但不得将实际值写入源码、
示例、运行日志或仓库。

然后显式常驻运行：

```bash
./scripts/run.sh --execute
```

程序保持等待直到收到退出信号；使用 `Ctrl+C` 或 `SIGTERM` 停止时，如果飞机不在地面，退出保护会先请求返航。

### Qwen 图片识别闭环

- A/B 触发区只有在到达容差内连续稳定 `3000 ms` 才算完成悬停。若题目事件早于
  `SCENE_TRIGGER_SUCCEEDED` 到达，状态机会缓存一次并在触发成功后消费。
- `scene_B.jpg` 发送给 Qwen，严格只接受单个 `A`、`B` 或 `C`；题目中的
  `SIM_ORACLE` 字段在组成模型提示词前会被删除，避免答案泄漏。
- `answer_layout.jpg` 独立发送给 Qwen，严格只接受 `A=1,B=2,C=3` 形式的
  三项全排列。程序将语义答案与布局合成最终物理位，不再使用固定映射。
- 下摄完成后会再次闭合夹爪，再飞往最终物理位。这既确认球仍被夹持，也避免仿真裁判在云台改变姿态后继续沿用观察位 2 作为投放位置。
- 两类识别最多重试两次；任何非法输出都会阻止投递并安全返航。
- 混合仿真可以显式设置 `IKING_ALLOW_SIM_ORACLE=1`，此时 Qwen 仍会实际调用，
  但程序会记录其与平台真值的差异并使用真值完成飞行闭环。该选项同时要求
  `IKING_SIMULATION_CONFIRMED=1`，真实飞行不得启用。
- 若 NX 无法直连视觉接口，可在启动时临时注入 `HTTPS_PROXY` 和 `HTTP_PROXY`；
  代理凭据和 API 密钥均不得写入受 Git 跟踪的文件。

Qwen 接收的是无人机实际相机帧，不是本地裁判网页中显示的虚拟题图。若前摄只拍到房间或人员、下摄只拍到天花板且没有 A/B/C 布局，模型必须返回 `INVALID`；这种场景只能用显式仿真真值验证飞行状态机，不能作为真实视觉识别通过。提示词会明确拒绝利用无关画面推断“数量为 0”。

无需飞行即可用已归档照片验证完整视觉组合：

```bash
./scripts/run-qwen-probe.sh scene_B.jpg answer_layout.jpg "题目及 A/B/C 选项"
```

启动器还会通过无人机本机 WebSocket 只读查询 `CFG_FLIGHTSIM`；只有实时返回 `integer=1` 才允许执行。环境变量本身不能绕过此检查。

若仿真飞机在控制器异常退出后仍悬停，可在再次确认仿真模式且没有其他控制器后运行独立的 `build/recovery_return`。它会在 `TAKEOFF` 时等待、在 `POSITION/MISSION` 时按间隔重试 `returnToHome`，并持续等待 `STANDBY`。

## 云台校准门槛

```bash
./scripts/calibrate-gimbal.sh
```

必须人工检查 `center`、`down`、`pitch-negative`、`pitch-positive` 四张照片。只有至少一个姿态明确朝向地面，才可将对应名称写入 `IKING_GIMBAL_PRESET`，并将同一个名称单独写入服务器本地、被 Git 忽略的 `.gimbal-preset-verified`。启动器要求两者严格一致；否则执行测试保持阻塞。

每个常驻进程的数据保存到 `runs/<时间戳>/`，每次飞行的照片再按序号分目录保存。泄露过的临时 API 密钥只用于本次隔离测试，测试结束后必须轮换。
