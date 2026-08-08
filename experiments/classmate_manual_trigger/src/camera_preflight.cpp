#include <iostream>
#include <string>

#include "iking_drone_sdk.h"

int main() {
    iking::drone::Config config;
    config.client_id = "summer_workshop_match_agent_manual_preflight";
    iking::drone::Client client(config);
    if (!client.connect()) {
        std::cerr << "[camera-preflight] SDK connect failed" << std::endl;
        return 1;
    }

    const iking::drone::Result result = client.getPodCapability();
    if (!iking::drone::isOk(result.status)) {
        std::cerr << "[camera-preflight] getPodCapability failed, status="
                  << static_cast<int>(result.status) << std::endl;
        client.disconnect();
        return 2;
    }

    bool front_available = false;
    bool pod_visible_light_available = false;
    const Json::Value& channels = result.msg["streamChannels"];
    if (channels.isArray()) {
        for (Json::ArrayIndex index = 0; index < channels.size(); ++index) {
            const std::string camera_name =
                channels[index].get("camera_name", "").asString();
            const std::string camera_type =
                channels[index].get("camera_type", "").asString();
            std::cout << "[camera-preflight] camera=" << camera_name
                      << " type=" << camera_type << std::endl;
            if (camera_type == "imx586" || camera_type == "fisheye") {
                front_available = true;
            }
            if (camera_type == "light") {
                pod_visible_light_available = true;
            }
        }
    }

    client.disconnect();
    if (!front_available || !pod_visible_light_available) {
        std::cerr << "[camera-preflight] refused: Front and PodVisibleLight "
                     "must both be advertised"
                  << std::endl;
        return 3;
    }

    std::cout << "[camera-preflight] passed" << std::endl;
    return 0;
}
