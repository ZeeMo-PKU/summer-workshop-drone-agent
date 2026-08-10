# 隔离常驻多轮仿真流程

该目录是 `/opt/iking/match_agent` 的隔离实验副本，不覆盖正式源码、二进制或抓图目录。

## 常驻行为

```text
MATCH_STARTED
  -> 第 1 轮：起飞至相对起飞点 1.57 m，再前往相对高度 0.3 m 的 B 触发区执行识别、投递、返航、降落
NEXT_ROUND_STARTED
  -> 第 2 轮：重新起飞，前往 A 触发区并执行完整闭环
NEXT_ROUND_STARTED
  -> 第 3 轮：重新起飞，前往 B 触发区；后续继续 A/B 交替
每轮落地后
  -> 进程不退出，继续等待 NEXT_ROUND_STARTED 或新的 MATCH_STARTED
新的 MATCH_STARTED
  -> 重置为新比赛第 1 轮，从 B 触发区重新开始
```

每轮在识别区完成 3 秒稳定悬停后，依次执行前向拍照与语义答案识别、答题区下摄布局识别、目标位置投递和自动返航降落。投放后先在相对起飞点 0.3 米高度回到启动区，再调用 SDK 的 `returnToAnyPosition` 以当次捕获的启动点、0.3 米和相对场地航向完成定点返航降落，避免 `returnToHome` 的高默认返航剖面。任何当次拍照、识别、导航或安全包络检查失败都会取消投递并返航；确认落地后进入故障锁定，忽略重复的 `MATCH_STARTED`，直到收到 `MATCH_FINISHED` 或人工重启程序。`MATCH_FINISHED` 与安全线违规可抢占任意空中状态并返航；若事件发生在 `TAKEOFF`，程序会等待飞控进入可返航模式。正常定点返航只发送一次；若程序接管时已经卡在 `LANDING`，15 秒后补发一次，已发送过命令后至少等待 60 秒才允许重试，避免降落过程中的命令风暴。

便携场地模式在首次 `MATCH_STARTED` 的地面预检中捕获启动经纬度、相对高度和机头航向：机头方向定义为场地 `+X`，场地 `+Z` 指向左侧。所有比赛点继续使用原场地米制坐标，但在运行时换算为当前场地经纬度。硬安全包络为启动点水平半径 `20 m`、相对高度 `-0.10 m` 至 `20 m`；触发区、答题观察点、三个投球区和返回启动点统一使用相对当次起飞点 `0.3 m`。目标发送前和导航过程中都会复查包络，越界立即中止当前任务并返航。续轮起飞前还要求飞机回到已捕获启动点 `1 m` 以内，防止落地后误换锚点。

部署到新场地时，必须先把未解锁飞机放在启动点，并让机头朝向从启动区指向触发区的场地 `+X`；裁判监控端必须使用同一位置和航向完成标定。0.3 米属于贴地飞行，切换实飞前仍需重新核验地面平整度、旋翼净空、电量、RTK、遥测、飞行模式、控制进程和现场安全。

程序只会为自己成功发送过的起飞命令执行返航。若另一个控制器使飞机进入空中状态，隔离程序拒绝抢发返航并退出；启动器还会在常驻期间每秒检查旧版及测试控制器，发现竞争进程后立即终止隔离程序并返回冲突错误。

> 2026-08-08 验证状态：当前源码已在服务器使用 iKing SDK
> `0.0.9.8.orin` 完整编译，`flow_logic_test`、`mission_sequence_test` 和
> `qwen_vision_test` 共 3/3 通过。验证只编译程序并运行纯逻辑测试，没有启动任何
> 飞控、相机或返航程序。编译与单元测试不代表飞行验收；切换到实飞前仍必须完成
> 本页列出的实时预检。

