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
    std::optional<flow::AnswerLayout> layout = flow::AnswerLayout{};
    std::string fail_at;
    int open_count = 0;
    bool opened_this_round = false;
    flow::SceneZone scene = flow::SceneZone::B;

    bool call(std::string name) {
        commands.push_back(name);
        return fail_at != name;
    }

    bool beginRound(int round_index, flow::SceneZone next_scene) {
        opened_this_round = false;
        scene = next_scene;
        return call(std::string("beginRound(") + std::to_string(round_index) +
                    "," + flow::sceneName(scene) + ")");
    }
    bool closeGripper() { return call("gripperClose"); }
    bool takeOff() { return call("takeOff"); }
    bool flyToScene(flow::SceneZone target) {
        return call(std::string("setPosition(scene-") +
                    flow::sceneName(target) + ")");
    }
    bool captureScenePhoto() {
        return call(std::string("capture(scene-") +
                    flow::sceneName(scene) + ")");
    }
    std::optional<flow::Answer> recognizeAnswer(const std::string&) {
        commands.push_back("recognize");
        return answer;
    }
    bool flyToZone(flow::PhysicalZone zone) {
        return call(std::string("setPosition(") + flow::zoneName(zone) + ")");
    }
    bool setAnswerCameraPose() { return call("gimbalDown"); }
    bool captureAnswerLayoutPhoto() { return call("capture(answer-layout)"); }
    std::optional<flow::AnswerLayout> recognizeAnswerLayout() {
        commands.push_back("recognize-layout");
        return layout;
    }
    bool restoreGripperClosedForDrop() {
        return call("gripperClose(drop)");
    }
    bool openGripperOnce() {
        if (opened_this_round) return false;
        opened_this_round = true;
        ++open_count;
        return call("gripperOpen");
    }
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
    flow::RoundMission<FakeActions> mission(normal);
    assert(mission.start(1, flow::SceneZone::B));
    const auto answer = mission.observeQuestion("question");
    assert(answer == flow::Answer::B);
    assert(mission.deliverAndLand(*answer));
    assert(normal.open_count == 1);
    assertCommands(normal.commands, {
        "beginRound(1,B)", "gripperClose", "takeOff", "setPosition(scene-B)",
        "capture(scene-B)", "recognize", "setPosition(physical zone 2)",
        "gimbalDown", "capture(answer-layout)", "recognize-layout",
        "gripperClose(drop)", "gripperOpen", "returnToHome"});

    normal.commands.clear();
    assert(mission.start(2, flow::SceneZone::A));
    const auto second_answer = mission.observeQuestion("question-2");
    assert(second_answer == flow::Answer::B);
    assert(mission.deliverAndLand(*second_answer));
    assert(normal.open_count == 2);
    assert(normal.commands.front() == "beginRound(2,A)");
    assert(normal.commands[3] == "setPosition(scene-A)");
    assert(normal.commands[4] == "capture(scene-A)");

    FakeActions answer_a;
    answer_a.answer = flow::Answer::A;
    answer_a.layout = flow::parseAnswerLayoutToken("A=3,B=1,C=2");
    flow::RoundMission<FakeActions> mission_a(answer_a);
    assert(mission_a.start(1, flow::SceneZone::B));
    const auto semantic_a = mission_a.observeQuestion("question");
    assert(semantic_a == flow::Answer::A);
    assert(mission_a.deliverAndLand(*semantic_a));
    assert(answer_a.open_count == 1);
    assert(answer_a.commands[10] == "gripperClose(drop)");
    assert(answer_a.commands[11] == "setPosition(physical zone 3)");

    FakeActions answer_c;
    answer_c.answer = flow::Answer::C;
    answer_c.layout = flow::parseAnswerLayoutToken("A=2,B=3,C=1");
    flow::RoundMission<FakeActions> mission_c(answer_c);
    assert(mission_c.start(1, flow::SceneZone::B));
    const auto semantic_c = mission_c.observeQuestion("question");
    assert(semantic_c == flow::Answer::C);
    assert(mission_c.deliverAndLand(*semantic_c));
    assert(answer_c.open_count == 1);
    assert(answer_c.commands[10] == "gripperClose(drop)");
    assert(answer_c.commands[11] == "setPosition(physical zone 1)");

    FakeActions invalid;
    invalid.answer = std::nullopt;
    flow::RoundMission<FakeActions> invalid_mission(invalid);
    assert(invalid_mission.start(1, flow::SceneZone::B));
    assert(!invalid_mission.observeQuestion("question"));
    assert(invalid.open_count == 0);

    FakeActions camera_failure;
    camera_failure.fail_at = "capture(answer-layout)";
    flow::RoundMission<FakeActions> failed_mission(camera_failure);
    assert(failed_mission.start(1, flow::SceneZone::B));
    const auto failed_target = failed_mission.observeQuestion("question");
    assert(failed_target);
    assert(!failed_mission.deliverAndLand(*failed_target));
    assert(camera_failure.open_count == 0);
    assert(failed_mission.emergencyReturn("camera failure"));
    assert(camera_failure.commands.back() == "returnToHome");

    FakeActions invalid_layout;
    invalid_layout.layout = std::nullopt;
    flow::RoundMission<FakeActions> invalid_layout_mission(invalid_layout);
    assert(invalid_layout_mission.start(1, flow::SceneZone::B));
    const auto recognized = invalid_layout_mission.observeQuestion("question");
    assert(recognized);
    assert(!invalid_layout_mission.deliverAndLand(*recognized));
    assert(invalid_layout.open_count == 0);

    FakeActions gripper_restore_failure;
    gripper_restore_failure.fail_at = "gripperClose(drop)";
    flow::RoundMission<FakeActions> gripper_restore_mission(
        gripper_restore_failure);
    assert(gripper_restore_mission.start(1, flow::SceneZone::B));
    const auto answer_before_restore_failure =
        gripper_restore_mission.observeQuestion("question");
    assert(answer_before_restore_failure);
    assert(!gripper_restore_mission.deliverAndLand(
        *answer_before_restore_failure));
    assert(gripper_restore_failure.open_count == 0);

    return 0;
}
