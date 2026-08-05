#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "iking_drone_sdk.h"

using namespace std::chrono_literals;

namespace {

bool ok(const iking::drone::Result& result, const char* action) {
    std::cout << "[recovery] " << action
              << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) std::cout << " error=" << result.error;
    std::cout << std::endl;
    return iking::drone::isOk(result.status);
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

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!ok(client.getModeStatus(mode, 2000), "getModeStatus")) {
        client.disconnect();
        return 1;
    }
    if (mode == iking::drone::STANDBY) {
        std::cout << "[recovery] already STANDBY\n";
        client.disconnect();
        return 0;
    }

    if (mode != iking::drone::LANDING &&
        !ok(client.returnToHome(5000), "returnToHome")) {
        client.disconnect();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + 180s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ok(client.getModeStatus(mode, 2000), "getModeStatus") &&
            mode == iking::drone::STANDBY) {
            std::cout << "[recovery] landing confirmed\n";
            client.disconnect();
            return 0;
        }
        std::this_thread::sleep_for(1s);
    }

    std::cerr << "[recovery] timed out waiting for STANDBY\n";
    client.disconnect();
    return 1;
}
