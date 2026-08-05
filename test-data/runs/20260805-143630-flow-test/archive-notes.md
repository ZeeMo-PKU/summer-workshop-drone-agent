# 归档说明

- 类型：`--execute` 云台标记门槛验证。
- 首次同步时，更新后的 `run.sh` 被误传到隔离目录根部，实际 `scripts/run.sh` 仍是旧版；程序因此连接 SDK 并等待裁判事件。
- 发现后立即发送 `Ctrl+C`，当次没有收到 `MATCH_STARTED`，飞机始终处于 `STANDBY`、`armed=false`、高度 `0 m`。
- `commands.jsonl` 只有 `getModeStatus(preflight)`、`openFramePool(Front)` 与 `openFramePool(PodVisibleLight)`，没有起飞、位置、返航、云台或夹爪命令。
- 随后将脚本同步到正确路径并删除误传文件；缺少 `.gimbal-preset-verified` 时，`--execute` 已在连接 SDK 前以返回码 `8` 拒绝。
- 原始服务器文件由 `SHA256SUMS` 校验；本地补充说明由 `ARCHIVE_SHA256SUMS` 一并校验。
