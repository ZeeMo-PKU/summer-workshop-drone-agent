#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mission_sequence.hpp"

namespace {

struct FakeActions {
    std::vector<std::string> commands;
    std::optional<flow::Answer> answer = flow::Answer::B;
    std::string fail_at;
    int open_count = 0;

    bool call(std::string name) {
        commands.push_back(name);
        return fail_at != name;
    }

    bool closeGripper() { return call("gripperClose"); }
    bool takeOff() { return call("takeOff"); }
    bool flyToSceneB() { return call("setPosition(scene-B)"); }
    bool captureScenePhoto() { return call("capture(scene-B)"); }
    std::optional<flow::Answer> recognizeAnswer(const std::string&) {
        commands.push_back("recognize");
        return answer;
    }
    bool flyToZone(flow::PhysicalZone zone) {
        return call(std::string("setPosition(") + flow::zoneName(zone) + ")");
    }
    bool setAnswerCameraPose() { return call("gimbalDown"); }
    bool captureAnswerLayoutPhoto() { return call("capture(answer-layout)"); }
    bool openGripperOnce() {
        ++open_count;
        return call("gripperOpen");
    }
    bool returnToStart() { return call("setPosition(start)"); }
    bool returnHome(const std::string&) { return call("returnToHome"); }
};

void assertCommands(const std::vector<std::string>& actual,
                    std::initializer_list<const char*> expected) {
    assert(actual.size() == expected.size());
    size_t index = 0;
    for (const char* command : expected) {
        assert(actual[index++] == command);
    }
}

}  // namespace

int main() {
    FakeActions normal;
    flow::SingleRoundMission<FakeActions> mission(normal);
    assert(mission.start());
    const auto target = mission.observeQuestion("question");
    assert(target == flow::PhysicalZone::Zone2);
    assert(mission.deliverAndLand(*target));
    assert(normal.open_count == 1);
    assertCommands(normal.commands, {
        "gripperClose", "takeOff", "setPosition(scene-B)",
        "capture(scene-B)", "recognize", "setPosition(physical zone 2)",
        "gimbalDown", "capture(answer-layout)", "gripperOpen",
        "setPosition(start)", "returnToHome"});

    FakeActions answer_a;
    answer_a.answer = flow::Answer::A;
    flow::SingleRoundMission<FakeActions> mission_a(answer_a);
    assert(mission_a.start());
    const auto zone_a = mission_a.observeQuestion("question");
    assert(zone_a == flow::PhysicalZone::Zone1);
    assert(mission_a.deliverAndLand(*zone_a));
    assert(answer_a.open_count == 1);
    assert(answer_a.commands[8] == "setPosition(physical zone 1)");

    FakeActions answer_c;
    answer_c.answer = flow::Answer::C;
    flow::SingleRoundMission<FakeActions> mission_c(answer_c);
    assert(mission_c.start());
    const auto zone_c = mission_c.observeQuestion("question");
    assert(zone_c == flow::PhysicalZone::Zone3);
    assert(mission_c.deliverAndLand(*zone_c));
    assert(answer_c.open_count == 1);
    assert(answer_c.commands[8] == "setPosition(physical zone 3)");

    FakeActions invalid;
    invalid.answer = std::nullopt;
    flow::SingleRoundMission<FakeActions> invalid_mission(invalid);
    assert(invalid_mission.start());
    assert(!invalid_mission.observeQuestion("question"));
    assert(invalid.open_count == 0);

    FakeActions camera_failure;
    camera_failure.fail_at = "capture(answer-layout)";
    flow::SingleRoundMission<FakeActions> failed_mission(camera_failure);
    assert(failed_mission.start());
    const auto failed_target = failed_mission.observeQuestion("question");
    assert(failed_target);
    assert(!failed_mission.deliverAndLand(*failed_target));
    assert(camera_failure.open_count == 0);
    assert(failed_mission.emergencyReturn("camera failure"));
    assert(camera_failure.commands.back() == "returnToHome");

    return 0;
}
