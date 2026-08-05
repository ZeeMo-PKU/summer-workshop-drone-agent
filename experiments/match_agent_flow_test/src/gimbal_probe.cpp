#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "iking_drone_sdk.h"

using namespace std::chrono_literals;

namespace {

constexpr auto kChannel = iking::drone::StreamChannelType::PodVisibleLight;

bool ok(const iking::drone::Result& result, const char* action) {
    std::cout << "[gimbal-probe] " << action
              << " status=" << static_cast<int>(result.status);
    if (!result.error.empty()) std::cout << " error=" << result.error;
    std::cout << std::endl;
    return iking::drone::isOk(result.status);
}

bool applyPreset(iking::drone::Client& client, const std::string& preset) {
    if (preset == "center") {
        return ok(client.gimbalCenterSet(), "gimbalCenterSet");
    }
    if (preset == "down") {
        return ok(client.gimbalDownSet(), "gimbalDownSet");
    }
    if (preset == "pitch-negative") {
        return ok(client.gimbalControlAngleSet(0.0f, -90.0f, 0.0f),
                  "gimbalControlAngleSet(0,-90,0)");
    }
    if (preset == "pitch-positive") {
        return ok(client.gimbalControlAngleSet(0.0f, 90.0f, 0.0f),
                  "gimbalControlAngleSet(0,90,0)");
    }
    std::cerr << "[gimbal-probe] unsupported preset: " << preset << std::endl;
    return false;
}

bool saveFrame(iking::drone::Client& client, const std::string& output) {
    iking::drone::FrameView frame;
    const auto acquired = client.acquireFrame(kChannel, frame, 5000);
    if (acquired != iking::drone::StatusCode::Ok) {
        std::cerr << "[gimbal-probe] acquireFrame status="
                  << static_cast<int>(acquired) << std::endl;
        return false;
    }

    cv::Mat bgr;
    if (frame.data && frame.width > 0 && frame.height > 0) {
        if (frame.type == 1) {
            bgr = cv::Mat(frame.height, frame.width, CV_8UC3, frame.data).clone();
        } else if (frame.type == 0) {
            const cv::Mat rgb(frame.height, frame.width, CV_8UC3, frame.data);
            cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        } else if (frame.type == 3) {
            const cv::Mat yuv(frame.height + frame.height / 2,
                              frame.width,
                              CV_8UC1,
                              frame.data);
            cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
        }
    }

    const auto released = client.releaseFrame(kChannel, frame);
    if (released != iking::drone::StatusCode::Ok) {
        std::cerr << "[gimbal-probe] releaseFrame status="
                  << static_cast<int>(released) << std::endl;
    }
    if (bgr.empty()) return false;

    std::filesystem::create_directories(
        std::filesystem::path(output).parent_path());
    return cv::imwrite(output, bgr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " PRESET OUTPUT.jpg\n";
        return 2;
    }
    const char* simulation = std::getenv("IKING_SIMULATION_CONFIRMED");
    if (!simulation || std::string(simulation) != "1") {
        std::cerr << "[gimbal-probe] IKING_SIMULATION_CONFIRMED=1 is required\n";
        return 2;
    }

    iking::drone::Config config;
    config.client_id = "summer_workshop_gimbal_probe";
    iking::drone::Client client(config);
    if (!client.connect()) return 1;

    iking::drone::DRONE_MODE_STATUS_t mode = iking::drone::STANDBY;
    if (!ok(client.getModeStatus(mode, 2000), "getModeStatus") ||
        mode != iking::drone::STANDBY) {
        std::cerr << "[gimbal-probe] STANDBY is required\n";
        client.disconnect();
        return 1;
    }
    if (!ok(client.openFramePool(kChannel), "openFramePool(PodVisibleLight)")) {
        client.disconnect();
        return 1;
    }

    const bool preset_ok = applyPreset(client, argv[1]);
    if (preset_ok) std::this_thread::sleep_for(2s);
    const bool saved = preset_ok && saveFrame(client, argv[2]);

    client.closeFramePool(kChannel);
    client.disconnect();
    std::cout << "[gimbal-probe] saved=" << std::boolalpha << saved
              << " output=" << argv[2] << std::endl;
    return saved ? 0 : 1;
}
