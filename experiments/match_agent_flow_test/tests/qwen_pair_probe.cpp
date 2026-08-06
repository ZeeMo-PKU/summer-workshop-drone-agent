#include <cstdlib>
#include <iostream>
#include <string>

#include "qwen_vision.hpp"
#include "recognize_image.hpp"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0]
                  << " SCENE.jpg LAYOUT.jpg QUESTION\n";
        return 2;
    }

    const char* simulation = std::getenv("IKING_SIMULATION_CONFIRMED");
    const char* allow_oracle = std::getenv("IKING_ALLOW_SIM_ORACLE");
    const bool use_oracle =
        simulation && std::string(simulation) == "1" &&
        allow_oracle && std::string(allow_oracle) == "1";

    vision::QwenVision service(
        [](const std::string& image,
           const std::string& prompt,
           int timeout_ms) {
            return recognize::recognizeImage(image, prompt, timeout_ms);
        },
        [](const std::string& line) { std::cout << line << '\n'; },
        use_oracle);

    const std::string question = argv[3];
    const auto answer = service.recognizeSemanticAnswer(argv[1], question);
    const auto layout = service.recognizeAnswerLayout(argv[2], question);
    if (!answer || !layout) {
        std::cerr << "[probe] incomplete result\n";
        return 1;
    }

    const auto target = flow::zoneForAnswer(*layout, *answer);
    std::cout << "[probe] semantic_answer=" << flow::answerName(*answer)
              << " layout=" << flow::answerLayoutName(*layout)
              << " target=" << flow::zoneName(target) << '\n';
    return 0;
}
