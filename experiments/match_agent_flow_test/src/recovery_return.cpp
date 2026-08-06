#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "iking_drone_sdk.h"
#include "flow_logic.hpp"

using namespace std::chrono_literals;

namespace {

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

int main() {
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
    int failed_mode_queries = 0;
    int failed_return_commands = 0;
    int previous_mode = -1;

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
        }

        const flow::ReturnAction action =
            flow::returnActionForMode(returnModeFromSdk(mode));
        if (action == flow::ReturnAction::Complete) {
            std::cout << "[recovery] landing confirmed\n";
            client.disconnect();
            return 0;
        }

        const auto now = std::chrono::steady_clock::now();
        if (action == flow::ReturnAction::SendCommand &&
            now >= next_return_command) {
            if (!ok(client.returnToHome(5000), "returnToHome")) {
                if (++failed_return_commands >= 3) break;
            } else {
                failed_return_commands = 0;
            }
            next_return_command = now + 10s;
        }

        std::this_thread::sleep_for(500ms);
    }

    std::cerr << "[recovery] timed out waiting for STANDBY\n";
    client.disconnect();
    return 1;
}
