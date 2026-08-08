/**
 * Portable square-route performance test for the iKing drone SDK.
 *
 * The program defaults to --dry-run. Actual SDK writes require the launcher
 * to set IKING_PERFORMANCE_EXECUTION_CONFIRMED=1 after live preflight checks.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
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
#include <vector>

#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "iking_drone_sdk.h"
#include "performance_plan.hpp"

using namespace std::chrono_literals;

namespace {

constexpr int kRpcTimeoutMs = 5000;
constexpr int kFrameRgb = 0;
constexpr int kFrameBgr = 1;
constexpr int kFrameYuv420p = 3;
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kGroundAltitudeToleranceMeters = 0.10;
constexpr double kPositionToleranceMeters = 0.45;
constexpr double kAltitudeToleranceMeters = 0.25;
constexpr double kHeadingToleranceDegrees = 5.0;
constexpr auto kTelemetryFreshness = 3s;
constexpr auto kArrivalStableDuration = 1s;
constexpr auto kNavigationTimeout = 60s;
constexpr auto kReturnTimeout = 180s;
constexpr char kDefaultRunRoot[] =
    "/opt/iking/portable_performance_test/runs";

struct ProgramOptions {
    bool execute = false;
    performance::Options route;
    std::string run_id;
    std::string run_root = kDefaultRunRoot;
};

struct PositionSnapshot {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double heading_degrees = 0.0;
    double horizontal_speed = 0.0;
    double vertical_speed = 0.0;
    bool heading_valid = false;
    bool speed_valid = false;
    bool armed = true;
    bool sdk_mode = false;
    bool flight_state_valid = false;
    std::string flight_path;
    std::chrono::steady_clock::time_point updated_at{};
};

struct Target {
    double latitude;
    double longitude;
    double altitude;
    double heading_degrees;
};

struct MetricState {
    bool has_previous = false;
    PositionSnapshot previous;
    double max_horizontal_speed = 0.0;
    double max_vertical_speed = 0.0;
    double max_relative_altitude = 0.0;
    double max_launch_distance = 0.0;
};

std::atomic<bool> g_stop{false};
std::atomic<bool> g_safety_violation{false};
std::atomic<bool> g_flight_owned{false};
std::mutex g_print_mutex;
std::mutex g_artifact_mutex;
std::mutex g_telemetry_mutex;
std::mutex g_metrics_mutex;
PositionSnapshot g_position;
bool g_position_valid = false;
MetricState g_metrics;
std::optional<PositionSnapshot> g_launch;
performance::Options g_route_options;
std::string g_run_directory;
std::string g_capture_directory;
std::ofstream g_run_log;
std::ofstream g_command_log;
std::ofstream g_telemetry_log;
Json::Value g_segments(Json::arrayValue);
int g_front_photos = 0;
int g_pod_photos = 0;
int g_capture_failures = 0;
double g_takeoff_seconds = 0.0;
double g_return_seconds = 0.0;
double g_closure_error_meters = std::numeric_limits<double>::quiet_NaN();

double degreesToRadians(double value) {
    constexpr double kPi = 3.14159265358979323846;
    return value * kPi / 180.0;
}

double radiansToDegrees(double value) {
    constexpr double kPi = 3.14159265358979323846;
    return value * 180.0 / kPi;
}

double normalizeDegrees(double value) {
    return radiansToDegrees(std::atan2(
        std::sin(degreesToRadians(value)),
        std::cos(degreesToRadians(value))));
}

double headingError(double expected, double actual) {
    return std::abs(normalizeDegrees(actual - expected));
}

double horizontalDistance(double latitude_a,
                          double longitude_a,
                          double latitude_b,
                          double longitude_b) {
    const double mean_latitude =
        degreesToRadians((latitude_a + latitude_b) / 2.0);
    const double east = degreesToRadians(longitude_b - longitude_a) *
                        kEarthRadiusMeters * std::cos(mean_latitude);
    const double north = degreesToRadians(latitude_b - latitude_a) *
                         kEarthRadiusMeters;
    return std::hypot(east, north);
}

Target relativeTarget(const PositionSnapshot& start,
                      double forward,
                      double left,
                      double up,
                      double yaw_offset) {
    const double heading = degreesToRadians(start.heading_degrees);
    const double east = forward * std::sin(heading) - left * std::cos(heading);
    const double north = forward * std::cos(heading) + left * std::sin(heading);
    return Target{
        start.latitude + radiansToDegrees(north / kEarthRadiusMeters),
        start.longitude + radiansToDegrees(
            east / (kEarthRadiusMeters *
                    std::cos(degreesToRadians(start.latitude)))),
        start.altitude + up,
        normalizeDegrees(start.heading_degrees + yaw_offset),
    };
}

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string defaultRunId() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y%m%d-%H%M%S");
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

void recordSegment(const char* action,
                   int station,
                   double seconds,
                   bool success) {
    Json::Value item;
    item["action"] = action;
    item["station"] = station;
    item["duration_seconds"] = seconds;
    item["success"] = success;
    g_segments.append(item);
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

void onSignal(int) {
    g_stop.store(true);
}

void onStatus(const std::string& json, void*) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json, root, false)) return;
    const Json::Value& data = root["status"]["data"];
    const Json::Value& flight = data["flight"];
    const Json::Value& position = flight["positionStatus"];
    const Json::Value& attitude = flight["attitude"];
    const Json::Value& speed = flight["speed"];
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
    snapshot.speed_valid = speed.isObject() &&
                           speed["x"].isNumeric() &&
                           speed["y"].isNumeric() &&
                           speed["z"].isNumeric();
    if (snapshot.speed_valid) {
        snapshot.horizontal_speed = std::hypot(
            speed["x"].asDouble(), speed["y"].asDouble());
        snapshot.vertical_speed = speed["z"].asDouble();
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

    {
        std::lock_guard<std::mutex> lock(g_telemetry_mutex);
        g_position = snapshot;
        g_position_valid = true;
    }

    {
        std::lock_guard<std::mutex> lock(g_metrics_mutex);
        if (snapshot.speed_valid &&
            std::isfinite(snapshot.horizontal_speed) &&
            std::isfinite(snapshot.vertical_speed)) {
            g_metrics.max_horizontal_speed = std::max(
                g_metrics.max_horizontal_speed,
                snapshot.horizontal_speed);
            g_metrics.max_vertical_speed = std::max(
                g_metrics.max_vertical_speed,
                std::abs(snapshot.vertical_speed));
        } else if (g_metrics.has_previous) {
            const double seconds = std::chrono::duration<double>(
                snapshot.updated_at - g_metrics.previous.updated_at).count();
            if (seconds > 0.05) {
                g_metrics.max_horizontal_speed = std::max(
                    g_metrics.max_horizontal_speed,
                    horizontalDistance(
                        g_metrics.previous.latitude,
                        g_metrics.previous.longitude,
                        snapshot.latitude,
                        snapshot.longitude) / seconds);
                g_metrics.max_vertical_speed = std::max(
                    g_metrics.max_vertical_speed,
                    std::abs(snapshot.altitude -
                             g_metrics.previous.altitude) / seconds);
            }
        }
        g_metrics.previous = snapshot;
        g_metrics.has_previous = true;
        if (g_launch) {
            const double launch_distance = horizontalDistance(
                g_launch->latitude,
                g_launch->longitude,
                snapshot.latitude,
                snapshot.longitude);
            const double relative_altitude =
                snapshot.altitude - g_launch->altitude;
            g_metrics.max_launch_distance = std::max(
                g_metrics.max_launch_distance, launch_distance);
            g_metrics.max_relative_altitude = std::max(
                g_metrics.max_relative_altitude, relative_altitude);
            if (g_flight_owned.load() &&
                (launch_distance > g_route_options.site_radius_meters + 0.25 ||
                 relative_altitude >
                     g_route_options.site_altitude_limit_meters + 0.25 ||
                 relative_altitude < -0.25)) {
                g_safety_violation.store(true);
            }
        }
    }

    Json::Value record;
    record["latitude"] = snapshot.latitude;
    record["longitude"] = snapshot.longitude;
    record["altitude"] = snapshot.altitude;
    record["heading_degrees"] = snapshot.heading_degrees;
    record["heading_valid"] = snapshot.heading_valid;
    record["horizontal_speed"] = snapshot.horizontal_speed;
    record["vertical_speed"] = snapshot.vertical_speed;
    record["speed_valid"] = snapshot.speed_valid;
    record["armed"] = snapshot.armed;
    record["sdk_mode"] = snapshot.sdk_mode;
    record["flight_state_valid"] = snapshot.flight_state_valid;
    record["flight_path"] = snapshot.flight_path;
    appendJsonLine(g_telemetry_log, record);
}

bool latestPosition(PositionSnapshot& output) {
    std::lock_guard<std::mutex> lock(g_telemetry_mutex);
    if (!g_position_valid) return false;
    output = g_position;
    return true;
}

bool freshPosition(PositionSnapshot& output) {
    return latestPosition(output) &&
           std::chrono::steady_clock::now() - output.updated_at <=
               kTelemetryFreshness;
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

bool waitForMode(iking::drone::Client& client,
                 iking::drone::DRONE_MODE_STATUS_t expected,
                 std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_safety_violation.load()) return false;
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto result = client.getModeStatus(mode, 2000);
        if (iking::drone::isOk(result.status) && mode == expected) return true;
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

bool waitForGround(iking::drone::Client& client,
                   std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int previous_mode = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto result = client.getModeStatus(mode, 2000);
        if (iking::drone::isOk(result.status)) {
            if (static_cast<int>(mode) != previous_mode) {
                print(std::string("[return] mode=") + modeName(mode));
                previous_mode = static_cast<int>(mode);
            }
            PositionSnapshot latest;
            if (mode == iking::drone::STANDBY && freshPosition(latest) &&
                latest.flight_state_valid && !latest.armed &&
                latest.sdk_mode &&
                g_launch &&
                std::abs(latest.altitude - g_launch->altitude) <=
                    kGroundAltitudeToleranceMeters) {
                g_flight_owned.store(false);
                return true;
            }
        }
        std::this_thread::sleep_for(500ms);
    }
    return false;
}

bool waitForTarget(const Target& target,
                   std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto stable_since = std::chrono::steady_clock::time_point{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_safety_violation.load()) return false;
        PositionSnapshot latest;
        if (!freshPosition(latest) || !latest.heading_valid) {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        const double distance = horizontalDistance(
            target.latitude, target.longitude,
            latest.latitude, latest.longitude);
        const bool reached =
            distance <= kPositionToleranceMeters &&
            std::abs(latest.altitude - target.altitude) <=
                kAltitudeToleranceMeters &&
            headingError(target.heading_degrees,
                         latest.heading_degrees) <=
                kHeadingToleranceDegrees;
        if (reached) {
            if (stable_since.time_since_epoch().count() == 0) {
                stable_since = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - stable_since >=
                       kArrivalStableDuration) {
                return true;
            }
        } else {
            stable_since = {};
        }
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

bool hold(double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_stop.load() || g_safety_violation.load()) return false;
        std::this_thread::sleep_for(100ms);
    }
    return true;
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

bool captureFrame(iking::drone::Client& client,
                  iking::drone::StreamChannelType channel,
                  const std::string& path,
                  const char* camera_name) {
    if (!isOk(client.openFramePool(channel),
              (std::string("openFramePool(") + camera_name + ")").c_str())) {
        return false;
    }
    iking::drone::FrameView frame;
    const auto acquire_status = client.acquireFrame(channel, frame, 3000);
    bool saved = false;
    if (acquire_status == iking::drone::StatusCode::Ok) {
        if (frame.data && frame.width > 0 && frame.height > 0) {
            cv::Mat bgr;
            if (frame.type == kFrameBgr) {
                bgr = cv::Mat(frame.height, frame.width, CV_8UC3, frame.data);
            } else if (frame.type == kFrameRgb) {
                const cv::Mat rgb(
                    frame.height, frame.width, CV_8UC3, frame.data);
                cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
            } else if (frame.type == kFrameYuv420p) {
                const cv::Mat yuv(frame.height + frame.height / 2,
                                  frame.width, CV_8UC1, frame.data);
                cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
            }
            if (!bgr.empty()) saved = cv::imwrite(path, bgr);
        }
        client.releaseFrame(channel, frame);
    }
    client.closeFramePool(channel);
    print(std::string("[capture] ") + camera_name +
          (saved ? " saved " : " failed ") + path);
    return saved;
}

void captureStation(iking::drone::Client& client,
                    int station,
                    performance::CameraMode mode) {
    const std::string prefix = g_capture_directory + "/station-" +
                               std::to_string(station);
    if (mode == performance::CameraMode::Front ||
        mode == performance::CameraMode::Both) {
        if (captureFrame(client,
                         iking::drone::StreamChannelType::Front,
                         prefix + "-front.jpg", "Front")) {
            ++g_front_photos;
        } else {
            ++g_capture_failures;
        }
    }
    if (mode == performance::CameraMode::Pod ||
        mode == performance::CameraMode::Both) {
        if (captureFrame(client,
                         iking::drone::StreamChannelType::PodVisibleLight,
                         prefix + "-pod.jpg", "PodVisibleLight")) {
            ++g_pod_photos;
        } else {
            ++g_capture_failures;
        }
    }
}

bool moveRelative(iking::drone::Client& client,
                  double forward,
                  double yaw_offset,
                  double speed,
                  const char* action,
                  int station) {
    PositionSnapshot start;
    if (!freshPosition(start) || !start.heading_valid) return false;
    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!isOk(client.getModeStatus(mode, 2000),
              "getModeStatus(relative-preflight)") ||
        mode != iking::drone::POSITION) {
        print(std::string("[movement] ") + action +
              " requires POSITION");
        return false;
    }

    const Target target = relativeTarget(
        start, forward, 0.0, 0.0, yaw_offset);
    const auto began = std::chrono::steady_clock::now();
    const float yaw = std::abs(yaw_offset) > 1e-6
        ? static_cast<float>(yaw_offset)
        : std::numeric_limits<float>::quiet_NaN();
    const bool accepted = isOk(
        client.setRelativePosition(
            static_cast<float>(forward), 0.0f, 0.0f, yaw,
            kRpcTimeoutMs, static_cast<float>(speed)),
        action);
    const bool reached = accepted && waitForTarget(target, kNavigationTimeout);
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - began).count();
    recordSegment(action, station, seconds, reached);
    print(std::string("[movement] ") + action +
          (reached ? " completed" : " failed") +
          " duration=" + std::to_string(seconds) + "s");
    return reached;
}

bool takeOff(iking::drone::Client& client, double altitude) {
    const auto began = std::chrono::steady_clock::now();
    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!isOk(client.getModeStatus(mode, 2000),
              "getModeStatus(takeoff-preflight)") ||
        mode != iking::drone::STANDBY) {
        return false;
    }
    if (!isOk(client.takeOff(static_cast<float>(altitude), kRpcTimeoutMs),
              "takeOff")) {
        return false;
    }
    g_flight_owned.store(true);
    const bool reached = waitForMode(client, iking::drone::POSITION, 90s);
    g_takeoff_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - began).count();
    recordSegment("takeoff", 0, g_takeoff_seconds, reached);
    return reached;
}

bool returnAndLand(iking::drone::Client& client, const char* reason) {
    if (!g_flight_owned.load() || !g_launch) return true;
    print(std::string("[return] reason=") + reason);
    const auto began = std::chrono::steady_clock::now();
    const Target target{
        g_launch->latitude,
        g_launch->longitude,
        g_launch->altitude + g_route_options.altitude_meters,
        g_launch->heading_degrees,
    };
    const bool accepted = isOk(
        client.returnToAnyPosition(
            static_cast<float>(target.longitude),
            static_cast<float>(target.latitude),
            static_cast<float>(target.altitude),
            static_cast<float>(target.heading_degrees),
            kRpcTimeoutMs),
        "returnToAnyPosition(launch)");
    const bool landed = accepted && waitForGround(client, kReturnTimeout);
    g_return_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - began).count();
    recordSegment("return_and_land", 4, g_return_seconds, landed);
    return landed;
}

bool validRunId(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '-' || ch == '_';
           });
}

bool parseNumber(const char* text, double& output) {
    try {
        std::size_t used = 0;
        output = std::stod(text, &used);
        return used == std::string(text).size() && std::isfinite(output);
    } catch (...) {
        return false;
    }
}

void printUsage(const char* program) {
    std::cout
        << "usage: " << program << " [--dry-run | --execute] [options]\n"
        << "  --altitude M             test altitude, 1..20 (default 3)\n"
        << "  --leg M                  square edge, 1..10 (default 3)\n"
        << "  --speed MPS              movement speed, 0.3..2 (default 1)\n"
        << "  --hover SEC              station hover, 1..10 (default 2)\n"
        << "  --site-radius M          safety radius, 3..20 (default 20)\n"
        << "  --site-altitude-limit M  safety altitude, <=20 (default 20)\n"
        << "  --camera front|pod|both  capture mode (default both)\n"
        << "  --run-id ID              artifact directory name\n"
        << "  --run-root PATH          artifact root\n";
}

bool parseOptions(int argc, char** argv, ProgramOptions& options) {
    options.run_id = defaultRunId();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--execute") options.execute = true;
        else if (arg == "--dry-run") options.execute = false;
        else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else if (arg == "--run-id" && i + 1 < argc) {
            options.run_id = argv[++i];
        } else if (arg == "--run-root" && i + 1 < argc) {
            options.run_root = argv[++i];
        } else if (arg == "--camera" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "front") options.route.camera = performance::CameraMode::Front;
            else if (value == "pod") options.route.camera = performance::CameraMode::Pod;
            else if (value == "both") options.route.camera = performance::CameraMode::Both;
            else return false;
        } else if (i + 1 < argc && arg == "--altitude") {
            if (!parseNumber(argv[++i], options.route.altitude_meters)) return false;
        } else if (i + 1 < argc && arg == "--leg") {
            if (!parseNumber(argv[++i], options.route.leg_meters)) return false;
        } else if (i + 1 < argc && arg == "--speed") {
            if (!parseNumber(argv[++i], options.route.speed_meters_per_second)) return false;
        } else if (i + 1 < argc && arg == "--hover") {
            if (!parseNumber(argv[++i], options.route.hover_seconds)) return false;
        } else if (i + 1 < argc && arg == "--site-radius") {
            if (!parseNumber(argv[++i], options.route.site_radius_meters)) return false;
        } else if (i + 1 < argc && arg == "--site-altitude-limit") {
            if (!parseNumber(argv[++i], options.route.site_altitude_limit_meters)) return false;
        } else {
            return false;
        }
    }
    return validRunId(options.run_id) && !options.run_root.empty();
}

Json::Value routeJson(const performance::Options& route) {
    Json::Value value;
    value["altitude_meters"] = route.altitude_meters;
    value["leg_meters"] = route.leg_meters;
    value["speed_meters_per_second"] = route.speed_meters_per_second;
    value["hover_seconds"] = route.hover_seconds;
    value["turn_degrees"] = route.turn_degrees;
    value["site_radius_meters"] = route.site_radius_meters;
    value["site_altitude_limit_meters"] = route.site_altitude_limit_meters;
    value["maximum_planned_radius_meters"] =
        performance::maximumPlannedRadius(route);
    value["camera"] = performance::cameraModeName(route.camera);
    return value;
}

bool initializeArtifacts(const ProgramOptions& options) {
    g_run_directory = options.run_root + "/" + options.run_id;
    g_capture_directory = g_run_directory + "/captures";
    std::error_code error;
    if (std::filesystem::exists(g_run_directory)) return false;
    std::filesystem::create_directories(g_capture_directory, error);
    if (error) return false;
    g_run_log.open(g_run_directory + "/run.log");
    g_command_log.open(g_run_directory + "/commands.jsonl");
    g_telemetry_log.open(g_run_directory + "/telemetry.jsonl");
    if (!g_run_log || !g_command_log || !g_telemetry_log) return false;

    Json::Value metadata;
    metadata["run_id"] = options.run_id;
    metadata["mode"] = options.execute ? "execute" : "dry-run";
    metadata["started_at"] = utcTimestamp();
    metadata["route"] = routeJson(options.route);
    metadata["coordinate_mode"] = "body-relative square from launch pose";
    metadata["environment"] = std::getenv("IKING_TEST_ENVIRONMENT")
        ? std::getenv("IKING_TEST_ENVIRONMENT") : "not-selected";
    if (const char* return_height = std::getenv("IKING_RETURN_HEIGHT")) {
        metadata["verified_return_height"] = return_height;
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::ofstream file(g_run_directory + "/metadata.json");
    file << Json::writeString(builder, metadata) << '\n';
    return static_cast<bool>(file);
}

void writePlan(const performance::Options& route) {
    Json::Value root;
    root["route"] = routeJson(route);
    Json::Value steps(Json::arrayValue);
    for (const auto& step : performance::makeSquarePlan(route)) {
        Json::Value item;
        item["action"] = performance::stepKindName(step.kind);
        item["station"] = step.station;
        item["value"] = step.value;
        steps.append(item);
    }
    root["steps"] = steps;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::ofstream file(g_run_directory + "/plan.json");
    file << Json::writeString(builder, root) << '\n';
}

void writeMetrics(bool success, const std::string& result) {
    Json::Value root;
    root["success"] = success;
    root["result"] = result;
    root["completed_at"] = utcTimestamp();
    root["takeoff_seconds"] = g_takeoff_seconds;
    root["return_seconds"] = g_return_seconds;
    root["front_photos"] = g_front_photos;
    root["pod_photos"] = g_pod_photos;
    root["capture_failures"] = g_capture_failures;
    root["safety_violation"] = g_safety_violation.load();
    if (std::isfinite(g_closure_error_meters)) {
        root["closure_error_meters"] = g_closure_error_meters;
    }
    {
        std::lock_guard<std::mutex> lock(g_metrics_mutex);
        root["max_horizontal_speed_mps"] = g_metrics.max_horizontal_speed;
        root["max_vertical_speed_mps"] = g_metrics.max_vertical_speed;
        root["max_relative_altitude_meters"] = g_metrics.max_relative_altitude;
        root["max_launch_distance_meters"] = g_metrics.max_launch_distance;
    }
    root["segments"] = g_segments;
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::ofstream file(g_run_directory + "/metrics.json");
    file << Json::writeString(builder, root) << '\n';
}

bool groundPreflight(iking::drone::Client& client) {
    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!isOk(client.getModeStatus(mode, 2000),
              "getModeStatus(preflight)") ||
        mode != iking::drone::STANDBY) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        PositionSnapshot latest;
        if (freshPosition(latest) && latest.heading_valid &&
            latest.flight_state_valid && !latest.armed && latest.sdk_mode &&
            std::abs(latest.altitude) <= kGroundAltitudeToleranceMeters) {
            {
                std::lock_guard<std::mutex> lock(g_metrics_mutex);
                g_launch = latest;
            }
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    ProgramOptions options;
    if (!parseOptions(argc, argv, options)) {
        printUsage(argv[0]);
        return 2;
    }
    std::string validation_error;
    if (!performance::isValid(options.route, &validation_error)) {
        std::cerr << "[startup] invalid route: " << validation_error << '\n';
        return 2;
    }
    if (options.execute) {
        const char* confirmed =
            std::getenv("IKING_PERFORMANCE_EXECUTION_CONFIRMED");
        if (!confirmed || std::string(confirmed) != "1") {
            std::cerr << "[startup] --execute must use scripts/run.sh\n";
            return 2;
        }
    }
    if (!initializeArtifacts(options)) {
        std::cerr << "[startup] cannot initialize run artifacts\n";
        return 1;
    }
    writePlan(options.route);
    g_route_options = options.route;

    if (!options.execute) {
        print("[startup] dry-run; no SDK connection or flight command");
        for (const auto& step : performance::makeSquarePlan(options.route)) {
            print(std::string("[plan] ") + performance::stepKindName(step.kind) +
                  " station=" + std::to_string(step.station) +
                  " value=" + std::to_string(step.value));
        }
        writeMetrics(true, "dry-run plan generated");
        return 0;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    print("[startup] execute enabled after launcher preflight");

    iking::drone::Config config;
    config.client_id = "portable_performance_test";
    iking::drone::Client client(config);
    if (!client.connect()) {
        print("[startup] SDK connect failed");
        writeMetrics(false, "SDK connect failed");
        return 1;
    }
    client.onStatus(onStatus);

    bool route_success = false;
    std::string result = "preflight failed";
    if (!groundPreflight(client)) {
        print("[preflight] STANDBY, ground telemetry, position and yaw required");
    } else {
        const auto capability = client.getPodCapability();
        const bool capability_ok = isOk(capability, "getPodCapability");
        const bool front_available = capability_ok &&
            (hasCameraType(capability.msg, "imx586") ||
             hasCameraType(capability.msg, "fisheye"));
        const bool pod_available = capability_ok &&
            hasCameraType(capability.msg, "light");
        const bool cameras_ready =
            (options.route.camera == performance::CameraMode::Front && front_available) ||
            (options.route.camera == performance::CameraMode::Pod && pod_available) ||
            (options.route.camera == performance::CameraMode::Both &&
             front_available && pod_available);

        if (!cameras_ready) {
            result = "selected camera channel unavailable";
            print("[preflight] selected camera channel unavailable");
        } else {
            if (options.route.camera != performance::CameraMode::Front &&
                !isOk(client.gimbalDownSet(), "gimbalDownSet")) {
                result = "cannot point pod camera down";
            } else if (!takeOff(client, options.route.altitude_meters)) {
                result = "takeoff failed";
            } else {
                route_success = hold(options.route.hover_seconds);
                if (route_success) captureStation(client, 0, options.route.camera);
                for (int station = 1; station <= 4 && route_success; ++station) {
                    route_success = moveRelative(
                        client, options.route.leg_meters, 0.0,
                        options.route.speed_meters_per_second,
                        "move_forward", station);
                    if (route_success) route_success = hold(options.route.hover_seconds);
                    if (route_success) {
                        route_success = moveRelative(
                            client, 0.0, options.route.turn_degrees,
                            options.route.speed_meters_per_second,
                            "turn_clockwise_90", station);
                    }
                    if (route_success) route_success = hold(options.route.hover_seconds);
                    if (route_success) captureStation(client, station, options.route.camera);
                }
                PositionSnapshot before_return;
                if (freshPosition(before_return) && g_launch) {
                    g_closure_error_meters = horizontalDistance(
                        g_launch->latitude, g_launch->longitude,
                        before_return.latitude, before_return.longitude);
                }
                if (g_safety_violation.load()) {
                    result = "safety envelope violated";
                    route_success = false;
                } else if (g_stop.load()) {
                    result = "interrupted";
                    route_success = false;
                } else if (!route_success) {
                    result = "movement, turn or hover failed";
                } else if (g_capture_failures > 0) {
                    result = "route completed with camera failures";
                    route_success = false;
                } else {
                    result = "route completed";
                }
            }
        }
    }

    const bool landed = returnAndLand(client, result.c_str());
    if (!landed) {
        result += "; landing not confirmed";
        route_success = false;
    }
    if (g_safety_violation.load()) {
        if (result.find("safety envelope violated") == std::string::npos) {
            result += "; safety envelope violated during return";
        }
        route_success = false;
    }
    client.disconnect();
    const bool success = route_success && landed;
    writeMetrics(success, result);
    print(success ? "[result] PASS" : "[result] FAIL: " + result);
    return success ? 0 : 1;
}
