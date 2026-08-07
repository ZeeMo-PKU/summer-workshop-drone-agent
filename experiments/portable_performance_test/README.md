# 便携场地无人机性能测试

该程序不依赖固定场地经纬度。飞机放置在临时启动点后，以当时机头方向为前方，执行一个参数化正方形：起飞、悬停拍照、直飞一条边、原地顺时针转 90 度，重复四次，最后回到启动点并定点返航降落。

## 测量内容

- 起飞、每段直飞、每次转弯和返航耗时；
- 最大相对高度、最大水平/垂直速度和最大离场距离；
- 四边飞行后的闭环位置误差；
- 每个测点的前向照片和下摄照片；
- 全部飞控命令、遥测、参数、日志和 SHA-256 清单。

默认参数为 3 米高度、3 米边长、1 米/秒、每站悬停 2 秒、前向与下摄同时拍照。规划路线的最远点约为边长的 `sqrt(2)` 倍。

## 安全规则

- 默认 `--dry-run`，不会连接 SDK 或发送飞行命令。
- `--execute` 必须通过 `scripts/run.sh` 启动，并明确选择 `sim` 或 `real`。
- 启动器要求飞机处于 `STANDBY`、未解锁、高度 0、SDK 控制且没有其他控制器。
- 实飞还要求人工确认电量充足、现场净空，并要求 RTK 状态和卫星数量合格。
- 启动器读取 `RETURN_HEIGHT`；若返航高度超过本次场地限高，程序拒绝起飞。
- 高度上限不超过 10 米，水平半径不超过 20 米。任何运行中越界都会中止测试并返航。
- “任意场地”指具备有效定位、稳定悬停能力、满足净空和限高条件的场地，不代表可在禁飞区、人员上方或无定位环境飞行。

## 离线查看路线

```bash
./build/portable_performance_test --dry-run \
  --altitude 3 --leg 3 --speed 1 --hover 2 \
  --site-radius 10 --site-altitude-limit 10 --camera both
```

## 仿真执行

```bash
./scripts/run.sh --execute --environment sim --confirm-site-clear \
  --altitude 3 --leg 3 --speed 1 --hover 2 \
  --site-radius 10 --site-altitude-limit 10 --camera both
```

## 实飞执行

```bash
./scripts/run.sh --execute --environment real \
  --confirm-site-clear --confirm-battery-ready \
  --altitude 3 --leg 3 --speed 1 --hover 2 \
  --site-radius 10 --site-altitude-limit 10 --camera both
```

每次结果写入 `runs/<时间戳>/`。照片上传公开仓库前必须先检查人脸、手机屏幕、二维码和其他隐私内容。
