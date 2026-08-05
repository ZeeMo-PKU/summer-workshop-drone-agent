/**
 * 单轮裁判事件驱动的隔离仿真流程。
 *
 * 默认 --dry-run，只记录事件。--execute 必须同时由启动脚本完成仿真预检。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cctype>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "iking_drone_sdk.h"
#include "flow_logic.hpp"
#include "mission_sequence.hpp"
#include "recognize_image.hpp"

using namespace std::chrono_literals;

namespace {

constexpr float kTakeoffHeight = 1.57f;
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
constexpr char kDefaultRunRoot[] = "/opt/iking/match_agent_flow_test/runs";
constexpr char kBaselineMatchSha256[] =
    "ee1aa9cbf4cefa2e775e63fadd80842cbdadc603097b633557bbf354d53f54d2";
constexpr char kBaselineRecognizeSha256[] =
    "0b2d178fa09e60f2dd0b2cbe05d0b90f737ca7f0e3136c51dce29b5e5f0ce51c";

// 拍照旋转角度
constexpr float kMatchYaw = -90.0f;

// 当前学生练习场地标定：
// reference=(39.07721710205078, 119.71366882324219)，rotation=0°；

// B 触发区场地坐标为 (x=5.4 m, z=0 m)。
constexpr float kBLatitude = 39.07721710205078f;
constexpr float kBLongitude = 119.71373131094664f;
constexpr float kBAltitude = 1.57f;
constexpr double kBArrivalToleranceMeters = 0.20;

constexpr double kAnswerArrivalToleranceMeters = 0.15;
// 物理答题区 1：(x=0, z=-3.49 m)。
constexpr float kAnswerALongitude = 119.71366882324219f;
constexpr float kAnswerALatitude = 39.077185750847363f;
constexpr float kAnswerAAltitude = 1.57f;

// 物理答题区 2：(x=0, z=-2.97 m)。
constexpr float kAnswerBLongitude = 119.71366882324219f;
constexpr float kAnswerBLatitude = 39.077190422086844f;
constexpr float kAnswerBAltitude = 1.57f;

// 物理答题区 3：(x=0, z=-2.45 m)。
constexpr float kAnswerCLongitude = 119.71366882324219f;
constexpr float kAnswerCLatitude = 39.077195093326317f;
constexpr float kAnswerCAltitude = 1.57f;

// 启动区：(x=0, z=0)。返回到 1.57 m 后调用 returnToHome 降落。
constexpr float kStartLatitude = 39.07721710205078f;
constexpr float kStartLongitude = 119.71366882324219f;
constexpr float kStartAltitude = 1.57f;
constexpr double kStartArrivalToleranceMeters = 0.20;

constexpr auto kArrivalStableDuration = 1s;
constexpr auto kNavigationTimeout = 60s;
constexpr auto kAnswerResultTimeout = 5s;
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kAltitudeToleranceMeters = 0.20;

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
std::atomic<bool> g_urgent_return{false};
std::atomic<bool> g_answer_result_received{false};
std::mutex g_print_mutex;
std::mutex g_artifact_mutex;
EventQueue g_events;
TelemetryStore g_telemetry;
std::string g_run_directory;
std::string g_capture_directory;
std::ofstream g_run_log;
std::ofstream g_event_log;
std::ofstream g_telemetry_log;
std::ofstream g_command_log;

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

void appendJsonLine(std::ofstream& stream, Json::Value value) {
    std::lock_guard<std::mutex> lock(g_artifact_mutex);
    if (!stream) return;
    value["recorded_at"] = utcTimestamp();
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    stream << Json::writeString(builder, value) << '\n';
    stream.flush();
}

void print(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << text << std::endl;
    if (g_run_log) {
        g_run_log << utcTimestamp() << ' ' << text << '\n';
        g_run_log.flush();
    }
}

bool capturePhoto(iking::drone::Client& client,
                  iking::drone::StreamChannelType channel,
                  const std::string& path,
                  const char* camera_name) {
    using iking::drone::StatusCode;

    std::error_code error;
    std::filesystem::create_directories(g_capture_directory, error);
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

bool captureScenePhotoFromSdk(iking::drone::Client& client,
                              const std::string& scene) {
    const std::string path =
        g_capture_directory + "/scene_" + scene + ".jpg";

    return capturePhoto(
        client, kSceneCameraChannel, path, "Front");
}

bool captureAnswerLayoutPhotoFromSdk(iking::drone::Client& client) {
    const std::string path =
        g_capture_directory + "/answer_layout.jpg";

    return capturePhoto(
        client, kAnswerCameraChannel, path, "PodVisibleLight");
}

void onSignal(int) {
    g_stop.store(true);
}

bool isOk(const iking::drone::Result& result, const char* action) {
    Json::Value record;
    record["action"] = action;
    record["status"] = static_cast<int>(result.status);
    record["ok"] = iking::drone::isOk(result.status);
    if (!result.error.empty()) record["error"] = result.error;
    appendJsonLine(g_command_log, record);

    std::ostringstream line;
    line << "[sdk] " << action
         << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) line << " error=" << result.error;
    print(line.str());
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

    Json::Value record;
    record["request_id"] = event.request_id;
    record["type"] = event.type;
    record["message"] = event.message;
    appendJsonLine(g_event_log, record);

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

    Json::Value record;
    record["latitude"] = snapshot.latitude;
    record["longitude"] = snapshot.longitude;
    record["altitude"] = snapshot.altitude;
    appendJsonLine(g_telemetry_log, record);
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
        if (g_stop.load() && can_be_preempted) return false;
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

bool takeOffAfterGripperClosed(iking::drone::Client& client) {
    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!isOk(client.getModeStatus(mode, 2000), "getModeStatus") ||
        mode != iking::drone::STANDBY) {
        print("[match] takeoff cancelled: mode must be STANDBY");
        return false;
    }

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
                   float longitude,
                   float latitude,
                   float altitude,
                   float yaw,
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
            static_cast<double>(latitude),
            static_cast<double>(longitude));
        const double altitude_error =
            std::abs(position.altitude - static_cast<double>(altitude));

        if (last_log.time_since_epoch().count() == 0 ||
            position.updated_at - last_log >= 1s) {
            print(std::string("[navigation] target=") + target_name +
                  " distance=" + std::to_string(distance) +
                  "m altitude=" + std::to_string(position.altitude) +
                  "m altitude_error=" + std::to_string(altitude_error) + "m");
            last_log = position.updated_at;
        }

        if (distance <= arrival_tolerance &&
            altitude_error <= kAltitudeToleranceMeters) {
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

bool flyToBAndHover(iking::drone::Client& client) {
    return flyToAndHover(client,
                         "scene B",
                         kBLongitude,
                         kBLatitude,
                         kBAltitude,
                         kMatchYaw,
                         kBArrivalToleranceMeters);
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
        "No explanation. If the answer is uncertain, output INVALID.\n\nQuestion: ") +
        question;
}

bool flyToAnswerZone(iking::drone::Client& client,
                     flow::PhysicalZone zone) {
    switch (zone) {
        case flow::PhysicalZone::Zone1:
            return flyToAndHover(client,
                                 flow::zoneName(zone),
                                 kAnswerALongitude,
                                 kAnswerALatitude,
                                 kAnswerAAltitude,
                                 kMatchYaw,
                                 kAnswerArrivalToleranceMeters);
        case flow::PhysicalZone::Zone2:
            return flyToAndHover(client,
                                 flow::zoneName(zone),
                                 kAnswerBLongitude,
                                 kAnswerBLatitude,
                                 kAnswerBAltitude,
                                 kMatchYaw,
                                 kAnswerArrivalToleranceMeters);
        case flow::PhysicalZone::Zone3:
            return flyToAndHover(client,
                                 flow::zoneName(zone),
                                 kAnswerCLongitude,
                                 kAnswerCLatitude,
                                 kAnswerCAltitude,
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

bool setAnswerCameraPoseFromSdk(iking::drone::Client& client) {
    const char* configured = std::getenv("IKING_GIMBAL_PRESET");
    const std::string preset = configured ? configured : "down";
    if (preset == "down") {
        return isOk(client.gimbalDownSet(), "gimbalDownSet");
    }
    if (preset == "pitch-negative") {
        return isOk(client.gimbalControlAngleSet(0.0f, -90.0f, 0.0f),
                    "gimbalControlAngleSet(0,-90,0)");
    }
    if (preset == "pitch-positive") {
        return isOk(client.gimbalControlAngleSet(0.0f, 90.0f, 0.0f),
                    "gimbalControlAngleSet(0,90,0)");
    }
    print("[round] unsupported IKING_GIMBAL_PRESET=" + preset);
    return false;
}

bool returnHomeWithSdk(iking::drone::Client& client,
                       const std::string& reason) {
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

class SdkActions {
public:
    explicit SdkActions(iking::drone::Client& client) : client_(client) {}

    bool closeGripper() {
        if (!isOk(client_.gripperClose(kRpcTimeoutMs), "gripperClose")) {
            return false;
        }
        std::this_thread::sleep_for(1s);
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool takeOff() { return takeOffAfterGripperClosed(client_); }
    bool flyToSceneB() { return flyToBAndHover(client_); }

    bool captureScenePhoto() {
        std::this_thread::sleep_for(500ms);
        return !g_stop.load() && !g_urgent_return.load() &&
               captureScenePhotoFromSdk(client_, "B");
    }

    std::optional<flow::Answer> recognizeAnswer(
        const std::string& question) {
        const std::string photo_path = g_capture_directory + "/scene_B.jpg";
        const std::string recognized = recognize::recognizeImage(
            photo_path, buildRecognizePrompt(question), 30000);
        if (g_stop.load() || g_urgent_return.load()) return std::nullopt;
        return flow::parseAnswerToken(recognized);
    }

    bool flyToZone(flow::PhysicalZone zone) {
        print(std::string("[round] flying to ") + flow::zoneName(zone));
        return flyToAnswerZone(client_, zone);
    }

    bool setAnswerCameraPose() {
        if (!setAnswerCameraPoseFromSdk(client_)) return false;
        std::this_thread::sleep_for(2s);
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool captureAnswerLayoutPhoto() {
        const bool saved = captureAnswerLayoutPhotoFromSdk(client_);
        if (!saved) print("[round] answer layout capture failed; drop cancelled");
        return saved;
    }

    bool openGripperOnce() {
        if (gripper_open_attempted_) {
            print("[round] duplicate gripperOpen blocked");
            return false;
        }
        gripper_open_attempted_ = true;
        g_answer_result_received.store(false);
        if (!isOk(client_.gripperOpen(kRpcTimeoutMs), "gripperOpen")) {
            return false;
        }
        print("[round] gripper open accepted; waiting for referee answer result");
        if (!waitForAnswerResult()) {
            print("[round] answer result was not received within 5 seconds");
        }
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool returnToStart() {
        print("[round] returning to start zone");
        const bool returned = flyToAndHover(client_,
                                            "start",
                                            kStartLongitude,
                                            kStartLatitude,
                                            kStartAltitude,
                                            kMatchYaw,
                                            kStartArrivalToleranceMeters);
        if (returned) print("[round] start zone reached");
        return returned;
    }

    bool returnHome(const std::string& reason) {
        return returnHomeWithSdk(client_, reason);
    }

private:
    iking::drone::Client& client_;
    bool gripper_open_attempted_ = false;
};

flow::EventKind classifyEvent(const RefereeEvent& event) {
    if (event.message == "MATCH_STARTED") {
        return flow::EventKind::MatchStarted;
    }
    if (event.message == "SCENE_TRIGGER_SUCCEEDED") {
        return flow::EventKind::SceneTriggerSucceeded;
    }
    if (event.message == "MATCH_FINISHED") {
        return flow::EventKind::MatchFinished;
    }
    if (event.message == "SAFETY_LINE_VIOLATION") {
        return flow::EventKind::SafetyLineViolation;
    }
    if (event.message == "NEXT_ROUND_STARTED") {
        return flow::EventKind::NextRoundStarted;
    }
    if (event.type == "question") return flow::EventKind::Question;
    return flow::EventKind::Other;
}

void eventLoop(iking::drone::Client& client, bool execute) {
    std::unordered_set<std::string> handled_ids;
    flow::SingleRoundStateMachine state;
    SdkActions actions(client);
    flow::SingleRoundMission<SdkActions> mission(actions);
    RefereeEvent event;

    auto abort_and_return = [&](const std::string& reason) {
        state.markReturning();
        print("[abort] " + reason);
        const bool landed = mission.emergencyReturn(reason);
        if (landed) state.markAborted();
        else print("[abort] return-to-home was not confirmed");
        g_stop.store(true);
    };

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

        const flow::EventAction action = state.handle(classifyEvent(event));
        if (action == flow::EventAction::Ignore) {
            print("[event] ignored in current single-round state: " +
                  event.message);
            continue;
        }

        if (action == flow::EventAction::EmergencyReturn) {
            abort_and_return(event.message);
            break;
        }

        if (action == flow::EventAction::StartMission) {
            if (!mission.start()) {
                abort_and_return("takeoff or scene B navigation failed");
                break;
            }
            state.markSceneReady();
            print("[round] scene B reached; waiting for trigger");
            continue;
        }

        if (action == flow::EventAction::AwaitQuestion) {
            print("[round] scene triggered; waiting for question");
            continue;
        }

        if (action == flow::EventAction::ExecuteAnswer) {
            print("[match] question received: " + event.message);
            const auto target_zone = mission.observeQuestion(event.message);
            if (!target_zone) {
                abort_and_return("recognition failed or returned an invalid token");
                break;
            }
            if (g_urgent_return.load()) {
                abort_and_return("urgent return requested during recognition");
                break;
            }

            print(std::string("[round] recognized target=") +
                  flow::zoneName(*target_zone));

            if (!mission.deliverAndLand(*target_zone)) {
                abort_and_return("answer execution or return-to-start failed");
                break;
            }

            state.markCompleted();
            print("[round] single-round flow completed and landed");
            g_stop.store(true);
            break;
        }
    }
}

void requestReturnOnExit(iking::drone::Client& client, bool execute) {
    if (!execute) return;

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    const auto result = client.getModeStatus(mode, 2000);
    if (!iking::drone::isOk(result.status)) return;

    if (mode != iking::drone::STANDBY) {
        returnHomeWithSdk(client, "process exit");
    }
}

struct Options {
    bool execute = false;
    std::string run_id;
    std::string run_root = kDefaultRunRoot;
};

bool validRunId(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_';
    });
}

std::string defaultRunId() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S");
    return out.str();
}

bool initializeArtifacts(const Options& options) {
    g_run_directory = options.run_root + "/" + options.run_id;
    g_capture_directory = g_run_directory + "/captures";

    std::error_code error;
    if (std::filesystem::exists(g_run_directory)) {
        std::cerr << "[startup] run directory already exists: "
                  << g_run_directory << std::endl;
        return false;
    }
    std::filesystem::create_directories(g_capture_directory, error);
    if (error) {
        std::cerr << "[startup] cannot create run directory: "
                  << error.message() << std::endl;
        return false;
    }

    g_run_log.open(g_run_directory + "/run.log", std::ios::app);
    g_event_log.open(g_run_directory + "/events.jsonl", std::ios::app);
    g_telemetry_log.open(g_run_directory + "/telemetry.jsonl", std::ios::app);
    g_command_log.open(g_run_directory + "/commands.jsonl", std::ios::app);
    if (!g_run_log || !g_event_log || !g_telemetry_log || !g_command_log) {
        std::cerr << "[startup] cannot open run artifacts" << std::endl;
        return false;
    }

    Json::Value metadata;
    metadata["run_id"] = options.run_id;
    metadata["mode"] = options.execute ? "execute" : "dry-run";
    metadata["baseline_match_sha256"] = kBaselineMatchSha256;
    metadata["baseline_recognize_sha256"] = kBaselineRecognizeSha256;
    metadata["fixed_mapping"] = "A->zone1,B->zone2,C->zone3";
    metadata["started_at"] = utcTimestamp();
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::ofstream metadata_file(g_run_directory + "/metadata.json");
    metadata_file << Json::writeString(builder, metadata) << '\n';
    return static_cast<bool>(metadata_file);
}

void printUsage(const char* program) {
    std::cout
        << "usage: " << program
        << " [--dry-run | --execute] [--run-id ID] [--run-root PATH]\n"
        << "  --dry-run       listen and record only (default)\n"
        << "  --execute       enable the single-round simulation flow\n"
        << "  --run-id ID     unique artifact directory name\n"
        << "  --run-root PATH artifact root directory\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    options.run_id = defaultRunId();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") options.execute = true;
        else if (arg == "--dry-run") options.execute = false;
        else if (arg == "--run-id" && i + 1 < argc) {
            options.run_id = argv[++i];
        } else if (arg == "--run-root" && i + 1 < argc) {
            options.run_root = argv[++i];
        }
        else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }
    if (!validRunId(options.run_id) || options.run_root.empty()) {
        printUsage(argv[0]);
        return 2;
    }
    if (options.execute) {
        const char* simulation = std::getenv("IKING_SIMULATION_CONFIRMED");
        if (!simulation || std::string(simulation) != "1") {
            std::cerr << "[startup] --execute requires "
                         "IKING_SIMULATION_CONFIRMED=1" << std::endl;
            return 2;
        }
        const char* api_key = std::getenv("DASHSCOPE_API_KEY");
        if (!api_key || !*api_key) {
            std::cerr << "[startup] --execute requires DASHSCOPE_API_KEY"
                      << std::endl;
            return 2;
        }
    }
    if (!initializeArtifacts(options)) return 1;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    print(options.execute
              ? "[startup] WARNING: --execute enabled; flight commands may run"
              : "[startup] dry-run mode; no flight command will be sent");

    iking::drone::Config config;
    config.client_id = "summer_workshop_match_flow_test";
    iking::drone::Client client(config);

    if (!client.connect()) {
        std::cerr << "[startup] SDK connect failed" << std::endl;
        return 1;
    }

    client.onStatus(onStatus);
    client.onEvent(onEvent);

    iking::drone::DRONE_MODE_STATUS_t initial_mode = iking::drone::STANDBY;
    if (options.execute &&
        (!isOk(client.getModeStatus(initial_mode, 2000), "getModeStatus(preflight)") ||
         initial_mode != iking::drone::STANDBY)) {
        print("[startup] execute preflight requires STANDBY");
        client.disconnect();
        return 1;
    }

    const bool scene_camera_ready = !options.execute || isOk(
        client.openFramePool(kSceneCameraChannel), "openFramePool(Front)");
    const bool answer_camera_ready = !options.execute || isOk(
        client.openFramePool(kAnswerCameraChannel),
        "openFramePool(PodVisibleLight)");

    if (!scene_camera_ready) {
        print("[startup] Front unavailable; scene captures will fail");
    }
    if (!answer_camera_ready) {
        print("[startup] PodVisibleLight unavailable; answer layout captures will fail");
    }
    if (options.execute && (!scene_camera_ready || !answer_camera_ready)) {
        print("[startup] execute preflight failed: both cameras are required");
        if (scene_camera_ready) client.closeFramePool(kSceneCameraChannel);
        if (answer_camera_ready) client.closeFramePool(kAnswerCameraChannel);
        client.disconnect();
        return 1;
    }

    print("[startup] SDK connected; waiting for taskGuidance");

    std::thread worker(eventLoop, std::ref(client), options.execute);
    while (!g_stop.load()) std::this_thread::sleep_for(200ms);

    g_events.stop();
    worker.join();
    requestReturnOnExit(client, options.execute);
    if (options.execute && scene_camera_ready) {
        client.closeFramePool(kSceneCameraChannel);
    }
    if (options.execute && answer_camera_ready) {
        client.closeFramePool(kAnswerCameraChannel);
    }
    client.disconnect();
    print("[shutdown] disconnected");
    return 0;
}
