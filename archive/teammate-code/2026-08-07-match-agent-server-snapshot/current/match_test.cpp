/**
 * 最小裁判信号监听程序。
 *
 * --dry-run：只打印裁判消息。
 * 默认 --execute：MATCH_STARTED 时闭合夹手、起飞到 1.57 m、飞往 B 触发区；
 *            SCENE_TRIGGER_SUCCEEDED 时在题目区拍照，再飞往答题区观察、投球并返回；
 *            MATCH_FINISHED / SAFETY_LINE_VIOLATION 时返航。
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <signal.h>

#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "iking_drone_sdk.h"
#include "recognize_image.hpp"

using namespace std::chrono_literals;

namespace {

constexpr double kTakeoffHeight = 1.57;
constexpr int kRpcTimeoutMs = 5000;
constexpr const char* kRecognizePrompt =
    "Identify the question shown in the image. "
    "Output ONLY the letter of the correct answer choice (A, B, or C). "
    "No explanation.";

// 题目区使用前向相机；答题区使用吊舱可见光相机。
constexpr auto kSceneCameraChannel =
    iking::drone::StreamChannelType::Front;
constexpr auto kAnswerCameraChannel =
    iking::drone::StreamChannelType::PodVisibleLight;
constexpr int kFrameRgb = 0;
constexpr int kFrameBgr = 1;
constexpr int kFrameYuv420p = 3;
constexpr char kCaptureDirectory[] =
    "/opt/iking/match_agent/captures";

// 拍照旋转角度
constexpr double kMatchYaw = -90.0;

// 识别超时时间
constexpr int kRecognitionTimeoutMs = 30000;

// 当前学生练习场地标定：
// reference=(39.07721710205078, 119.71366882324219)，rotation=0°；

// B 触发区场地坐标为 (x=5.4 m, z=0 m)。
constexpr double kBLatitude = 39.07721710205078;
constexpr double kBLongitude = 119.71373131094664;
constexpr double kBAltitude = 1.57;
constexpr double kBArrivalToleranceMeters = 0.20;

// A 触发区场地坐标为 (x=5.40 m, z=-2.97 m)。
constexpr double kALongitude=119.71373131094664;
constexpr double kALatitude=39.077190422086844;
constexpr double kAAltitude = 1.57;
constexpr double kAArrivalToleranceMeters = 0.20;


constexpr double kAnswerArrivalToleranceMeters = 0.20;
// 物理答题区 1：(x=0, z=-3.49 m)。
constexpr double kZone1Longitude = 119.71366882324219;
constexpr double kZone1Latitude = 39.077185750847363;
constexpr double kZone1Altitude = 1.57;

// 物理答题区 2：(x=0, z=-2.97 m)。
constexpr double kZone2Longitude = 119.71366882324219;
constexpr double kZone2Latitude = 39.077190422086844;
constexpr double kZone2Altitude = 1.57;

// 物理答题区 3：(x=0, z=-2.45 m)。
constexpr double kZone3Longitude = 119.71366882324219;
constexpr double kZone3Latitude = 39.077195093326317;
constexpr double kZone3Altitude = 1.57;

// Zone2 与 Zone3 的中间观察点。
// 吊舱相机在这里读取 Zone2、Zone3 从左到右的两个字母；
// 没有出现在这两个位置中的第三个字母自动对应 Zone1。
constexpr double kZone23MidLongitude = 119.71366882324219;
constexpr double kZone23MidLatitude = 39.0771927577061;
constexpr double kZone23MidAltitude = 1.57;

// 识别物理答题区顺序的prompts
const std::string kRecognizeAnswerLayoutPrompt =
    "Read the large white A/B/C letters on the two dark panels "
    "from left to right. "
    "Output exactly two uppercase letters only.";

// 启动区：(x=0, z=0)。当前版本返回后保持 1.57 m 高度悬停。
constexpr double kStartLatitude = 39.07721710205078;
constexpr double kStartLongitude = 119.71366882324219;
constexpr double kStartAltitude = 1.57;
constexpr double kStartArrivalToleranceMeters = 0.20;

constexpr auto kArrivalStableDuration = 1s;
constexpr auto kNavigationTimeout = 60s;
constexpr auto kAnswerResultTimeout = 5s;
constexpr double kEarthRadiusMeters = 6378137.0;

enum class AnswerLabel {
    A,
    B,
    C,
};

enum class PhysicalAnswerZone {
    Zone1, // 离起点最远
    Zone2,
    Zone3, // 离起点最近
    Zone23Mid, // 23中点，便于拍照
};

struct RefereeEvent {
    std::string request_id;
    std::string type;
    std::string message;
};

struct PositionSnapshot {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    std::chrono::steady_clock::time_point updated_at{};
};

struct TelemetryStore {
    std::mutex mutex;
    PositionSnapshot position;
    bool valid = false;
};

// SDK 回调线程只负责接收消息并入队；
// 飞行、拍照和识图等阻塞操作统一由 eventLoop 工作线程串行执行。
// 紧急事件插入队首，并通过 g_urgent_return 抢占当前导航等待。
class EventQueue {
public:
    void push(RefereeEvent event, bool urgent) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        if (urgent) events_.push_front(std::move(event));
        else events_.push_back(std::move(event));
        cv_.notify_one();
    }

    bool pop(RefereeEvent& event) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopped_ || !events_.empty(); });
        if (stopped_) return false;
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        events_.clear();
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<RefereeEvent> events_;
    bool stopped_ = false;
};

std::atomic<bool> g_stop{false};

volatile sig_atomic_t g_last_signal = 0;
volatile sig_atomic_t g_signal_sender_pid = 0;
volatile sig_atomic_t g_signal_sender_uid = 0;
volatile sig_atomic_t g_signal_code = 0;

std::atomic<bool> g_urgent_return{false};
std::atomic<bool> g_answer_result_received{false};
std::mutex g_print_mutex;
EventQueue g_events;
TelemetryStore g_telemetry;

void print(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << text << std::endl;
}

bool capturePhoto(iking::drone::Client& client,
                  iking::drone::StreamChannelType channel,
                  const std::string& path,
                  const char* camera_name,
                  bool rotate_ccw = false) {
    using iking::drone::StatusCode;

    std::error_code error;
    std::filesystem::create_directories(kCaptureDirectory, error);
    if (error) {
        print("[capture] cannot create directory: " + error.message());
        return false;
    }

    iking::drone::FrameView frame;
    const StatusCode acquire_status =
        client.acquireFrame(channel, frame, 2000);

    if (acquire_status != StatusCode::Ok) {
        print(std::string("[capture] ") + camera_name +
              " acquireFrame failed, status=" +
              std::to_string(static_cast<int>(acquire_status)));
        return false;
    }

    bool saved = false;

    if (frame.data && frame.width > 0 && frame.height > 0) {
        cv::Mat bgr;

        if (frame.type == kFrameBgr) {
            bgr = cv::Mat(
                frame.height,
                frame.width,
                CV_8UC3,
                frame.data);
        } else if (frame.type == kFrameRgb) {
            const cv::Mat rgb(
                frame.height,
                frame.width,
                CV_8UC3,
                frame.data);

            cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        } else if (frame.type == kFrameYuv420p) {
            const cv::Mat yuv(
                frame.height + frame.height / 2,
                frame.width,
                CV_8UC1,
                frame.data);

            cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
        } else {
            print("[capture] unsupported frame type=" +
                  std::to_string(frame.type));
        }

        if (!bgr.empty()) {
            if (rotate_ccw) {
                cv::Mat rotated;
                cv::rotate(bgr, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);
                bgr = rotated;

                print(std::string("[capture] ") + camera_name +
                    " rotated 90 degrees counterclockwise");
            }

            saved = cv::imwrite(path, bgr);

            print(saved
                    ? std::string("[capture] ") + camera_name +
                            " saved " + path
                    : std::string("[capture] ") + camera_name +
                            " cannot save " + path);
        }
    }

    const StatusCode release_status =
        client.releaseFrame(channel, frame);

    if (release_status != StatusCode::Ok) {
        print(std::string("[capture] ") + camera_name +
              " releaseFrame failed, status=" +
              std::to_string(static_cast<int>(release_status)));
    }

    return saved;
}

bool captureScenePhoto(iking::drone::Client& client,
                       const std::string& scene,
                       int round_index) {
    const std::string path =
        std::string(kCaptureDirectory) +
        "/round_" + std::to_string(round_index) +
        "_scene_" + scene + ".jpg";

    return capturePhoto(
        client, kSceneCameraChannel, path, "Front");
}

bool captureAnswerLayoutPhoto(iking::drone::Client& client,
                              int round_index) {
    const std::string path =
        std::string(kCaptureDirectory) +
        "/round_" + std::to_string(round_index) +
        "_answer_layout.jpg";

    return capturePhoto(
        client, kAnswerCameraChannel, path, "PodVisibleLight", true);
}

void onSignal(int signal, siginfo_t* info, void*) {
    g_last_signal = signal;

    if (info) {
        g_signal_sender_pid = info->si_pid;
        g_signal_sender_uid = info->si_uid;
        g_signal_code = info->si_code;
    }

    g_stop.store(true);
}

bool isOk(const iking::drone::Result& result, const char* action) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << "[sdk] " << action
              << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) std::cout << " error=" << result.error;
    std::cout << std::endl;
    return iking::drone::isOk(result.status);
}

const char* modeName(iking::drone::DRONE_MODE_STATUS_t mode) {
    switch (mode) {
        case iking::drone::STANDBY: return "STANDBY";
        case iking::drone::LANDING: return "LANDING";
        case iking::drone::POSITION: return "POSITION";
        case iking::drone::MISSION: return "MISSION";
        case iking::drone::TAKEOFF: return "TAKEOFF";
        default: return "UNKNOWN";
    }
}

bool isUrgent(const std::string& message) {
    return message == "MATCH_FINISHED" ||
           message == "SAFETY_LINE_VIOLATION";
}

bool parseTaskGuidance(const std::string& json, RefereeEvent& output) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json, root, false)) return false;

    const Json::Value& event = root["event"];
    if (!event.isObject() ||
        event.get("name", "").asString() != "taskGuidance") {
        return false;
    }

    const Json::Value& data = event["data"];
    if (!data.isObject()) return false;

    output.request_id = root.get("requestId", "").asString();
    output.type = data.get("type", "").asString();
    output.message = data.get("message", "").asString();
    return !output.message.empty();
}

void onEvent(const std::string& json, void*) {
    RefereeEvent event;
    if (!parseTaskGuidance(json, event)) return;

    const bool urgent = isUrgent(event.message);
    if (urgent) g_urgent_return.store(true);
    if (event.message == "ANSWER_CORRECT" ||
        event.message == "ANSWER_WRONG") {
        g_answer_result_received.store(true);
    }

    print("[taskGuidance] type=" + event.type +
          " message=" + event.message);
    g_events.push(std::move(event), urgent);
}

void onStatus(const std::string& json, void*) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json, root, false)) return;

    const Json::Value& position =
        root["status"]["data"]["flight"]["positionStatus"];
    if (!position.isObject() ||
        !position["latitude"].isNumeric() ||
        !position["longitude"].isNumeric() ||
        !position["altitude"].isNumeric()) {
        return;
    }

    PositionSnapshot snapshot;
    snapshot.latitude = position["latitude"].asDouble();
    snapshot.longitude = position["longitude"].asDouble();
    snapshot.altitude = position["altitude"].asDouble();
    snapshot.updated_at = std::chrono::steady_clock::now();

    if (!std::isfinite(snapshot.latitude) ||
        !std::isfinite(snapshot.longitude) ||
        !std::isfinite(snapshot.altitude)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_telemetry.mutex);
    g_telemetry.position = snapshot;
    g_telemetry.valid = true;
}

bool latestPosition(PositionSnapshot& output) {
    std::lock_guard<std::mutex> lock(g_telemetry.mutex);
    if (!g_telemetry.valid) return false;
    output = g_telemetry.position;
    return true;
}

double degreesToRadians(double value) {
    constexpr double kPi = 3.14159265358979323846;
    return value * kPi / 180.0;
}

double horizontalDistanceMeters(const PositionSnapshot& position,
                                double target_latitude,
                                double target_longitude) {
    const double mean_latitude =
        degreesToRadians((position.latitude + target_latitude) / 2.0);
    const double east =
        degreesToRadians(position.longitude - target_longitude) *
        kEarthRadiusMeters * std::cos(mean_latitude);
    const double north =
        degreesToRadians(position.latitude - target_latitude) *
        kEarthRadiusMeters;
    return std::hypot(east, north);
}

bool waitForMode(iking::drone::Client& client,
                 iking::drone::DRONE_MODE_STATUS_t target,
                 std::chrono::seconds timeout,
                 bool can_be_preempted) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int failed_queries = 0;
    int previous_mode = -1;

    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load()) return false;
        if (can_be_preempted && g_urgent_return.load()) return false;

        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto result = client.getModeStatus(mode, 2000);
        if (!iking::drone::isOk(result.status)) {
            if (++failed_queries >= 3) return false;
        } else {
            failed_queries = 0;
            if (static_cast<int>(mode) != previous_mode) {
                print(std::string("[mode] ") + modeName(mode));
                previous_mode = static_cast<int>(mode);
            }
            if (mode == target) return true;
        }

        std::this_thread::sleep_for(500ms);
    }

    return false;
}

bool takeoff(iking::drone::Client& client) {
    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!isOk(client.getModeStatus(mode, 2000), "getModeStatus") ||
        mode != iking::drone::STANDBY) {
        print("[match] takeoff cancelled: mode must be STANDBY");
        return false;
    }

    if (!isOk(client.gripperClose(kRpcTimeoutMs), "gripperClose")) {
        print("[match] takeoff cancelled: gripperClose failed");
        return false;
    }

    std::this_thread::sleep_for(1s);
    if (g_stop.load() || g_urgent_return.load()) return false;

    if (!isOk(client.takeOff(kTakeoffHeight, kRpcTimeoutMs), "takeOff(1.57m)")) {
        return false;
    }

    print("[match] takeoff accepted; waiting for POSITION");
    const bool reached = waitForMode(client, iking::drone::POSITION, 90s, true);
    print(reached ? "[match] takeoff completed" :
                    "[match] takeoff was not confirmed");
    return reached;
}

bool flyToAndHover(iking::drone::Client& client,
                   const char* target_name,
                   double longitude,
                   double latitude,
                   double altitude,
                   double yaw,
                   double arrival_tolerance) {
    print(std::string("[navigation] sending target: ") + target_name);
    if (!isOk(client.setPosition(longitude,
                                 latitude,
                                 altitude,
                                 yaw,
                                 kRpcTimeoutMs),
              "setPosition")) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + kNavigationTimeout;
    auto last_update = std::chrono::steady_clock::time_point{};
    auto stable_since = std::chrono::steady_clock::time_point{};
    auto last_log = std::chrono::steady_clock::time_point{};
    bool stable = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_urgent_return.load()) return false;

        PositionSnapshot position;
        if (!latestPosition(position) || position.updated_at == last_update) {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        last_update = position.updated_at;

        const double distance = horizontalDistanceMeters(
            position,
            latitude,
            longitude);

        if (last_log.time_since_epoch().count() == 0 ||
            position.updated_at - last_log >= 1s) {
            print(std::string("[navigation] target=") + target_name +
                  " distance=" + std::to_string(distance) +
                  "m altitude=" + std::to_string(position.altitude) + "m");
            last_log = position.updated_at;
        }

        if (distance <= arrival_tolerance) {
            if (!stable) {
                stable = true;
                stable_since = position.updated_at;
            } else if (position.updated_at - stable_since >=
                       kArrivalStableDuration) {
                print(std::string("[navigation] reached ") + target_name +
                      "; holding POSITION");
                return true;
            }
        } else {
            stable = false;
        }
    }

    print(std::string("[navigation] timed out waiting for ") + target_name);
    return false;
}

bool flyToAAndHover(iking::drone::Client& client) {
    return flyToAndHover(client,
                         "scene A",
                         kALongitude,
                         kALatitude,
                         kAAltitude,
                         kMatchYaw,
                         kAArrivalToleranceMeters);
}

bool flyToBAndHover(iking::drone::Client& client) {
    return flyToAndHover(client,
                         "scene B",
                         kBLongitude,
                         kBLatitude,
                         kBAltitude,
                         kMatchYaw,
                         kBArrivalToleranceMeters);
}

const char* answerZoneName(PhysicalAnswerZone zone) {
    switch (zone) {
        case PhysicalAnswerZone::Zone1: return "Zone 1";
        case PhysicalAnswerZone::Zone2: return "Zone 2";
        case PhysicalAnswerZone::Zone3: return "Zone 3";
        case PhysicalAnswerZone::Zone23Mid: return "Zone 23 Mid";
    }
    return "unknown answer zone";
}

// recognized_order[0]、recognized_order[1] 分别表示 Zone2、Zone3 上的字母。
// 若逻辑答案不在这两个位置中，则第三个未拍到的位置 Zone1 为目标区域。
PhysicalAnswerZone pickZoneFromAnswer(const std::string& text, const std::string& answer) {
    const char answer_letter = (answer.empty() ? 'A' : answer[0]);
    if (text[1] == answer_letter) return PhysicalAnswerZone::Zone2;
    if (text[0] == answer_letter) return PhysicalAnswerZone::Zone3;
    return PhysicalAnswerZone::Zone1;
}

// Build the recognition prompt from the referee question message.
// The message contains the question text plus A/B/C choices; the model
// must read the image and output ONLY the matching letter.
std::string buildRecognizePrompt(const std::string& question) {
    if (question.empty()) return std::string(kRecognizePrompt);
    return std::string(
        "A referee question with A/B/C choices is given below. "
        "Look at the image, determine the correct choice, "
        "and output ONLY the letter (A, B, or C) of the correct answer. "
        "No explanation. If you don't know the answer,return C.\n\nQuestion: ") + question;
}

bool flyToAnswerZone(iking::drone::Client& client,
                     PhysicalAnswerZone zone) {
    switch (zone) {
        case PhysicalAnswerZone::Zone1:
            return flyToAndHover(client,
                                 answerZoneName(zone),
                                 kZone1Longitude,
                                 kZone1Latitude,
                                 kZone1Altitude,
                                 kMatchYaw,
                                 kAnswerArrivalToleranceMeters);
        case PhysicalAnswerZone::Zone2:
            return flyToAndHover(client,
                                 answerZoneName(zone),
                                 kZone2Longitude,
                                 kZone2Latitude,
                                 kZone2Altitude,
                                 kMatchYaw,
                                 kAnswerArrivalToleranceMeters);
        case PhysicalAnswerZone::Zone3:
            return flyToAndHover(client,
                                 answerZoneName(zone),
                                 kZone3Longitude,
                                 kZone3Latitude,
                                 kZone3Altitude,
                                 kMatchYaw,
                                 kAnswerArrivalToleranceMeters);
        case PhysicalAnswerZone::Zone23Mid:
            return flyToAndHover(client,
                                 answerZoneName(zone),
                                 kZone23MidLongitude,
                                 kZone23MidLatitude,
                                 kZone23MidAltitude,
                                 kMatchYaw,
                                 kAnswerArrivalToleranceMeters);
    }
    return false;
}

bool waitForAnswerResult() {
    const auto deadline = std::chrono::steady_clock::now() + kAnswerResultTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_urgent_return.load()) return false;
        if (g_answer_result_received.load()) return true;
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

bool executeAnswerAndReturn(iking::drone::Client& client,
                            int round_index,
                            const std::string& answer) {
    print("[round] flying to zone 2 observation point");
    if (!flyToAnswerZone(client, PhysicalAnswerZone::Zone23Mid)) return false;

    if (g_stop.load() || g_urgent_return.load()) return false;

    std::this_thread::sleep_for(500ms);
    if (!captureAnswerLayoutPhoto(client, round_index)) {
        print("[round] answer layout capture failed; continuing with selected zone");
    }

    // ---- recognition: recognize the correct order of zones ----
    const std::string photo_zone_path =
        std::string(kCaptureDirectory) +
        // "/round_" + std::to_string(round_index) +
        // "_answer_layout.jpg";
        "/test_zone.jpg";
    PhysicalAnswerZone target_zone = PhysicalAnswerZone::Zone1;

    const std::string recognized_order = recognize::recognizeImage(
        photo_zone_path, kRecognizeAnswerLayoutPrompt, kRecognitionTimeoutMs);
    if (!recognized_order.empty()) {
        print("[round] recognized zone order from down to up: " + recognized_order);
        target_zone = pickZoneFromAnswer(recognized_order, answer);
        print(std::string("[round] selected zone: ") +
                answerZoneName(target_zone));
    } else {
        print("[round] recognition failed; using default zone A");
    }

    print(std::string("[round] flying to selected ") +
            answerZoneName(target_zone));
    if (!flyToAnswerZone(client, target_zone)) return false;

    if (g_stop.load() || g_urgent_return.load()) return false;

    g_answer_result_received.store(false);
    const bool opened = isOk(client.gripperOpen(kRpcTimeoutMs), "gripperOpen");
    if (opened) {
        print("[round] gripper open accepted; waiting for referee answer result");
        if (!waitForAnswerResult()) {
            print("[round] answer result was not received within 5 seconds");
        }
    } else {
        print("[round] gripperOpen failed; returning to start with the ball");
    }

    if (g_stop.load() || g_urgent_return.load()) return false;

    print("[round] returning to start zone");
    const bool returned = flyToAndHover(client,
                                        "start",
                                        kStartLongitude,
                                        kStartLatitude,
                                        kStartAltitude,
                                        kMatchYaw,
                                        kStartArrivalToleranceMeters);
    if (returned) {
        print("[round] start zone reached; waiting for next instruction");
    }
    return opened && returned;
}

bool returnHome(iking::drone::Client& client, const std::string& reason) {
    print("[return] reason=" + reason);

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    const auto mode_result = client.getModeStatus(mode, 2000);
    if (iking::drone::isOk(mode_result.status) &&
        mode == iking::drone::STANDBY) {
        g_urgent_return.store(false);
        print("[return] already STANDBY");
        return true;
    }

    if (!(iking::drone::isOk(mode_result.status) &&
          mode == iking::drone::LANDING)) {
        if (!isOk(client.returnToHome(kRpcTimeoutMs), "returnToHome")) {
            return false;
        }
    }

    g_urgent_return.store(false);
    const bool landed = waitForMode(
        client, iking::drone::STANDBY, 180s, false);
    print(landed ? "[return] landing confirmed" :
                   "[return] landing was not confirmed");
    return landed;
}

// 单轮流程：
// SCENE_TRIGGER_SUCCEEDED 解锁本轮答题流程；
// 随后的 question 事件提供题目文本，并触发拍照、识图、投球和返回启动区。
// NEXT_ROUND_STARTED 闭合夹手并在 A、B 题目区之间交替。
void eventLoop(iking::drone::Client& client, bool execute) {
    std::unordered_set<std::string> handled_ids;
    bool area_select = false; // 题目区初始false选B，true选A
    std::string current_scene = "B";
    int round_index = 1;
    bool match_started = false;
    bool match_finished = false;
    bool answer_flow_started = false;
    RefereeEvent event;

    while (g_events.pop(event)) {
        if (g_stop.load()) break;

        if (!event.request_id.empty() &&
            !handled_ids.insert(event.request_id).second) {
            print("[event] duplicate ignored: " + event.request_id);
            continue;
        }

        if (!execute) {
            print("[dry-run] would handle " + event.message);
            continue;
        }

        if (event.message == "MATCH_FINISHED" ||
            event.message == "SAFETY_LINE_VIOLATION") {
            match_finished = true;
            returnHome(client, event.message);
        } else if (event.message == "MATCH_STARTED") {
            if (match_started || match_finished) {
                print("[match] repeated MATCH_STARTED ignored");
                continue;
            }
            match_started = true;
            current_scene = "B";
            if (takeoff(client)) {
                flyToBAndHover(client);
            }
        } else if (event.message == "SCENE_TRIGGER_SUCCEEDED") {
            if (match_finished || answer_flow_started) {
                print("[round] SCENE_TRIGGER_SUCCEEDED ignored in current state");
                continue;
            }
            answer_flow_started = true;   // scene triggered; question follows right after
            print("[round] scene triggered; waiting for question");
        }  else if (event.message == "NEXT_ROUND_STARTED") {

            ++round_index;
            area_select = !area_select;
            client.gripperClose(kRpcTimeoutMs);
            if (area_select) {
                current_scene = "A";
                flyToAAndHover(client);
            }
            else {
                current_scene = "B";
                flyToBAndHover(client);
            }
        } else if (event.type == "question") {
            if (match_finished || !answer_flow_started) {
                print("[round] question ignored in current state");
                continue;
            }
            print("[match] question received: " + event.message);
            std::this_thread::sleep_for(500ms);
            if (!captureScenePhoto(client, current_scene, round_index)) {
                print("[round] photo capture failed; continuing task");
            }

            // ---- recognition: use the question message as the prompt ----
            const std::string photo_path =
                std::string(kCaptureDirectory) + 
                // "/round_" +
                // std::to_string(round_index) + "_scene_" +
                // current_scene + ".jpg";
                "/test.jpg";
            const std::string recognized = recognize::recognizeImage(
                photo_path, buildRecognizePrompt("图中车辆的车牌号码是什么？ A.甘AVR395 B.甘ASR397 C.甘ASR395"), kRecognitionTimeoutMs);
            if (!recognized.empty()) {
                print("[round] recognized answer: " + recognized);
            } else {
                print("[round] recognition failed; using default zone A");
            }
            executeAnswerAndReturn(client, round_index, recognized);
            answer_flow_started = false;
        } else {
            print("[match] no action for " + event.message);
        }
    }
}

// 程序收到 Ctrl+C 或 SIGTERM 后的安全兜底：
// 仅在无人机仍处于空中模式时请求返航。
void requestReturnOnExit(iking::drone::Client& client, bool execute) {
    if (!execute) return;

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    const auto result = client.getModeStatus(mode, 2000);
    if (!iking::drone::isOk(result.status)) return;

    if (mode == iking::drone::TAKEOFF ||
        mode == iking::drone::POSITION ||
        mode == iking::drone::MISSION) {
        isOk(client.returnToHome(kRpcTimeoutMs), "returnToHome(on exit)");
    }
}

void printUsage(const char* program) {
    std::cout
        << "usage: " << program << " [--dry-run | --execute]\n"
        << "  --dry-run  only listen and print (default)\n"
        << "  --execute  enable B-scene, default answer-B drop, and return\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool execute = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") execute = true;
        else if (arg == "--dry-run") execute = false;
        else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    struct sigaction action {};
    action.sa_sigaction = onSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;

    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    print(execute
              ? "[startup] WARNING: --execute enabled; flight commands may run"
              : "[startup] dry-run mode; no flight command will be sent");

    iking::drone::Config config;
    config.client_id = "summer_workshop_match_agent";
    iking::drone::Client client(config);

    if (!client.connect()) {
        std::cerr << "[startup] SDK connect failed" << std::endl;
        return 1;
    }

    const bool scene_camera_ready = isOk(
        client.openFramePool(kSceneCameraChannel),
        "openFramePool(Front)");
    const bool answer_camera_ready = isOk(
        client.openFramePool(kAnswerCameraChannel),
        "openFramePool(PodVisibleLight)");

    if (!scene_camera_ready) {
        print("[startup] Front unavailable; scene captures will fail");
    }
    if (!answer_camera_ready) {
        print("[startup] PodVisibleLight unavailable; answer layout captures will fail");
    }

    client.onStatus(onStatus);
    client.onEvent(onEvent);
    print("[startup] SDK connected; waiting for taskGuidance");

    std::thread worker(eventLoop, std::ref(client), execute);
    while (!g_stop.load()) std::this_thread::sleep_for(200ms);

    if (g_last_signal != 0) {
        const char* signal_name =
            g_last_signal == SIGINT  ? "SIGINT" :
            g_last_signal == SIGTERM ? "SIGTERM" :
                                    "UNKNOWN";

        print("[shutdown] received signal=" +
            std::to_string(g_last_signal) +
            " (" + signal_name + ")" +
            " sender_pid=" +
            std::to_string(g_signal_sender_pid) +
            " sender_uid=" +
            std::to_string(g_signal_sender_uid) +
            " si_code=" +
            std::to_string(g_signal_code));
    }

    g_events.stop();
    worker.join();
    requestReturnOnExit(client, execute);
    if (scene_camera_ready) {
        client.closeFramePool(kSceneCameraChannel);
    }
    if (answer_camera_ready) {
        client.closeFramePool(kAnswerCameraChannel);
    }
    client.disconnect();
    print("[shutdown] disconnected");
    return 0;
}
