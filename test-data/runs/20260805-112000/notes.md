# 下摄像头手动拍照测试 20260805-112000

## 结果

- 拍照前确认飞机处于 `STANDBY`、`armed=false`、高度 0，且没有比赛控制进程。
- `getPodCapability` 确认 `PodVisibleLight` 对应吊舱可见光相机 `ov48`。
- `gimbalDownSet` 返回 `status=Ok(0)`，吊舱已切换到向下姿态。
- 设备端成功生成两张 4032x3024 JPEG，并已保存到本目录的 `captures/`。
- 两张照片内容基本一致，画面严重虚焦；飞机位于地面时下摄镜头距离地面或遮挡物过近，当前照片不能用于识别 A/B/C 排列。

## 故障与恢复路径

1. `openFramePool(PodVisibleLight)` 可以成功打开，但 `acquireFrame` 持续返回 `FrameTimeout(1010)`。
2. 官方 SDK 示例 `06_pod_frame_acquire` 同样连续超时，排除了临时取帧程序的问题。
3. 系统日志显示 `ov48` 的 RTSP 管线约每 10 秒报错并自动重启。
4. `capturePhoto` 带本地路径时返回 `httpPathMissing`；不要求下载时返回 `Ok(0)`，但响应数据为空。
5. 最终从吊舱 HTTP 图片服务 `/image/mission/223886/image/` 找到固件固定命名的 `sdk_demo__0.jpg` 和 `sdk_demo__1.jpg`，并下载、重命名和校验哈希。

## 结论

下摄硬件可以进行高分辨率静态拍照，但实时可见光帧流当前故障。后续测试应在飞机升高或将镜头远离地面后重新拍摄，并修复 `ov48` RTSP 服务，才能用于比赛中的在线布局识别。
