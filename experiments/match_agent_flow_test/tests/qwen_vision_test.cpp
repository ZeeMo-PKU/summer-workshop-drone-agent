#include <cassert>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "qwen_vision.hpp"

namespace {

struct FakeQwen {
    std::vector<std::string> responses;
    std::vector<std::string> prompts;
    size_t next = 0;

    std::string request(const std::string&,
                        const std::string& prompt,
                        int) {
        prompts.push_back(prompt);
        if (next >= responses.size()) return "";
        return responses[next++];
    }
};

}  // namespace

int main() {
    using namespace std::chrono_literals;

    FakeQwen real;
    real.responses = {"INVALID", "B", "A=3,B=1,C=2"};
    std::vector<std::string> logs;
    vision::QwenVision real_vision(
        [&](const std::string& image, const std::string& prompt, int timeout) {
            return real.request(image, prompt, timeout);
        },
        [&](const std::string& line) { logs.push_back(line); },
        false,
        0ms);

    const std::string question =
        "question A/x B/y C/z [SIM_ORACLE expected=A slotA=B slotB=A slotC=C]";
    assert(real_vision.recognizeSemanticAnswer("scene.jpg", question) ==
           flow::Answer::B);
    assert(real.prompts.size() == 2);
    assert(real.prompts.front().find("SIM_ORACLE") == std::string::npos);
    const auto real_layout =
        real_vision.recognizeAnswerLayout("layout.jpg", question);
    assert(real_layout);
    assert(flow::answerLayoutName(*real_layout) == "A=3,B=1,C=2");

    FakeQwen simulation;
    simulation.responses = {"B", "A=1,B=2,C=3"};
    vision::QwenVision simulation_vision(
        [&](const std::string& image, const std::string& prompt, int timeout) {
            return simulation.request(image, prompt, timeout);
        },
        [](const std::string&) {},
        true,
        0ms);

    assert(simulation_vision.recognizeSemanticAnswer("scene.jpg", question) ==
           flow::Answer::A);
    const auto simulation_layout =
        simulation_vision.recognizeAnswerLayout("layout.jpg", question);
    assert(simulation_layout);
    assert(flow::answerLayoutName(*simulation_layout) == "A=2,B=1,C=3");

    FakeQwen invalid;
    invalid.responses = {"A=1,B=1,C=3", "INVALID"};
    vision::QwenVision invalid_vision(
        [&](const std::string& image, const std::string& prompt, int timeout) {
            return invalid.request(image, prompt, timeout);
        },
        [](const std::string&) {},
        false,
        0ms);
    assert(!invalid_vision.recognizeAnswerLayout("layout.jpg", "question"));

    return 0;
}
