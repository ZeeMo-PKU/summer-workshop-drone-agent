#include <iostream>
#include <string>

#include "recognize_image.hpp"

int main(int argc, char** argv) {
    const std::string img =
        (argc > 1) ? argv[1]
                   : "/opt/iking/match_agent/captures/round_1_scene_B.jpg";
    const std::string prompt =
        (argc > 2) ? argv[2] : std::string(recognize::kDefaultPrompt);
    std::cout << "[test] recognizing " << img << std::endl;
    const std::string r = recognize::recognizeImage(img, prompt, 15000);
    std::cout << "[test] result empty=" << r.empty() << std::endl;
    if (!r.empty()) std::cout << "[test] answer=" << r << std::endl;
    return 0;
}
