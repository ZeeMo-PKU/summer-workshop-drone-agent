#include <iostream>
#include <string>

#include "recognize_image.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " IMAGE [PROMPT]\n";
        return 2;
    }
    const std::string img = argv[1];
    const std::string prompt =
        (argc > 2) ? argv[2] : std::string(recognize::kDefaultPrompt);
    std::cout << "[test] recognizing " << img << std::endl;
    const std::string r = recognize::recognizeImage(img, prompt, 15000);
    std::cout << "[test] result empty=" << r.empty() << std::endl;
    if (!r.empty()) std::cout << "[test] answer=" << r << std::endl;
    return 0;
}
