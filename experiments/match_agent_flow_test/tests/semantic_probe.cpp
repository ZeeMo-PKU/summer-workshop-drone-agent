#include <cstdlib>
#include <iostream>
#include <string>

#include "qwen_vision.hpp"
#include "recognize_image.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " IMAGE.jpg QUESTION\n";
        return 2;
    }

    vision::QwenVision service(
        [](const std::string& image,
           const std::string& prompt,
           int timeout_ms) {
            return recognize::recognizeImage(image, prompt, timeout_ms);
        },
        [](const std::string& line) { std::cout << line << '\n'; },
        false);

    const auto answer = service.recognizeSemanticAnswer(argv[1], argv[2]);
    if (!answer) {
        std::cerr << "[probe] semantic_answer=INVALID\n";
        return 1;
    }

    std::cout << "[probe] semantic_answer="
              << flow::answerName(*answer) << '\n';
    return 0;
}
