# 隔离基线与后验检查

隔离目录以复制当时的服务器文件为基线，原始哈希保存在 `baseline.sha256`：

- `/opt/iking/match_agent/match_recognize.cpp`: `ee1aa9cbf4cefa2e775e63fadd80842cbdadc603097b633557bbf354d53f54d2`
- `/opt/iking/match_agent/recognize_image.hpp`: `0b2d178fa09e60f2dd0b2cbe05d0b90f737ca7f0e3136c51dce29b5e5f0ce51c`
- `/opt/iking/match_agent/test_recognize.cpp`: `0dd4a76237e8ade3b0b6b68d07e41bfc885f679ee2bb2d83273fa6a4c1e0fddb`

隔离代码只写入 `/opt/iking/match_agent_flow_test`。2026-08-05 14:40:27 +08:00 后验检查原目录时发现并发变化：

- `match.cpp` 的修改时间为 14:16:51，SHA-256 为 `55fcb2873150c37f01d218d7e43927db1d2034860aca34024ec3f2d1c7f805c4`，与本轮开始记录的 `a63c0f0694e8b91c15d36fa6ff1a7c89fd504c6667894c5d77e1d01d5660b799` 不同。
- `match_recognize.cpp` 已不存在。
- `match` 二进制仍为 `ca7f666b4994127fd7c72cd9e0be0ec86630667b65969f37a8359224ea7ab996`。
- `recognize_image.hpp` 仍为 `0b2d178fa09e60f2dd0b2cbe05d0b90f737ca7f0e3136c51dce29b5e5f0ce51c`。

因此本实验保持最初的一次性基线快照，不追赶或混入 14:16 之后的版本。后验不一致属于原目录的并发变更，不能声明原目录在整个测试窗口内保持不变。