只有在启动器实时确认 `CFG_FLIGHTSIM=1` 后，缺失的实体夹爪或云台动作才允许降级为有日志的仿真动作；非仿真环境仍将其视为失败。每次起飞前都会重新确认 `STANDBY`、最新遥测、未解锁、SDK 控制、速度接近 0 和两路相机能力，不只依赖进程启动时的首次预检。返航也只有在 `STANDBY` 与同一组地面遥测同时成立时才算完成。

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
export VISION_MODEL=qwen/qwen3-vl-235b-a22b-instruct
```

客户端仍兼容 `VISION_API_KEY` 和旧的 `DASHSCOPE_API_KEY`，但不得将实际值写入源码、
示例、运行日志或仓库。

然后显式常驻运行：

```bash
./scripts/run.sh --execute
```

`scripts/build.sh` 会在编译和三组逻辑测试通过后记录源码摘要。若源码在上次构建后
发生变化，`scripts/run.sh` 会拒绝使用旧二进制，并明确要求重新构建。

程序保持等待直到收到退出信号；使用 `Ctrl+C` 或 `SIGTERM` 停止时，如果飞机不在地面，退出保护会先请求返航。

### 手动轮次触发

除裁判系统的 `MATCH_STARTED` 和 `NEXT_ROUND_STARTED` 外，常驻程序还接受本机受控
信号。先正常启动 `scripts/run.sh --execute`，再从另一个终端执行：

```bash
./scripts/trigger-round.sh first  # 手动触发 MATCH_STARTED
./scripts/trigger-round.sh next   # 手动触发 NEXT_ROUND_STARTED
```

触发脚本只允许连接到唯一一个以 `--execute` 运行的隔离比赛进程。事件仍交给原状态机
处理：比赛进行中重复首轮、当前轮未落地就请求续轮都会被忽略，不会绕过预检直接调用
飞控。Windows CMD 对应命令为 `match-first` 和 `match-next`。

### Qwen 图片识别闭环

- A/B 触发区只有在到达容差内连续稳定 `3000 ms` 才算完成悬停。若题目事件早于
  `SCENE_TRIGGER_SUCCEEDED` 到达，状态机会缓存一次并在触发成功后消费。
- `scene_B.jpg` 发送给 Qwen，严格只接受单个 `A`、`B` 或 `C`；题目中的
  `SIM_ORACLE` 字段在组成模型提示词前会被删除，避免答案泄漏。
- `answer_layout.jpg` 独立发送给 Qwen，严格只接受 `A=1,B=2,C=3` 形式的
  三项全排列。程序将语义答案与布局合成最终物理位，不再使用固定映射。
- 下摄完成后会再次闭合夹爪，再飞往最终物理位。这既确认球仍被夹持，也避免仿真裁判在云台改变姿态后继续沿用观察位 2 作为投放位置。
- 真实模式下每张图片只发起一次最长 `15 s` 的识别请求；任何超时或非法输出都会
  阻止投递并安全返航，避免网络重试耗尽飞控的位置模式保持时间。
- 混合仿真可以显式设置 `IKING_ALLOW_SIM_ORACLE=1`，此时跳过外部视觉请求并使用
  平台真值，只验证控制链时序。该选项同时要求 `IKING_SIMULATION_CONFIRMED=1`，
  真实飞行不得启用，且该结果不能计为视觉识别通过。
- 每次发送新坐标前会重新查询飞控模式。只要不再是 `POSITION`，程序就拒绝发送
  目标命令并进入安全返航。若 SDK 首次拒绝 `setPosition`，仅在飞控仍为
  `POSITION` 且没有紧急事件时对同一目标重试一次；不会无上限重复发指令。
- 导航中若飞行状态遥测无效、退出 SDK 控制或超过 `3 s` 没有更新，程序立即中止
  本轮并进入返航，不再空等完整的 `60 s` 导航超时。
- 若 NX 无法直连视觉接口，可在启动时临时注入 `HTTPS_PROXY` 和 `HTTP_PROXY`；
  代理凭据和 API 密钥均不得写入受 Git 跟踪的文件。

Qwen 接收的是无人机实际相机帧，不是本地裁判网页中显示的虚拟题图。若前摄只拍到房间或人员、下摄只拍到天花板且没有 A/B/C 布局，模型必须返回 `INVALID`；这种场景只能用显式仿真真值验证飞行状态机，不能作为真实视觉识别通过。提示词会明确拒绝利用无关画面推断“数量为 0”。

无需飞行即可用已归档照片验证完整视觉组合：

```bash
./scripts/run-qwen-probe.sh scene_B.jpg answer_layout.jpg "题目及 A/B/C 选项"
```

启动器还会通过无人机本机 WebSocket 只读查询 `CFG_FLIGHTSIM`；只有实时返回 `integer=1` 才允许执行。环境变量本身不能绕过此检查。

若仿真飞机在控制器异常退出后仍悬停，可在再次确认仿真模式且没有其他控制器后运行独立的恢复工具。恢复工具不再内置任何旧场地经纬度，必须显式指定当前运行生成的锚点文件：

```bash
build/recovery_return --anchor runs/<本次运行目录>/portable_site_anchor.json
```

它会在 `TAKEOFF` 时等待，在 `POSITION/MISSION` 时向该锚点发出定点返航，并持续等待 `STANDBY`。首次命令失败时按 10 秒间隔限制重试；命令已接受后至少等待 60 秒才允许补发。

## 云台校准门槛

```bash
./scripts/calibrate-gimbal.sh
```

必须人工检查 `center`、`down`、`pitch-negative`、`pitch-positive` 四张照片。只有至少一个姿态明确朝向地面，才可将对应名称写入 `IKING_GIMBAL_PRESET`，并将同一个名称单独写入服务器本地、被 Git 忽略的 `.gimbal-preset-verified`。启动器要求两者严格一致；否则执行测试保持阻塞。

每个常驻进程的数据保存到 `runs/<时间戳>/`，每次飞行的照片再按序号分目录保存。泄露过的临时 API 密钥只用于本次隔离测试，测试结束后必须轮换。
