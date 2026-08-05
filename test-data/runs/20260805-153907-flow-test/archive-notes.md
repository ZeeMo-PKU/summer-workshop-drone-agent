# 测试说明

本次执行在 `MATCH_STARTED` 后调用 `gripperClose` 时返回 `gimbalControlAngleSetFailed`。程序在起飞前进入安全中止，确认已经是 `STANDBY` 后退出；没有发送起飞或导航命令，也没有生成照片。
