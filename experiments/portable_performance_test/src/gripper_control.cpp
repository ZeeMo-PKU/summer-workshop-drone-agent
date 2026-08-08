#include <cstring>
#include <iostream>

#include "iking_drone_sdk.h"

namespace {

constexpr int kRpcTimeoutMs = 5000;

bool ok(const iking::drone::Result& result, const char* action) {
    std::cout << "[sdk] " << action
              << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) std::cout << " error=" << result.error;
    std::cout << '\n';
    return iking::drone::isOk(result.status);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--close") != 0) {
        std::cerr << "usage: " << argv[0] << " --close\n";
        return 2;
    }

    iking::drone::Config config;
    config.client_id = "portable_gripper_control";
    iking::drone::Client client(config);
    if (!client.connect()) {
        std::cerr << "[startup] SDK connect failed\n";
        return 1;
    }

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!ok(client.getModeStatus(mode, 2000), "getModeStatus") ||
        mode != iking::drone::STANDBY) {
        std::cerr << "[safety] gripper action requires STANDBY\n";
        client.disconnect();
        return 1;
    }

    const bool accepted = ok(
        client.gripperClose(kRpcTimeoutMs), "gripperClose");
    client.disconnect();
    std::cout << (accepted ? "[result] gripper close accepted\n"
                           : "[result] gripper close failed\n");
    return accepted ? 0 : 1;
}
