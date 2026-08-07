#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace performance {

enum class CameraMode { Front, Pod, Both };
enum class StepKind { Takeoff, Hover, Capture, MoveForward, Turn, Return };

struct Options {
    double altitude_meters = 3.0;
    double leg_meters = 3.0;
    double speed_meters_per_second = 1.0;
    double hover_seconds = 2.0;
    double turn_degrees = 90.0;
    double site_radius_meters = 10.0;
    double site_altitude_limit_meters = 10.0;
    CameraMode camera = CameraMode::Both;
};

struct Step {
    StepKind kind;
    int station;
    double value;
};

inline double maximumPlannedRadius(const Options& options) {
    return std::sqrt(2.0) * options.leg_meters;
}

inline bool isValid(const Options& options, std::string* error = nullptr) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!std::isfinite(options.altitude_meters) ||
        options.altitude_meters < 1.0 || options.altitude_meters > 10.0) {
        return fail("altitude must be within [1, 10] meters");
    }
    if (!std::isfinite(options.leg_meters) ||
        options.leg_meters < 1.0 || options.leg_meters > 10.0) {
        return fail("leg must be within [1, 10] meters");
    }
    if (!std::isfinite(options.speed_meters_per_second) ||
        options.speed_meters_per_second < 0.3 ||
        options.speed_meters_per_second > 2.0) {
        return fail("speed must be within [0.3, 2.0] m/s");
    }
    if (!std::isfinite(options.hover_seconds) ||
        options.hover_seconds < 1.0 || options.hover_seconds > 10.0) {
        return fail("hover must be within [1, 10] seconds");
    }
    if (!std::isfinite(options.turn_degrees) ||
        std::abs(options.turn_degrees - 90.0) > 1e-6) {
        return fail("the square test requires a 90 degree clockwise turn");
    }
    if (!std::isfinite(options.site_radius_meters) ||
        options.site_radius_meters < 3.0 ||
        options.site_radius_meters > 20.0) {
        return fail("site radius must be within [3, 20] meters");
    }
    if (!std::isfinite(options.site_altitude_limit_meters) ||
        options.site_altitude_limit_meters < options.altitude_meters ||
        options.site_altitude_limit_meters > 10.0) {
        return fail("site altitude limit must cover the route and not exceed 10 meters");
    }
    if (maximumPlannedRadius(options) + 0.5 >
        options.site_radius_meters) {
        return fail("the square route plus 0.5 meter margin exceeds the site radius");
    }
    return true;
}

inline std::vector<Step> makeSquarePlan(const Options& options) {
    std::vector<Step> plan;
    plan.push_back({StepKind::Takeoff, 0, options.altitude_meters});
    plan.push_back({StepKind::Hover, 0, options.hover_seconds});
    plan.push_back({StepKind::Capture, 0, 0.0});
    for (int station = 1; station <= 4; ++station) {
        plan.push_back({StepKind::MoveForward, station, options.leg_meters});
        plan.push_back({StepKind::Hover, station, options.hover_seconds});
        plan.push_back({StepKind::Turn, station, options.turn_degrees});
        plan.push_back({StepKind::Hover, station, options.hover_seconds});
        plan.push_back({StepKind::Capture, station, 0.0});
    }
    plan.push_back({StepKind::Return, 4, options.altitude_meters});
    return plan;
}

inline const char* cameraModeName(CameraMode mode) {
    switch (mode) {
        case CameraMode::Front: return "front";
        case CameraMode::Pod: return "pod";
        case CameraMode::Both: return "both";
    }
    return "unknown";
}

inline const char* stepKindName(StepKind kind) {
    switch (kind) {
        case StepKind::Takeoff: return "takeoff";
        case StepKind::Hover: return "hover";
        case StepKind::Capture: return "capture";
        case StepKind::MoveForward: return "move_forward";
        case StepKind::Turn: return "turn_clockwise";
        case StepKind::Return: return "return_and_land";
    }
    return "unknown";
}

}  // namespace performance
