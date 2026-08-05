# 归档说明

- 类型：受控 10 秒 dry-run。
- 程序仅连接 SDK、订阅裁判事件和遥测；没有收到裁判事件。
- `commands.jsonl` 与 `events.jsonl` 均为空，没有飞控、云台或夹爪写命令。
- 外部超时通过 `SIGTERM` 正常转发，程序断开后没有残留控制进程。
- 原始服务器文件由 `SHA256SUMS` 校验；本地补充说明由 `ARCHIVE_SHA256SUMS` 一并校验。
