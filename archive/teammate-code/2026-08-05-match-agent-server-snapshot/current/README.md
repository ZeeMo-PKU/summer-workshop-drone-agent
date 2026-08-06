# match_recognize — 无人机比赛识别代理

在 `match.cpp` 基础上加入**大模型图片识别**的完整比赛流程：
裁判下发题目（含 A/B/C 选项）→ 无人机飞到题目场景拍照 → 大模型识别车牌等目标 → 选出正确答案区 → 投放并返回。

**原则：不修改原 `match.cpp`**，所有改动都在副本 `match_recognize.cpp` 和独立模块 `recognize_image.hpp` 里。

## 文件说明

| 文件 | 说明 |
|---|---|
| `match_recognize.cpp` | `match.cpp` 的副本，识别逻辑已集成 |
| `match_recognize` | 编译产物（可执行文件） |
| `recognize_image.hpp` | 识别模块（header-only）：图片压缩 → base64 → 调阿里云百炼 qwen-vl-max |
| `test_recognize.cpp` | 独立识别测试程序 |
| `rtunnel.py` | 本机运行的反向 SSH 隧道（机载 8088 ↔ 本机 8088） |
| `match.cpp` / `match` | 原文件，保持不动 |

## 编译命令

### 编译 match_recognize

```bash
cd /opt/iking/match_agent
g++ -std=c++17 -O2 \
  -I/opt/iking/drone_sdk/include \
  -I/usr/local/include/opencv4 \
  -I/usr/include/jsoncpp \
  match_recognize.cpp \
  -L/opt/iking/drone_sdk/lib -L/usr/local/lib \
  -liking_drone_sdk -ljsoncpp \
  $(pkg-config --libs opencv4) \
  -pthread -lstdc++fs \
  -o match_recognize
```

要点：
- `-I/usr/include/jsoncpp` **必须**——SDK 头文件里 `#include <json/json.h>`（JsonCpp）
- `-ljsoncpp` 必须显式链接
- `-std=c++17`（用了 `std::filesystem`）、`-pthread`、GCC 9 需 `-lstdc++fs`

### 编译 test_recognize（独立识别测试）

```bash
cd /opt/iking/match_agent
g++ -std=c++17 -O2 -I/usr/local/include/opencv4 \
  test_recognize.cpp $(pkg-config --libs opencv4) -o test_recognize
```

## 网络架构：机载无外网，经本机代理出网

机载（服务器）**没有外网**，识别走"本机当中继"：

```
机载 match_recognize → curl → 127.0.0.1:8088
   → 反向隧道(SSH, rtunnel.py, 自动重连)
   → 本机 proxy.py(127.0.0.1:8088)
   → 阿里云百炼 qwen-vl-max
   → 识别结果返回机载
```

### 本机（Windows）三步操作

```bash
# 1. 安装 proxy.py（已装过则跳过）
pip install proxy.py

# 2. 启动 HTTP 代理（终端 1）
python -m proxy --hostname 127.0.0.1 --port 8088

# 3. 启动反向隧道（终端 2，等价于 ssh -R 8088:localhost:8088）
python rtunnel.py
```

注意：
- 端口用 **8088**，因为 8080 被本机 node.exe 占用。
- 两个进程必须保持在线；本机重启后需重新启动。
- `rtunnel.py` 断线自动重连（3 秒后重试）。

### 机载运行（关键：必须带代理环境变量）

```bash
cd /opt/iking/match_agent
export HTTPS_PROXY=http://127.0.0.1:8088
./match_recognize
```

> 没设 `HTTPS_PROXY` 会报 `[recognize] no response (network/proxy? check HTTPS_PROXY)`——curl 直连阿里云失败。可用内联写法避免忘记：`HTTPS_PROXY=http://127.0.0.1:8088 ./match_recognize`。

### 连通性验证（链路通会返回 401，表示到了百炼只是没带 key）

