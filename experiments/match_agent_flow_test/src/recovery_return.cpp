#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include <json/json.h>

#include "iking_drone_sdk.h"
#include "flow_logic.hpp"
#include "portable_site.hpp"

using namespace std::chrono_literals;

namespace {

constexpr float kReturnAltitude = 7.0f;
constexpr float kReturnYawOffset = 180.0f;

std::optional<portable_site::SiteAnchor> readAnchor(
    const std::string& path) {
    std::ifstream file(path);
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!file || !Json::parseFromStream(builder, file, &root, &errors) ||
        !root["latitude"].isNumeric() ||
        !root["longitude"].isNumeric() ||
        !root["altitude"].isNumeric() ||
        !root["heading_degrees"].isNumeric()) {
        return std::nullopt;
    }
    portable_site::SiteAnchor anchor{
        root["latitude"].asDouble(),
        root["longitude"].asDouble(),
        root["altitude"].asDouble(),
        root["heading_degrees"].asDouble(),
    };
    if (!portable_site::isValidAnchor(anchor)) return std::nullopt;
    return anchor;
}

bool ok(const iking::drone::Result& result, const char* action) {
    std::cout << "[recovery] " << action
              << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) std::cout << " error=" << result.error;
    std::cout << std::endl;
    return iking::drone::isOk(result.status);
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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--anchor") {
        std::cerr << "usage: " << argv[0]
                  << " --anchor /absolute/path/to/portable_site_anchor.json\n";
        return 2;
    }
    const auto anchor = readAnchor(argv[2]);
    if (!anchor) {
        std::cerr << "[recovery] invalid or unreadable portable-site anchor\n";
        return 2;
    }
    const auto return_target = portable_site::targetFromField(
        *anchor, 0.0, 0.0, kReturnAltitude, kReturnYawOffset);

    const char* simulation = std::getenv("IKING_SIMULATION_CONFIRMED");
    if (!simulation || std::string(simulation) != "1") {
        std::cerr << "[recovery] IKING_SIMULATION_CONFIRMED=1 is required\n";
        return 2;
    }

    iking::drone::Config config;
    config.client_id = "summer_workshop_isolated_recovery";
    iking::drone::Client client(config);
    if (!client.connect()) {
        std::cerr << "[recovery] SDK connect failed\n";
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + 180s;
    auto next_return_command = std::chrono::steady_clock::time_point{};
    auto landing_started_at = std::chrono::steady_clock::time_point{};
    auto last_return_command = std::chrono::steady_clock::time_point{};
    int failed_mode_queries = 0;
    int failed_return_commands = 0;
    int previous_mode = -1;
    bool return_command_accepted = false;

    while (std::chrono::steady_clock::now() < deadline) {
        iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
        const auto mode_result = client.getModeStatus(mode, 2000);
        if (!iking::drone::isOk(mode_result.status)) {
            if (++failed_mode_queries >= 3) {
                ok(mode_result, "getModeStatus");
                break;
            }
            std::this_thread::sleep_for(500ms);
            continue;
        }
        failed_mode_queries = 0;

        if (static_cast<int>(mode) != previous_mode) {
            std::cout << "[recovery] mode=" << modeName(mode) << '\n';
            previous_mode = static_cast<int>(mode);
            if (mode == iking::drone::LANDING) {
                landing_started_at = std::chrono::steady_clock::now();
            }
        }

        const flow::ReturnAction action =
            flow::returnActionForMode(returnModeFromSdk(mode));
        if (action == flow::ReturnAction::Complete) {
            std::cout << "[recovery] landing confirmed\n";
            client.disconnect();
            return 0;
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
            const bool accepted = ok(client.returnToAnyPosition(
                                         static_cast<float>(
                                             return_target.longitude),
                                         static_cast<float>(
                                             return_target.latitude),
                                         static_cast<float>(
                                             return_target.altitude),
                                         static_cast<float>(
                                             return_target.yaw_degrees),
                                         5000),
                                     "returnToAnyPosition(start,7.0m)");
            if (!accepted) {
                if (++failed_return_commands >= 3) break;
            } else {
                failed_return_commands = 0;
                return_command_accepted = true;
            }
            last_return_command = now;
            next_return_command = now + (accepted ? 60s : 10s);
        }

        std::this_thread::sleep_for(500ms);
    }

    std::cerr << "[recovery] timed out waiting for STANDBY\n";
    client.disconnect();
    return 1;
}
