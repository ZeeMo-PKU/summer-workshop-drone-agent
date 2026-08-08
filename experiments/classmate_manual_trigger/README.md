# 同学程序电脑手动触发版

## 当前状态

本目录已于 2026-08-08 部署到 `/opt/iking/match_agent_manual`，不会覆盖同学原目录
`/opt/iking/match_agent` 或原二进制 `./match`。

ARM64 编译和手动触发测试已经通过；`MATCH_STARTED` 与
`NEXT_ROUND_STARTED` 均能进入原事件队列。2026-08-08 的仿真冒烟测试已验证
“电脑触发第一轮 -> 起飞 -> B 区稳定悬停 -> 退出返航落地”，记录见 `test-data/`。
该测试没有发送题目事件，因此尚未验收识图、答题区拍照和投球。

## 与同学原版的关系

- 基线：`archive/teammate-code/2026-08-07-match-agent-server-snapshot/current/`。
- 原裁判事件入口保持不变。
- 新增 `SIGUSR1` 手动注入 `MATCH_STARTED`。
- 新增 `SIGUSR2` 手动注入 `NEXT_ROUND_STARTED`。
- 新版默认 `--dry-run`，只有显式 `--execute` 才允许发送控制命令。
- 使用独立 SDK `client_id`、图片目录、构建目录、锁和二进制名。
- `--execute` 启动时要求 Front 和 PodVisibleLight 两路相机均可用，否则拒绝执行。
- 启动脚本先运行 15 秒上限的 `camera_preflight`，能力不完整时不会进入主控制器。
- 相机预检前后都会检查其他控制器；运行期间每秒复查一次，发现冲突立即退出且不抢发返航命令。
- 识图密钥只从 `DASHSCOPE_API_KEY` 或服务器权限 `600` 的 `.secrets/dashscope_api_key` 读取，禁止写入源码和 Git。

信号处理器只设置 `sig_atomic_t` 标志。主线程每 200 ms 读取标志并将事件
放入原有 `EventQueue`，飞行操作仍由原来的单一工作线程串行执行。

## 手动控制边界

手动第一轮只替代裁判的 `MATCH_STARTED`；手动续轮只替代
`NEXT_ROUND_STARTED`。场景触发、题目内容和判题结果仍由裁判系统提供，
因此不会伪造题目或绕过原答题流程。

## 后续完整闭环验证

在仿真模式、飞机落地且没有其他控制器时，继续执行以下工作：

1. 由裁判系统发送场景触发和真实题目事件。
2. 验证题图拍摄、Qwen 输出和 A/B/C 严格解析。
3. 验证答题区下摄照片、唯一一次投球和返航落地。
4. 未经新的现场确认，不进行实飞。