```bash
# 本机测
curl -s -x http://127.0.0.1:8088 -o /dev/null -m 8 -w "%{http_code}\n" \
  https://ws-sxeumotzb6ouodsm.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions

# 机载测（走隧道）
curl -s -x http://127.0.0.1:8088 -o /dev/null -m 8 -w "%{http_code}\n" \
  https://ws-sxeumotzb6ouodsm.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions
```

返回 `401` = 通路 OK；`000` = 隧道/代理断了。

## 比赛运行流程

裁判事件时序（**question 紧跟在 SCENE_TRIGGER 之后**）：

```
[taskGuidance] type=guid    message=SCENE_TRIGGER_SUCCEEDED   → 置 answer_flow_started=true，等题目
[taskGuidance] type=question message=图中车辆的车牌号码是什么？ A. 皖AHF220 B. 皖CHF220 C. 皖AHJ220
                                                            → 拍照 → 识别 → 选答案区 → 飞行投放
```

`question` 分支执行：
1. `captureScenePhoto` 拍照存到 `captures/round_<n>_scene_<X>.jpg`
2. `recognizeImage(photo_path, buildRecognizePrompt(event.message), 30000)`
   ——把裁判题目文本拼进 prompt，要求模型看图片只输出答案字母 A/B/C
3. `pickZoneFromAnswer` 解析字母 → `PhysicalAnswerZone`（A/B/C）
4. `executeAnswerAndReturn(client, round_index, target_zone)` 飞行

识别失败（返回空）自动降级：默认飞 **A** 答案区，流程不卡死。

## 识别模块 recognize_image.hpp

- **HTTP**：用 `curl` CLI（popen），自动读取 `HTTPS_PROXY`/`HTTP_PROXY` 环境变量
- **payload**：nlohmann/json 拼装，写临时文件 `/tmp/recognize_<pid>.json`，`curl --data @file`（绕开 2MB ARG_MAX）
- **图片压缩**：OpenCV 缩到最长边 1280px + JPEG q85，失败回退原文件字节
- **API key**：环境变量 `DASHSCOPE_API_KEY` 优先，未设置回退到文件内常量
- **超时**：默认 60s（match 里用 30s），失败一律返回空字符串
- 依赖：nlohmann/json（/usr/include）、OpenCV、/usr/bin/curl

## 测试

```bash
cd /opt/iking/match_agent
# 用真实裁判题目格式测试识别
HTTPS_PROXY=http://127.0.0.1:8088 ./test_recognize captures/round_1_scene_B.jpg \
  '图中车辆的车牌号码是什么？ A. 皖AHF220 B. 皖CHF220 C. 皖AHJ220'
```

输出 `[test] answer=...`；空串说明识别失败（检查代理/隧道）。

## 排障

| 现象 | 原因 | 解决 |
|---|---|---|
| `[recognize] no response (network/proxy?...)` | `HTTPS_PROXY` 没设 或 隧道断了 | 设环境变量；重启 `proxy.py` + `rtunnel.py` |
| `[recognize] curl exit=...` | curl 直连失败/超时 | 检查隧道连通性（见上） |
| `[recognize] file not found` | 图片路径不对 | 检查 `captures/` 下的文件名 |
| 识别结果没有 A/B/C 字母 | 图片里没有目标/模型答非所问 | 检查真实场景图；`pickZoneFromAnswer` 会降级默认 A |
| `未定义的引用 Json::Value` | 编译漏了 `-ljsoncpp` | 补上链接参数 |
| `json/json.h 找不到` | 编译漏了 `-I/usr/include/jsoncpp` | 补上头文件路径 |

## 安全提醒

- API key 硬编码在 `recognize_image.hpp` 里，且已在对话中暴露，**建议用环境变量并轮换**：
  ```bash
  export DASHSCOPE_API_KEY=sk-ws-...
  ```
- 本机代理/隧道依赖：**比赛全程本机需在线**，识别流量过本机中转。
