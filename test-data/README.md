# 测试数据归档

每次测试使用一个不可覆盖的时间戳目录：

```text
test-data/runs/YYYYMMDD-HHMMSS/
  captures/            本次测试新生成的相机图片
  metadata.txt         无人机状态、控制进程、源码和二进制哈希
  manifest.json        本地文件大小与 SHA-256
  run.json             测试时间和参数
  notes.md             测试目的与人工观察
  run.log              可选的程序标准输出日志
```

## 使用

测试结束后执行：

```powershell
.\tools\archive-test-run.ps1 `
  -StartedAt "2026-08-05 10:00:00" `
  -Notes "下摄构图测试"
```

默认行为：

1. 要求测试前的 Git 工作区干净，避免混入无关修改。
2. 通过 SSH 读取状态、控制进程和服务器文件哈希。
3. 只下载 `StartedAt` 之后生成的 JPEG/PNG。
4. 生成本地 SHA-256 清单。
5. 创建 `test-data: add run <RunId>` 提交并推送到当前分支。

使用 `-NoPush` 可以只归档，不自动提交和推送。使用 `-LogPath` 可以把本地保存的程序输出加入本次数据。

## 公开仓库要求

- 测试开始前清空拍摄区域，避免人员进入相机画面。
- 上传前检查照片中没有人脸、姓名、账号、二维码、密钥或其他隐私信息。
- 不保存 SSH 密码、GitHub 令牌、VPN 配置或系统账号文件。
- 若画面包含人员，本次原始数据只保留在本地，不推送到公开仓库。

2026-08-05 的已有下摄照片包含可识别人员，因此未加入公开仓库。

## 隔离闭环测试

`/opt/iking/match_agent_flow_test` 产生的完整运行目录使用专用脚本归档：

```powershell
.\tools\archive-flow-test-run.ps1 -RunId "20260805-140000"
```

脚本要求服务器清单存在，逐文件验证 SHA-256，并在提交前扫描凭据。使用 `-NoPush` 可只下载并校验。
