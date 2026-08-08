/**
 * 常驻多轮裁判事件驱动的隔离仿真流程。
 *
 * 默认 --dry-run，只记录事件。--execute 必须同时由启动脚本完成仿真预检。
 * MATCH_STARTED 从 B 区开始新比赛；NEXT_ROUND_STARTED 在 A/B 区间交替。
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
#include <optional>
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
#include "portable_site.hpp"
#include "qwen_vision.hpp"
#include "recognize_image.hpp"

using namespace std::chrono_literals;

namespace {

constexpr float kMissionAltitude = 7.0f;
constexpr float kTakeoffHeight = kMissionAltitude;
constexpr int kRpcTimeoutMs = 5000;

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

// 便携场地坐标：起飞时机头方向为 +X，+Z 指向左侧。
constexpr double kMatchYawOffsetDegrees = 180.0;
constexpr double kPortableHorizontalRadiusMeters = 20.0;
constexpr double kPortableMaximumAltitudeMeters = 20.0;
constexpr double kPortableMinimumAltitudeMeters = -0.10;
constexpr double kAnchorReturnToleranceMeters = 1.0;

// B 触发区场地坐标为 (x=5.4 m, z=0 m)。
constexpr double kBFieldX = 5.4;
constexpr double kBFieldZ = 0.0;
constexpr double kBArrivalToleranceMeters = 0.20;

// A 触发区场地坐标为 (x=5.4 m, z=-2.97 m)。
constexpr double kAFieldX = 5.4;
constexpr double kAFieldZ = -2.97;
constexpr double kAArrivalToleranceMeters = 0.20;

constexpr double kAnswerArrivalToleranceMeters = 0.15;
// 物理答题区 1：(x=0, z=-3.49 m)。
constexpr double kAnswerAFieldX = 0.0;
constexpr double kAnswerAFieldZ = -3.49;

// 物理答题区 2：(x=0, z=-2.97 m)。
constexpr double kAnswerBFieldX = 0.0;
constexpr double kAnswerBFieldZ = -2.97;

// 物理答题区 3：(x=0, z=-2.45 m)。
constexpr double kAnswerCFieldX = 0.0;
constexpr double kAnswerCFieldZ = -2.45;

constexpr double kStartFieldX = 0.0;
constexpr double kStartFieldZ = 0.0;
constexpr double kStartArrivalToleranceMeters = 0.20;

constexpr auto kArrivalStableDuration = 1s;
constexpr auto kSceneStableDuration = 3s;
constexpr auto kNavigationTimeout = 60s;
constexpr auto kAnswerResultTimeout = 5s;
constexpr auto kTelemetryFreshnessLimit = 3s;
constexpr auto kGroundTelemetryWait = 5s;
constexpr double kAltitudeToleranceMeters = 0.20;
constexpr double kGroundAltitudeToleranceMeters = 0.10;

struct RefereeEvent {
    std::string request_id;
    std::string type;
    std::string message;
};

struct PositionSnapshot {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double heading_degrees = 0.0;
    bool heading_valid = false;
    bool armed = true;
    bool sdk_mode = false;
    bool flight_state_valid = false;
    std::string flight_path;
    std::chrono::steady_clock::time_point updated_at{};
};

enum class AnswerResult {
    Pending,
    Correct,
    Wrong,
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
std::atomic<bool> g_flight_commanded_by_process{false};
std::atomic<AnswerResult> g_answer_result{AnswerResult::Pending};
volatile std::sig_atomic_t g_manual_match_start_requested = 0;
volatile std::sig_atomic_t g_manual_next_round_requested = 0;
std::mutex g_print_mutex;
std::mutex g_artifact_mutex;
EventQueue g_events;
TelemetryStore g_telemetry;
std::optional<portable_site::SiteAnchor> g_site_anchor;
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

bool isOk(const iking::drone::Result& result, const char* action);

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

bool capturePhotoWithPool(iking::drone::Client& client,
                          iking::drone::StreamChannelType channel,
                          const std::string& path,
                          const char* camera_name) {
    const std::string open_action =
        std::string("openFramePool(") + camera_name + ")";
    if (!isOk(client.openFramePool(channel), open_action.c_str())) {
        return false;
    }

    const bool saved = capturePhoto(client, channel, path, camera_name);
    client.closeFramePool(channel);
    return saved;
}

bool captureScenePhotoFromSdk(iking::drone::Client& client,
                              const std::string& path) {
    return capturePhotoWithPool(
        client, kSceneCameraChannel, path, "Front");
}

bool captureAnswerLayoutPhotoFromSdk(iking::drone::Client& client,
                                     const std::string& path) {
    return capturePhotoWithPool(
        client, kAnswerCameraChannel, path, "PodVisibleLight");
}

bool hasCameraType(const Json::Value& capability,
                   const std::string& camera_type) {
    const Json::Value& channels = capability["streamChannels"];
    if (!channels.isArray()) return false;
    for (const auto& channel : channels) {
        if (channel.get("camera_type", "").asString() == camera_type) {
            return true;
        }
    }
    return false;
}

void onSignal(int signal_number) {
    if (signal_number == SIGUSR1) {
        g_manual_match_start_requested = 1;
        return;
    }
    if (signal_number == SIGUSR2) {
        g_manual_next_round_requested = 1;
        return;
    }
    g_stop.store(true);
}

void enqueueManualEvent(const char* message) {
    RefereeEvent event;
    event.type = "manual-control";
    event.message = message;

    Json::Value record;
    record["type"] = event.type;
    record["message"] = event.message;
    record["source"] = "SIGUSR";
    appendJsonLine(g_event_log, record);

    print(std::string("[manual-control] requested ") + message);
    g_events.push(std::move(event), false);
}

void pollManualEvents() {
    if (g_manual_match_start_requested != 0) {
        g_manual_match_start_requested = 0;
        enqueueManualEvent("MATCH_STARTED");
    }
    if (g_manual_next_round_requested != 0) {
        g_manual_next_round_requested = 0;
        enqueueManualEvent("NEXT_ROUND_STARTED");
    }
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

bool acceptSimulationPodFailure(bool success, const char* action) {
    if (success) return true;
    const char* confirmed = std::getenv("IKING_SIMULATION_CONFIRMED");
    if (!confirmed || std::string(confirmed) != "1") return false;
    print(std::string("[simulation] ") + action +
          " unavailable; continuing with virtual pod action");
    return true;
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

flow::ReturnMode returnModeFromSdk(iking::drone::DRONE_MODE_STATUS_t mode) {
    switch (mode) {
        case iking::drone::STANDBY: return flow::ReturnMode::Standby;
        case iking::drone::LANDING: return flow::ReturnMode::Landing;
        case iking::drone::POSITION: return flow::ReturnMode::Position;
        case iking::drone::MISSION: return flow::ReturnMode::Mission;
        case iking::drone::TAKEOFF: return flow::ReturnMode::Takeoff;
        default: return flow::ReturnMode::Unknown;
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
    if (event.message == "ANSWER_CORRECT") {
        g_answer_result.store(AnswerResult::Correct);
    } else if (event.message == "ANSWER_WRONG") {
        g_answer_result.store(AnswerResult::Wrong);
    }

    print("[taskGuidance] type=" + event.type +
          " message=" + event.message);
    g_events.push(std::move(event), urgent);
}

void onStatus(const std::string& json, void*) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json, root, false)) return;

    const Json::Value& data = root["status"]["data"];
    const Json::Value& flight = data["flight"];
    const Json::Value& position = flight["positionStatus"];
    const Json::Value& attitude = flight["attitude"];
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
    snapshot.heading_valid = attitude.isObject() &&
                             attitude["yaw"].isNumeric();
    if (snapshot.heading_valid) {
        snapshot.heading_degrees = attitude["yaw"].asDouble();
    }
    snapshot.flight_state_valid = flight["isArmed"].isBool() &&
                                  flight["sdkMode"].isBool();
    if (snapshot.flight_state_valid) {
        snapshot.armed = flight["isArmed"].asBool();
        snapshot.sdk_mode = flight["sdkMode"].asBool();
    }
    snapshot.flight_path = data.get("flightPath", "").asString();
    snapshot.updated_at = std::chrono::steady_clock::now();

    if (!std::isfinite(snapshot.latitude) ||
        !std::isfinite(snapshot.longitude) ||
        !std::isfinite(snapshot.altitude) ||
        (snapshot.heading_valid &&
         !std::isfinite(snapshot.heading_degrees))) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_telemetry.mutex);
    g_telemetry.position = snapshot;
    g_telemetry.valid = true;

    Json::Value record;
    record["latitude"] = snapshot.latitude;
    record["longitude"] = snapshot.longitude;
    record["altitude"] = snapshot.altitude;
    record["heading_degrees"] = snapshot.heading_degrees;
    record["heading_valid"] = snapshot.heading_valid;
    record["armed"] = snapshot.armed;
    record["sdk_mode"] = snapshot.sdk_mode;
    record["flight_state_valid"] = snapshot.flight_state_valid;
    record["flight_path"] = snapshot.flight_path;
    appendJsonLine(g_telemetry_log, record);
}

bool latestPosition(PositionSnapshot& output) {
    std::lock_guard<std::mutex> lock(g_telemetry.mutex);
    if (!g_telemetry.valid) return false;
    output = g_telemetry.position;
    return true;
}

flow::GroundTelemetryCheck groundTelemetryCheck(
    const PositionSnapshot& snapshot,
    bool valid,
    std::chrono::steady_clock::time_point now) {
    return flow::GroundTelemetryCheck{
        valid,
        valid && now - snapshot.updated_at <= kTelemetryFreshnessLimit,
        snapshot.flight_state_valid,
        snapshot.armed,
        snapshot.sdk_mode,
        snapshot.altitude,
    };
}

bool waitForGroundTelemetry(const char* context) {
    const auto deadline = std::chrono::steady_clock::now() +
                          kGroundTelemetryWait;
    PositionSnapshot latest;
    bool valid = false;
    while (std::chrono::steady_clock::now() < deadline) {
        valid = latestPosition(latest);
        const auto check = groundTelemetryCheck(
            latest, valid, std::chrono::steady_clock::now());
        if (flow::isGroundReady(check, kGroundAltitudeToleranceMeters)) {
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }

    const auto check = groundTelemetryCheck(
        latest, valid, std::chrono::steady_clock::now());
    print(std::string("[") + context +
          "] ground telemetry check failed: valid=" +
          (check.valid ? "true" : "false") + " fresh=" +
          (check.fresh ? "true" : "false") + " state_valid=" +
          (check.flight_state_valid ? "true" : "false") + " armed=" +
          (check.armed ? "true" : "false") + " sdk_mode=" +
          (check.sdk_mode ? "true" : "false") + " altitude=" +
          std::to_string(check.altitude) + " flight_path=" +
          latest.flight_path);
    return false;
}

bool captureOrValidatePortableSiteAnchor() {
    PositionSnapshot latest;
    if (!latestPosition(latest) ||
        std::chrono::steady_clock::now() - latest.updated_at >
            kTelemetryFreshnessLimit ||
        !latest.heading_valid) {
        print("[portable-site] fresh position and yaw telemetry are required");
        return false;
    }

    if (!g_site_anchor) {
        portable_site::SiteAnchor anchor{
            latest.latitude,
            latest.longitude,
            latest.altitude,
            portable_site::normalizeSignedDegrees(latest.heading_degrees),
        };
        if (!portable_site::isValidAnchor(anchor)) {
            print("[portable-site] invalid launch anchor telemetry");
            return false;
        }
        g_site_anchor = anchor;

        Json::Value artifact;
        artifact["latitude"] = anchor.latitude;
        artifact["longitude"] = anchor.longitude;
        artifact["altitude"] = anchor.altitude;
        artifact["heading_degrees"] = anchor.heading_degrees;
        artifact["horizontal_radius_meters"] =
            kPortableHorizontalRadiusMeters;
        artifact["maximum_relative_altitude_meters"] =
            kPortableMaximumAltitudeMeters;
        artifact["heading_contract"] =
            "aircraft nose at first round defines field +X";
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::ofstream file(g_run_directory + "/portable_site_anchor.json");
        file << Json::writeString(builder, artifact) << '\n';
        if (!file) {
            print("[portable-site] cannot save anchor artifact");
            g_site_anchor.reset();
            return false;
        }

        print("[portable-site] anchor captured: radius=20.0m max_altitude=20.0m "
              "heading=" + std::to_string(anchor.heading_degrees));
        return true;
    }

    const double distance = portable_site::horizontalDistanceMeters(
        *g_site_anchor, latest.latitude, latest.longitude);
    const double altitude_error =
        std::abs(latest.altitude - g_site_anchor->altitude);
    if (distance > kAnchorReturnToleranceMeters ||
        altitude_error > kGroundAltitudeToleranceMeters) {
        print("[portable-site] new round refused: aircraft is not at the "
              "captured launch anchor; distance=" +
              std::to_string(distance) + "m altitude_error=" +
              std::to_string(altitude_error) + "m");
        return false;
    }
    return true;
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

    if (!isOk(client.takeOff(kTakeoffHeight, kRpcTimeoutMs), "takeOff(7.0m)")) {
        return false;
    }
    g_flight_commanded_by_process.store(true);

    print("[match] takeoff accepted; waiting for POSITION");
    const bool reached = waitForMode(client, iking::drone::POSITION, 90s, true);
    print(reached ? "[match] takeoff completed" :
                    "[match] takeoff was not confirmed");
    return reached;
}

bool sendPositionWithBoundedRetry(iking::drone::Client& client,
                                  const char* target_name,
                                  float longitude,
                                  float latitude,
                                  float altitude,
                                  float yaw) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const std::string action =
            "setPosition(" + std::string(target_name) + ",attempt=" +
            std::to_string(attempt) + ")";
        if (isOk(client.setPosition(longitude,
                                    latitude,
                                    altitude,
                                    yaw,
                                    kRpcTimeoutMs),
                 action.c_str())) {
            return true;
        }

        const bool interrupted = g_stop.load() || g_urgent_return.load();
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        if (interrupted ||
            !isOk(client.getModeStatus(mode, 2000),
                  "getModeStatus(setPosition-retry)")) {
            return false;
        }
        if (!flow::shouldRetryPositionCommand(
                attempt, returnModeFromSdk(mode), interrupted)) {
            print(std::string("[navigation] setPosition retry refused for ") +
                  target_name + ": current mode=" + modeName(mode));
            return false;
        }

        print(std::string("[navigation] retrying target once: ") +
              target_name);
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

bool flyToAndHover(iking::drone::Client& client,
                   const char* target_name,
                   float longitude,
                   float latitude,
                   float altitude,
                   float yaw,
                   double arrival_tolerance,
                   std::chrono::steady_clock::duration stable_duration =
                       kArrivalStableDuration) {
    if (!g_site_anchor ||
        !portable_site::isPositionWithinEnvelope(
            *g_site_anchor,
            latitude,
            longitude,
            altitude,
            kPortableHorizontalRadiusMeters,
            kPortableMinimumAltitudeMeters,
            kPortableMaximumAltitudeMeters)) {
        print(std::string("[portable-site] refusing out-of-envelope target: ") +
              target_name);
        return false;
    }

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!isOk(client.getModeStatus(mode, 2000),
              "getModeStatus(navigation-preflight)")) {
        return false;
    }
    if (mode != iking::drone::POSITION) {
        print(std::string("[navigation] refusing target ") + target_name +
              ": expected POSITION, current mode=" + modeName(mode));
        return false;
    }

    print(std::string("[navigation] sending target: ") + target_name);
    if (!sendPositionWithBoundedRetry(client,
                                      target_name,
                                      longitude,
                                      latitude,
                                      altitude,
                                      yaw)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + kNavigationTimeout;
    auto last_update = std::chrono::steady_clock::time_point{};
    auto stable_since = std::chrono::steady_clock::time_point{};
    auto last_log = std::chrono::steady_clock::time_point{};
    bool stable = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_urgent_return.load()) return false;

        const auto now = std::chrono::steady_clock::now();
        PositionSnapshot position;
        if (!latestPosition(position)) {
            print(std::string("[navigation] telemetry unavailable for ") +
                  target_name);
            return false;
        }
        if (now - position.updated_at > kTelemetryFreshnessLimit) {
            print(std::string("[navigation] telemetry stale for ") +
                  target_name +
                  "; aborting without waiting for navigation timeout");
            return false;
        }
        if (!position.flight_state_valid || !position.sdk_mode) {
            print(std::string("[navigation] invalid flight control telemetry for ") +
                  target_name + ": state_valid=" +
                  (position.flight_state_valid ? "true" : "false") +
                  " sdk_mode=" + (position.sdk_mode ? "true" : "false"));
            return false;
        }
        if (position.updated_at == last_update) {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        last_update = position.updated_at;

        const portable_site::SiteAnchor target_anchor{
            static_cast<double>(latitude),
            static_cast<double>(longitude),
            0.0,
            0.0,
        };
        const double distance = portable_site::horizontalDistanceMeters(
            target_anchor, position.latitude, position.longitude);
        const double altitude_error =
            std::abs(position.altitude - static_cast<double>(altitude));

        if (!portable_site::isPositionWithinEnvelope(
                *g_site_anchor,
                position.latitude,
                position.longitude,
                position.altitude,
                kPortableHorizontalRadiusMeters,
                kPortableMinimumAltitudeMeters,
                kPortableMaximumAltitudeMeters)) {
            const double launch_distance =
                portable_site::horizontalDistanceMeters(
                    *g_site_anchor,
                    position.latitude,
                    position.longitude);
            const double relative_altitude =
                position.altitude - g_site_anchor->altitude;
            print(std::string("[portable-site] envelope violated while "
                              "navigating to ") + target_name +
                  ": launch_distance=" + std::to_string(launch_distance) +
                  "m relative_altitude=" +
                  std::to_string(relative_altitude) +
                  "m allowed_radius=20.0m allowed_altitude=[-0.1,20.0]m");
            g_urgent_return.store(true);
            return false;
        }

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
                       stable_duration) {
                print(std::string("[navigation] reached ") + target_name +
                      "; stable hover confirmed for " +
                      std::to_string(std::chrono::duration_cast<
                          std::chrono::milliseconds>(stable_duration).count()) +
                      "ms");
                return true;
            }
        } else {
            stable = false;
        }
    }

    print(std::string("[navigation] timed out waiting for ") + target_name);
    return false;
}

bool flyToFieldAndHover(
    iking::drone::Client& client,
    const char* target_name,
    double field_x,
    double field_z,
    double arrival_tolerance,
    std::chrono::steady_clock::duration stable_duration =
        kArrivalStableDuration) {
    if (!g_site_anchor) {
        print("[portable-site] target refused: launch anchor is unavailable");
        return false;
    }
    if (!portable_site::isLocalTargetWithinEnvelope(
            field_x,
            field_z,
            kMissionAltitude,
            kPortableHorizontalRadiusMeters,
            kPortableMaximumAltitudeMeters)) {
        print(std::string("[portable-site] local target outside envelope: ") +
              target_name);
        return false;
    }
    const auto target = portable_site::targetFromField(
        *g_site_anchor,
        field_x,
        field_z,
        kMissionAltitude,
        kMatchYawOffsetDegrees);
    return flyToAndHover(
        client,
        target_name,
        static_cast<float>(target.longitude),
        static_cast<float>(target.latitude),
        static_cast<float>(target.altitude),
        static_cast<float>(target.yaw_degrees),
        arrival_tolerance,
        stable_duration);
}

bool flyToBAndHover(iking::drone::Client& client) {
    return flyToFieldAndHover(client,
                              "scene B",
                              kBFieldX,
                              kBFieldZ,
                              kBArrivalToleranceMeters,
                              kSceneStableDuration);
}

bool flyToAAndHover(iking::drone::Client& client) {
    return flyToFieldAndHover(client,
                              "scene A",
                              kAFieldX,
                              kAFieldZ,
                              kAArrivalToleranceMeters,
                              kSceneStableDuration);
}

bool flyToAnswerZone(iking::drone::Client& client,
                     flow::PhysicalZone zone) {
    switch (zone) {
        case flow::PhysicalZone::Zone1:
            return flyToFieldAndHover(client,
                                      flow::zoneName(zone),
                                      kAnswerAFieldX,
                                      kAnswerAFieldZ,
                                      kAnswerArrivalToleranceMeters);
        case flow::PhysicalZone::Zone2:
            return flyToFieldAndHover(client,
                                      flow::zoneName(zone),
                                      kAnswerBFieldX,
                                      kAnswerBFieldZ,
                                      kAnswerArrivalToleranceMeters);
        case flow::PhysicalZone::Zone3:
            return flyToFieldAndHover(client,
                                      flow::zoneName(zone),
                                      kAnswerCFieldX,
                                      kAnswerCFieldZ,
                                      kAnswerArrivalToleranceMeters);
    }
    return false;
}

AnswerResult waitForAnswerResult() {
    const auto deadline = std::chrono::steady_clock::now() + kAnswerResultTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_urgent_return.load()) {
            return AnswerResult::Pending;
        }
        const AnswerResult result = g_answer_result.load();
        if (result != AnswerResult::Pending) return result;
        std::this_thread::sleep_for(100ms);
    }
    return AnswerResult::Pending;
}

bool setAnswerCameraPoseFromSdk(iking::drone::Client& client) {
    const char* configured = std::getenv("IKING_GIMBAL_PRESET");
    const std::string preset = configured ? configured : "down";
    if (preset == "down") {
        return acceptSimulationPodFailure(
            isOk(client.gimbalDownSet(), "gimbalDownSet"), "gimbalDownSet");
    }
    if (preset == "pitch-negative") {
        return acceptSimulationPodFailure(
            isOk(client.gimbalControlAngleSet(0.0f, -90.0f, 0.0f),
                 "gimbalControlAngleSet(0,-90,0)"),
            "gimbalControlAngleSet(0,-90,0)");
    }
    if (preset == "pitch-positive") {
        return acceptSimulationPodFailure(
            isOk(client.gimbalControlAngleSet(0.0f, 90.0f, 0.0f),
                 "gimbalControlAngleSet(0,90,0)"),
            "gimbalControlAngleSet(0,90,0)");
    }
    print("[round] unsupported IKING_GIMBAL_PRESET=" + preset);
    return false;
}

bool returnHomeWithSdk(iking::drone::Client& client,
                       const std::string& reason) {
    print("[return] reason=" + reason);

    if (!g_flight_commanded_by_process.load()) {
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto result = client.getModeStatus(mode, 2000);
        const bool ground_ready = iking::drone::isOk(result.status) &&
                                  mode == iking::drone::STANDBY &&
                                  waitForGroundTelemetry("return-unowned");
        const auto decision = flow::recoveryDecision(false, ground_ready);
        if (decision == flow::RecoveryDecision::AlreadyGrounded) {
            print("[return] no owned flight; aircraft is already grounded");
            return true;
        }
        print("[return] refusing to command an airborne aircraft because this "
              "process did not issue its takeoff");
        return false;
    }

    if (!g_site_anchor) {
        print("[return] launch anchor unavailable; refusing targeted return");
        return false;
    }
    const auto start_target = portable_site::targetFromField(
        *g_site_anchor,
        kStartFieldX,
        kStartFieldZ,
        kMissionAltitude,
        kMatchYawOffsetDegrees);

    constexpr auto kReturnTimeout = 180s;
    constexpr auto kReturnRetryDelay = 10s;
    const auto deadline = std::chrono::steady_clock::now() + kReturnTimeout;
    auto next_return_command = std::chrono::steady_clock::time_point{};
    auto landing_started_at = std::chrono::steady_clock::time_point{};
    auto last_return_command = std::chrono::steady_clock::time_point{};
    int failed_mode_queries = 0;
    int failed_return_commands = 0;
    int previous_mode = -1;
    bool return_command_accepted = false;
    bool reported_takeoff_wait = false;
    bool reported_unconfirmed_ground = false;

    while (std::chrono::steady_clock::now() < deadline) {
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto mode_result = client.getModeStatus(mode, 2000);
        if (!iking::drone::isOk(mode_result.status)) {
            if (++failed_mode_queries >= 3) {
                print("[return] three consecutive mode queries failed");
                return false;
            }
            std::this_thread::sleep_for(500ms);
            continue;
        }
        failed_mode_queries = 0;

        if (static_cast<int>(mode) != previous_mode) {
            print(std::string("[return] mode=") + modeName(mode));
            previous_mode = static_cast<int>(mode);
            if (mode == iking::drone::LANDING) {
                landing_started_at = std::chrono::steady_clock::now();
            }
        }

        const flow::ReturnAction action =
            flow::returnActionForMode(returnModeFromSdk(mode));
        if (action == flow::ReturnAction::Complete) {
            PositionSnapshot latest;
            const bool valid = latestPosition(latest);
            const auto check = groundTelemetryCheck(
                latest, valid, std::chrono::steady_clock::now());
            if (flow::isGroundReady(check,
                                    kGroundAltitudeToleranceMeters)) {
                g_urgent_return.store(false);
                g_flight_commanded_by_process.store(false);
                print("[return] landing confirmed by mode and telemetry");
                return true;
            }
            if (!reported_unconfirmed_ground) {
                print("[return] STANDBY reported but ground telemetry is not "
                      "yet confirmed");
                reported_unconfirmed_ground = true;
            }
        }

        if (mode == iking::drone::TAKEOFF && !reported_takeoff_wait) {
            print("[return] waiting for TAKEOFF to reach a return-capable mode");
            reported_takeoff_wait = true;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool landing_recovery_due =
            mode == iking::drone::LANDING &&
            landing_started_at.time_since_epoch().count() != 0 &&
            flow::shouldRecoverStalledLanding(
                return_command_accepted,
                std::chrono::duration<double>(now - landing_started_at).count(),
                last_return_command.time_since_epoch().count() == 0
                    ? 0.0
                    : std::chrono::duration<double>(
                          now - last_return_command).count());
        const double command_elapsed_seconds =
            last_return_command.time_since_epoch().count() == 0
                ? 0.0
                : std::chrono::duration<double>(
                      now - last_return_command).count();
        const bool position_return_due =
            now >= next_return_command &&
            flow::shouldIssuePositionReturn(
                action, return_command_accepted, command_elapsed_seconds);
        if (position_return_due ||
            (landing_recovery_due && now >= next_return_command)) {
            const bool accepted = isOk(client.returnToAnyPosition(
                                           static_cast<float>(
                                               start_target.longitude),
                                           static_cast<float>(
                                               start_target.latitude),
                                           static_cast<float>(
                                               start_target.altitude),
                                           static_cast<float>(
                                               start_target.yaw_degrees),
                                           kRpcTimeoutMs),
                                       "returnToAnyPosition(start,7.0m)");
            if (!accepted) {
                if (++failed_return_commands >= 3) {
                    print("[return] targeted return failed three times");
                    return false;
                }
            } else {
                failed_return_commands = 0;
                return_command_accepted = true;
            }
            last_return_command = now;
            next_return_command =
                now + (accepted ? 60s : kReturnRetryDelay);
        }

        std::this_thread::sleep_for(500ms);
    }

    print("[return] landing was not confirmed within 180 seconds");
    return false;
}

class SdkActions {
public:
    explicit SdkActions(iking::drone::Client& client)
        : client_(client),
          vision_(
              [](const std::string& image,
                 const std::string& prompt,
                 int timeout_ms) {
                  return recognize::recognizeImage(
                      image, prompt, timeout_ms);
              },
              [](const std::string& line) { print(line); },
              [] {
                  const char* simulation =
                      std::getenv("IKING_SIMULATION_CONFIRMED");
                  const char* allow_oracle =
                      std::getenv("IKING_ALLOW_SIM_ORACLE");
                  return simulation && std::string(simulation) == "1" &&
                         allow_oracle && std::string(allow_oracle) == "1";
              }()) {}

    bool beginRound(int round_index, flow::SceneZone scene) {
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        if (!isOk(client_.getModeStatus(mode, 2000),
                  "getModeStatus(round-preflight)") ||
            mode != iking::drone::STANDBY) {
            print("[round] preflight requires STANDBY");
            return false;
        }
        if (!waitForGroundTelemetry("round-preflight")) {
            print("[round] preflight requires fresh telemetry, armed=false, "
                  "sdk_mode=true, and altitude<=0.10m");
            return false;
        }
        if (!captureOrValidatePortableSiteAnchor()) {
            print("[round] portable-site anchor preflight failed");
            return false;
        }

        const auto capability = client_.getPodCapability();
        if (!isOk(capability, "getPodCapability(round-preflight)") ||
            !(hasCameraType(capability.msg, "imx586") ||
              hasCameraType(capability.msg, "fisheye")) ||
            !hasCameraType(capability.msg, "light")) {
            print("[round] preflight requires Front and PodVisibleLight");
            return false;
        }

        ++flight_sequence_;
        current_scene_ = scene;
        current_question_.clear();
        gripper_open_attempted_ = false;
        g_answer_result.store(AnswerResult::Pending);

        std::ostringstream directory_name;
        directory_name << "/flight-" << std::setw(4) << std::setfill('0')
                       << flight_sequence_ << "-round-" << std::setw(4)
                       << round_index << "-scene-" << flow::sceneName(scene);
        round_capture_directory_ =
            g_capture_directory + directory_name.str();
        std::error_code error;
        std::filesystem::create_directories(round_capture_directory_, error);
        if (error) {
            print("[round] cannot create capture directory: " +
                  error.message());
            return false;
        }
        scene_photo_path_ = round_capture_directory_ + "/scene_" +
                            flow::sceneName(scene) + ".jpg";
        answer_layout_photo_path_ =
            round_capture_directory_ + "/answer_layout.jpg";
        print("[round] starting flight sequence=" +
              std::to_string(flight_sequence_) + " round=" +
              std::to_string(round_index) + " scene=" +
              flow::sceneName(scene));
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool closeGripper() {
        if (!acceptSimulationPodFailure(
                isOk(client_.gripperClose(kRpcTimeoutMs), "gripperClose"),
                "gripperClose")) {
            return false;
        }
        std::this_thread::sleep_for(1s);
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool takeOff() { return takeOffAfterGripperClosed(client_); }
    bool flyToScene(flow::SceneZone scene) {
        return scene == flow::SceneZone::A
                   ? flyToAAndHover(client_)
                   : flyToBAndHover(client_);
    }

    bool captureScenePhoto() {
        std::this_thread::sleep_for(500ms);
        return !g_stop.load() && !g_urgent_return.load() &&
               captureScenePhotoFromSdk(client_, scene_photo_path_);
    }

    std::optional<flow::Answer> recognizeAnswer(
        const std::string& question) {
        current_question_ = question;
        const auto answer =
            vision_.recognizeSemanticAnswer(scene_photo_path_, question);
        if (g_stop.load() || g_urgent_return.load()) return std::nullopt;
        return answer;
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
        const bool saved = captureAnswerLayoutPhotoFromSdk(
            client_, answer_layout_photo_path_);
        if (!saved) print("[round] answer layout capture failed; drop cancelled");
        return saved;
    }

    std::optional<flow::AnswerLayout> recognizeAnswerLayout() {
        const auto layout =
            vision_.recognizeAnswerLayout(
                answer_layout_photo_path_, current_question_);
        if (g_stop.load() || g_urgent_return.load()) return std::nullopt;
        if (layout) {
            print("[round] resolved answer layout=" +
                  flow::answerLayoutName(*layout));
        }
        return layout;
    }

    bool restoreGripperClosedForDrop() {
        print("[round] restoring closed gripper state after gimbal capture");
        if (!acceptSimulationPodFailure(
                isOk(client_.gripperClose(kRpcTimeoutMs),
                     "gripperClose(drop)"),
                "gripperClose(drop)")) {
            return false;
        }
        std::this_thread::sleep_for(1s);
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool openGripperOnce() {
        if (gripper_open_attempted_) {
            print("[round] duplicate gripperOpen blocked");
            return false;
        }
        gripper_open_attempted_ = true;
        g_answer_result.store(AnswerResult::Pending);
        if (!acceptSimulationPodFailure(
                isOk(client_.gripperOpen(kRpcTimeoutMs), "gripperOpen"),
                "gripperOpen")) {
            return false;
        }
        print("[round] gripper open accepted; waiting for referee answer result");
        const AnswerResult result = waitForAnswerResult();
        if (result == AnswerResult::Pending) {
            print("[round] answer result was not received within 5 seconds");
            return false;
        }
        print(result == AnswerResult::Correct
                  ? "[round] referee result=CORRECT"
                  : "[round] referee result=WRONG");
        return !g_stop.load() && !g_urgent_return.load();
    }

    bool returnToStart() {
        print("[round] returning to the start zone before landing");
        return flyToFieldAndHover(client_,
                                  "start",
                                  kStartFieldX,
                                  kStartFieldZ,
                                  kStartArrivalToleranceMeters);
    }

    bool returnHome(const std::string& reason) {
        return returnHomeWithSdk(client_, reason);
    }

private:
    iking::drone::Client& client_;
    vision::QwenVision vision_;
    int flight_sequence_ = 0;
    flow::SceneZone current_scene_ = flow::SceneZone::B;
    std::string current_question_;
    std::string round_capture_directory_;
    std::string scene_photo_path_;
    std::string answer_layout_photo_path_;
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
    std::deque<std::string> handled_order;
    constexpr size_t kMaximumRememberedRequestIds = 4096;
    flow::ResidentMatchStateMachine state;
    SdkActions actions(client);
    flow::RoundMission<SdkActions> mission(actions);
    RefereeEvent event;
    std::optional<std::string> cached_question;

    auto abort_current_match = [&](const std::string& reason) {
        state.markReturning();
        print("[abort] " + reason);
        const bool landed = mission.emergencyReturn(reason);
        cached_question.reset();
        if (landed) {
            state.markFaulted();
            print("[resident] current match faulted; repeated start events are "
                  "locked out until MATCH_FINISHED or process restart");
        } else {
            print("[abort] return-to-home was not confirmed; process stopping");
            g_stop.store(true);
        }
    };

    while (g_events.pop(event)) {
        if (g_stop.load()) break;

        if (!event.request_id.empty()) {
            if (!handled_ids.insert(event.request_id).second) {
                print("[event] duplicate ignored: " + event.request_id);
                continue;
            }
            handled_order.push_back(event.request_id);
            if (handled_order.size() > kMaximumRememberedRequestIds) {
                handled_ids.erase(handled_order.front());
                handled_order.pop_front();
            }
        }

        if (!execute) {
            print("[dry-run] would handle " + event.message);
            continue;
        }

        const flow::EventAction action = state.handle(classifyEvent(event));
        if (action == flow::EventAction::Ignore) {
            print("[event] ignored in current resident state: " +
                  event.message);
            continue;
        }

        if (action == flow::EventAction::EndMatch) {
            cached_question.reset();
            g_urgent_return.store(false);
            print("[resident] match ended on ground; waiting for MATCH_STARTED");
            continue;
        }

        if (action == flow::EventAction::EmergencyReturn) {
            abort_current_match(event.message);
            if (g_stop.load()) break;
            continue;
        }

        if (action == flow::EventAction::StartRound) {
            cached_question.reset();
            const int round_index = state.roundIndex();
            const flow::SceneZone scene = state.currentScene();
            if (!mission.start(round_index, scene)) {
                abort_current_match(
                    std::string("round preflight, takeoff, or scene ") +
                    flow::sceneName(scene) + " navigation failed");
                if (g_stop.load()) break;
                continue;
            }
            state.markSceneReady();
            print(std::string("[round] scene ") + flow::sceneName(scene) +
                  " reached; waiting for trigger");
            continue;
        }

        if (action == flow::EventAction::AwaitQuestion) {
            print("[round] scene triggered; waiting for question");
            continue;
        }

        if (action == flow::EventAction::CacheQuestion) {
            cached_question = event.message;
            print("[round] question arrived before trigger; cached for this round");
            continue;
        }

        if (action == flow::EventAction::ExecuteAnswer ||
            action == flow::EventAction::ExecuteCachedQuestion) {
            const std::string question =
                action == flow::EventAction::ExecuteCachedQuestion
                    ? cached_question.value_or("")
                    : event.message;
            cached_question.reset();
            if (question.empty()) {
                abort_current_match("cached question was missing");
                if (g_stop.load()) break;
                continue;
            }

            print("[match] question received: " + question);
            const auto semantic_answer = mission.observeQuestion(question);
            if (!semantic_answer) {
                abort_current_match(
                    "recognition failed or returned an invalid token");
                if (g_stop.load()) break;
                continue;
            }
            if (g_urgent_return.load()) {
                abort_current_match("urgent return requested during recognition");
                if (g_stop.load()) break;
                continue;
            }

            print(std::string("[round] recognized semantic answer=") +
                  flow::answerName(*semantic_answer));

            if (!mission.deliverAndLand(*semantic_answer)) {
                abort_current_match(
                    "layout recognition, answer execution, or return failed");
                if (g_stop.load()) break;
                continue;
            }

            state.markRoundCompleted();
            print("[round] flow completed and landed; resident process is "
                  "waiting for NEXT_ROUND_STARTED or MATCH_STARTED");
        }
    }
}

void requestReturnOnExit(iking::drone::Client& client, bool execute) {
    if (!execute) return;

    if (!g_flight_commanded_by_process.load()) {
        print("[shutdown] no flight was commanded by this process; return "
              "command suppressed");
        return;
    }

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

bool visionCredentialsAvailable() {
    const char* vision_key = std::getenv("VISION_API_KEY");
    const char* dashscope_key = std::getenv("DASHSCOPE_API_KEY");
    const char* key_file = std::getenv("VISION_API_KEY_FILE");
    const bool vision_credentials =
        (vision_key && *vision_key) ||
        (key_file && *key_file && std::filesystem::is_regular_file(key_file));
    const bool provider_override =
        std::getenv("VISION_API_URL") || std::getenv("VISION_MODEL");
    if (provider_override) return vision_credentials;
    return vision_credentials || (dashscope_key && *dashscope_key);
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
    const char* vision_url = std::getenv("VISION_API_URL");
    const char* vision_model = std::getenv("VISION_MODEL");
    metadata["vision_api_url"] =
        (vision_url && *vision_url)
            ? vision_url
            : recognize::kDefaultApiUrl;
    metadata["vision_model"] =
        (vision_model && *vision_model)
            ? vision_model
            : recognize::kDefaultModelName;
    metadata["mapping_mode"] = "semantic-answer-plus-recognized-layout";
    metadata["resident_process"] = true;
    metadata["portable_site_mode"] = true;
    metadata["portable_horizontal_radius_meters"] =
        kPortableHorizontalRadiusMeters;
    metadata["portable_maximum_relative_altitude_meters"] =
        kPortableMaximumAltitudeMeters;
    metadata["mission_altitude_meters"] = kMissionAltitude;
    metadata["portable_anchor_source"] =
        "first-round ground position and aircraft yaw";
    metadata["scene_schedule"] = "MATCH_STARTED:B;NEXT_ROUND_STARTED:A/B alternating";
    const char* simulation_oracle = std::getenv("IKING_ALLOW_SIM_ORACLE");
    metadata["simulation_oracle_enabled"] =
        simulation_oracle && std::string(simulation_oracle) == "1";
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
        << "  --execute       enable the resident multi-round simulation flow\n"
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
        if (!visionCredentialsAvailable()) {
            std::cerr << "[startup] --execute requires vision credentials"
                      << std::endl;
            return 2;
        }
    }
    if (!initializeArtifacts(options)) return 1;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGUSR1, onSignal);
    std::signal(SIGUSR2, onSignal);

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
    if (options.execute && !waitForGroundTelemetry("startup")) {
        print("[startup] execute preflight requires fresh ground telemetry");
        client.disconnect();
        return 1;
    }

    bool scene_camera_ready = !options.execute;
    bool answer_camera_ready = !options.execute;
    if (options.execute) {
        const auto capability = client.getPodCapability();
        if (isOk(capability, "getPodCapability(preflight)")) {
            scene_camera_ready =
                hasCameraType(capability.msg, "imx586") ||
                hasCameraType(capability.msg, "fisheye");
            answer_camera_ready = hasCameraType(capability.msg, "light");
        }
    }

    if (!scene_camera_ready) {
        print("[startup] Front unavailable; scene captures will fail");
    }
    if (!answer_camera_ready) {
        print("[startup] PodVisibleLight unavailable; answer layout captures will fail");
    }
    if (options.execute && (!scene_camera_ready || !answer_camera_ready)) {
        print("[startup] execute preflight failed: both cameras are required");
        client.disconnect();
        return 1;
    }

    print("[startup] SDK connected; waiting for taskGuidance");

    std::thread worker(eventLoop, std::ref(client), options.execute);
    while (!g_stop.load()) {
        pollManualEvents();
        std::this_thread::sleep_for(200ms);
    }

    g_events.stop();
    worker.join();
    requestReturnOnExit(client, options.execute);
    client.disconnect();
    print("[shutdown] disconnected");
    return 0;
}
