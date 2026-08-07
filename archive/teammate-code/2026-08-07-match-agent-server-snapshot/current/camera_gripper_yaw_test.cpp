/**
 * @file camera_gripper_yaw_test.cpp
 * @brief 起飞到 10 m，双相机拍照，打开夹爪，向北飞 10 m 并将绝对
 *        yaw 设为 +90°，关闭夹爪，最后返航降落。
 *
 * 运行方式：
 *   ./camera_gripper_yaw_test --execute
 *
 * 本程序会发送真实飞行命令，请先在仿真环境验证。
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "iking_drone_sdk.h"

namespace {

using namespace std::chrono_literals;

constexpr double kTakeoffHeightMeters = 6.0;
constexpr double kNorthDistanceMeters = 10.0;
constexpr double kTargetYawDegrees = 90.0;

constexpr int kRpcTimeoutMs = 5000;
constexpr int kPositionRpcTimeoutMs = 10000;
constexpr int kFrameAcquireTimeoutMs = 5000;
constexpr int kCameraWarmupAttempts = 3;
constexpr int kPhotoCaptureAttempts = 3;
constexpr auto kTakeoffTimeout = 120s;
constexpr auto kNavigationTimeout = 90s;
constexpr auto kLandingTimeout = 180s;
constexpr auto kArrivalStableDuration = 1s;
constexpr double kArrivalToleranceMeters = 0.50;
constexpr double kAltitudeToleranceMeters = 0.50;
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kPi = 3.14159265358979323846;

constexpr char kCaptureDirectory[] =
    "/opt/iking/match_agent/captures";
constexpr char kFrontPhotoPath[] =
    "/opt/iking/match_agent/captures/front_10m.jpg";
constexpr char kPodPhotoPath[] =
    "/opt/iking/match_agent/captures/pod_visible_10m.jpg";

constexpr auto kFrontChannel =
    iking::drone::StreamChannelType::Front;
constexpr auto kPodChannel =
    iking::drone::StreamChannelType::PodVisibleLight;

constexpr int kFrameRgb = 0;
constexpr int kFrameBgr = 1;
constexpr int kFrameYuv420p = 3;

std::atomic<bool> g_stop{false};
std::mutex g_print_mutex;

struct PositionSnapshot {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    std::chrono::steady_clock::time_point updated_at{};
};

struct TelemetryStore {
    std::mutex mutex;
    std::condition_variable cv;
    PositionSnapshot position;
    bool valid = false;
};

TelemetryStore g_telemetry;

void print(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << text << std::endl;
}

void onSignal(int) {
    g_stop.store(true);
}

bool isOk(const iking::drone::Result& result, const char* action) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << "[sdk] " << action
              << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) {
        std::cout << " error=" << result.error;
    }
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

    {
        std::lock_guard<std::mutex> lock(g_telemetry.mutex);
        g_telemetry.position = snapshot;
        g_telemetry.valid = true;
    }
    g_telemetry.cv.notify_all();
}

bool waitForFreshPosition(PositionSnapshot& output,
                          std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(g_telemetry.mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (!g_stop.load()) {
        if (g_telemetry.valid &&
            std::chrono::steady_clock::now() -
                    g_telemetry.position.updated_at < 2s) {
            output = g_telemetry.position;
            return true;
        }

        if (g_telemetry.cv.wait_until(lock, deadline) ==
            std::cv_status::timeout) {
            break;
        }
    }

    return false;
}

bool waitForMode(iking::drone::Client& client,
                 iking::drone::DRONE_MODE_STATUS_t target,
                 std::chrono::seconds timeout,
                 bool interruptible) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int previous_mode = -1;
    int failed_queries = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        if (interruptible && g_stop.load()) return false;

        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto result = client.getModeStatus(mode, 2000);
        if (!iking::drone::isOk(result.status)) {
            if (++failed_queries >= 3) {
                isOk(result, "getModeStatus");
                return false;
            }
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

    print(std::string("[mode] timeout waiting for ") + modeName(target));
    return false;
}

bool capturePhoto(iking::drone::Client& client,
                  iking::drone::StreamChannelType channel,
                  const char* camera_name,
                  const char* path) {
    std::error_code error;
    std::filesystem::create_directories(kCaptureDirectory, error);
    if (error) {
        print("[capture] cannot create directory: " + error.message());
        return false;
    }

    iking::drone::FrameView frame;
    const auto acquire_status = client.acquireFrame(
        channel, frame, kFrameAcquireTimeoutMs);
    if (acquire_status != iking::drone::StatusCode::Ok) {
        print(std::string("[capture] ") + camera_name +
              " acquireFrame failed, status=" +
              std::to_string(static_cast<int>(acquire_status)));
        return false;
    }

    bool saved = false;
    if (frame.data && frame.width > 0 && frame.height > 0) {
        cv::Mat bgr;

        if (frame.type == kFrameBgr) {
            bgr = cv::Mat(frame.height,
                          frame.width,
                          CV_8UC3,
                          frame.data);
        } else if (frame.type == kFrameRgb) {
            const cv::Mat rgb(frame.height,
                              frame.width,
                              CV_8UC3,
                              frame.data);
            cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        } else if (frame.type == kFrameYuv420p) {
            const cv::Mat yuv(frame.height + frame.height / 2,
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
        }
    }

    const auto release_status = client.releaseFrame(channel, frame);
    if (release_status != iking::drone::StatusCode::Ok) {
        print(std::string("[capture] ") + camera_name +
              " releaseFrame failed, status=" +
              std::to_string(static_cast<int>(release_status)));
    }

    print(saved
              ? std::string("[capture] ") + camera_name + " saved " + path
              : std::string("[capture] ") + camera_name +
                    " failed to save " + path);
    return saved;
}

bool warmUpCamera(iking::drone::Client& client,
                  iking::drone::StreamChannelType channel,
                  const char* camera_name) {
    for (int attempt = 1; attempt <= kCameraWarmupAttempts; ++attempt) {
        if (g_stop.load()) return false;

        iking::drone::FrameView frame;
        const auto acquire_status = client.acquireFrame(
            channel, frame, kFrameAcquireTimeoutMs);

        if (acquire_status == iking::drone::StatusCode::Ok) {
            const auto release_status = client.releaseFrame(channel, frame);
            if (release_status == iking::drone::StatusCode::Ok) {
                print(std::string("[warmup] ") + camera_name +
                      " ready on attempt " + std::to_string(attempt));
                return true;
            }

            print(std::string("[warmup] ") + camera_name +
                  " releaseFrame failed, status=" +
                  std::to_string(static_cast<int>(release_status)));
        } else {
            print(std::string("[warmup] ") + camera_name +
                  " attempt " + std::to_string(attempt) +
                  " failed, status=" +
                  std::to_string(static_cast<int>(acquire_status)));
        }

        std::this_thread::sleep_for(1s);
    }

    print(std::string("[warmup] WARNING: ") + camera_name +
          " unavailable; flight test will continue");
    return false;
}

bool capturePhotoWithRetry(iking::drone::Client& client,
                           iking::drone::StreamChannelType channel,
                           const char* camera_name,
                           const char* path) {
    for (int attempt = 1; attempt <= kPhotoCaptureAttempts; ++attempt) {
        if (g_stop.load()) return false;

        print(std::string("[capture] ") + camera_name +
              " attempt " + std::to_string(attempt));
        if (capturePhoto(client, channel, camera_name, path)) {
            return true;
        }

        std::this_thread::sleep_for(1s);
    }

    print(std::string("[capture] WARNING: ") + camera_name +
          " photo failed; flight test will continue");
    return false;
}

double degreesToRadians(double value) {
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

bool flyNorthTenMeters(iking::drone::Client& client) {
    PositionSnapshot start;
    if (!waitForFreshPosition(start, 5s)) {
        print("[navigation] no fresh position available");
        return false;
    }

    const double target_latitude =
        start.latitude +
        (kNorthDistanceMeters / kEarthRadiusMeters) * 180.0 / kPi;
    const double target_longitude = start.longitude;

    print("[navigation] flying 10 m north with absolute yaw=+90 deg");
    if (!isOk(client.setPosition(target_longitude,
                                 target_latitude,
                                 kTakeoffHeightMeters,
                                 kTargetYawDegrees,
                                 kPositionRpcTimeoutMs),
              "setPosition(north 10m, yaw=90)")) {
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + kNavigationTimeout;
    auto last_update = std::chrono::steady_clock::time_point{};
    auto last_log = std::chrono::steady_clock::time_point{};
    auto stable_since = std::chrono::steady_clock::time_point{};
    bool stable = false;

    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load()) return false;

        PositionSnapshot position;
        if (!waitForFreshPosition(position, 2s) ||
            position.updated_at == last_update) {
            std::this_thread::sleep_for(50ms);
            continue;
        }
        last_update = position.updated_at;

        const double horizontal_error = horizontalDistanceMeters(
            position, target_latitude, target_longitude);
        const double altitude_error =
            std::abs(position.altitude - kTakeoffHeightMeters);

        if (last_log.time_since_epoch().count() == 0 ||
            position.updated_at - last_log >= 1s) {
            print("[navigation] distance=" +
                  std::to_string(horizontal_error) +
                  "m altitude=" + std::to_string(position.altitude) + "m");
            last_log = position.updated_at;
        }

        if (horizontal_error <= kArrivalToleranceMeters &&
            altitude_error <= kAltitudeToleranceMeters) {
            if (!stable) {
                stable = true;
                stable_since = position.updated_at;
            } else if (position.updated_at - stable_since >=
                       kArrivalStableDuration) {
                print("[navigation] north target reached; holding position");
                return true;
            }
        } else {
            stable = false;
        }
    }

    print("[navigation] timeout waiting for north target");
    return false;
}

bool returnHomeAndWait(iking::drone::Client& client) {
    const auto result = client.returnToHome(kPositionRpcTimeoutMs);
    if (!isOk(result, "returnToHome")) return false;

    print("[landing] return-to-home accepted; waiting for STANDBY");
    const bool landed = waitForMode(
        client, iking::drone::STANDBY, kLandingTimeout, false);
    print(landed ? "[landing] landing confirmed"
                 : "[landing] landing was not confirmed");
    return landed;
}

void printUsage(const char* program) {
    std::cout
        << "usage: " << program << " --execute\n"
        << "WARNING: take off to 10 m, capture two photos, fly 10 m "
           "north with yaw=+90, then return home and land.\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--execute") != 0) {
        printUsage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    iking::drone::Config config;
    config.client_id = "camera_gripper_yaw_test";
    iking::drone::Client client(config);

    if (!client.connect()) {
        std::cerr << "[startup] SDK connect failed" << std::endl;
        return 1;
    }
    client.onStatus(onStatus);
    print("[startup] SDK connected");

    bool front_open = isOk(client.openFramePool(kFrontChannel),
                           "openFramePool(Front)");
    bool pod_open = isOk(client.openFramePool(kPodChannel),
                         "openFramePool(PodVisibleLight)");
    bool airborne = false;
    bool gripper_open = false;
    bool success = false;

    do {
        if (g_stop.load()) {
            print("[test] cancelled before takeoff");
            break;
        }

        // 起飞前主动请求并归还一帧，让两个视频流完成启动和握手。
        if (front_open) {
            warmUpCamera(client, kFrontChannel, "Front");
        } else {
            print("[warmup] WARNING: Front FramePool is not open");
        }
        if (pod_open) {
            warmUpCamera(client, kPodChannel, "PodVisibleLight");
        } else {
            print("[warmup] WARNING: PodVisibleLight FramePool is not open");
        }

        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        if (!isOk(client.getModeStatus(mode, 2000),
                  "getModeStatus(initial)") ||
            mode != iking::drone::STANDBY) {
            print("[test] initial mode must be STANDBY");
            break;
        }

        if (!isOk(client.gripperClose(kRpcTimeoutMs),
                  "gripperClose(before takeoff)")) {
            break;
        }

        if (g_stop.load()) {
            print("[test] cancelled before takeoff command");
            break;
        }

        if (!isOk(client.takeOff(kTakeoffHeightMeters, kRpcTimeoutMs),
                  "takeOff(10m)")) {
            break;
        }
        airborne = true;

        print("[takeoff] accepted; waiting for POSITION");
        if (!waitForMode(client,
                         iking::drone::POSITION,
                         kTakeoffTimeout,
                         true)) {
            print("[takeoff] completion was not confirmed");
            break;
        }

        std::this_thread::sleep_for(1s);
        const bool front_saved = front_open && capturePhotoWithRetry(
            client, kFrontChannel, "Front", kFrontPhotoPath);
        const bool pod_saved = pod_open && capturePhotoWithRetry(
            client, kPodChannel, "PodVisibleLight", kPodPhotoPath);

        // 相机测试已经完成，先关闭视频流，再操作复用吊舱控制的夹爪。
        if (front_open) {
            client.closeFramePool(kFrontChannel);
            front_open = false;
        }
        if (pod_open) {
            client.closeFramePool(kPodChannel);
            pod_open = false;
        }

        if (!front_saved || !pod_saved) {
            print("[test] WARNING: one or more photos failed; "
                  "continuing with gripper and flight steps");
        }

        std::this_thread::sleep_for(500ms);
        if (!isOk(client.gripperOpen(kRpcTimeoutMs), "gripperOpen")) {
            break;
        }
        gripper_open = true;

        std::this_thread::sleep_for(1s);
        if (!flyNorthTenMeters(client)) break;

        if (!isOk(client.gripperClose(kRpcTimeoutMs), "gripperClose")) {
            break;
        }
        gripper_open = false;

        success = returnHomeAndWait(client);
        airborne = !success;
    } while (false);

    if (gripper_open) {
        isOk(client.gripperClose(kRpcTimeoutMs),
             "gripperClose(cleanup)");
    }

    if (airborne) {
        print("[cleanup] test incomplete; requesting return-to-home");
        returnHomeAndWait(client);
    }

    if (front_open) client.closeFramePool(kFrontChannel);
    if (pod_open) client.closeFramePool(kPodChannel);
    client.disconnect();
    print("[shutdown] disconnected");
    return success ? 0 : 1;
}
