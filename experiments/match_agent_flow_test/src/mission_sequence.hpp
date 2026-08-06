#pragma once

#include <optional>
#include <string>

#include "flow_logic.hpp"

namespace flow {

// One round starts and ends on the ground. The process can reuse this mission
// for every referee round while a separate state machine remains resident.
template <typename Actions>
class RoundMission {
public:
    explicit RoundMission(Actions& actions) : actions_(actions) {}

    bool start(int round_index, SceneZone scene) {
        return actions_.beginRound(round_index, scene) &&
               actions_.closeGripper() &&
               actions_.takeOff() &&
               actions_.flyToScene(scene);
    }

    std::optional<Answer> observeQuestion(
        const std::string& question) {
        if (!actions_.captureScenePhoto()) return std::nullopt;
        return actions_.recognizeAnswer(question);
    }

    bool deliverAndLand(Answer answer) {
        if (!actions_.flyToZone(PhysicalZone::Zone2)) return false;
        if (!actions_.setAnswerCameraPose()) return false;
        if (!actions_.captureAnswerLayoutPhoto()) return false;
        const auto layout = actions_.recognizeAnswerLayout();
        if (!layout) return false;
        if (!actions_.restoreGripperClosedForDrop()) return false;
        const PhysicalZone target = zoneForAnswer(*layout, answer);
        if (target != PhysicalZone::Zone2 && !actions_.flyToZone(target)) {
            return false;
        }
        if (!actions_.openGripperOnce()) return false;
        if (!actions_.returnToStart()) return false;
        return actions_.returnHome("round completed");
    }

    bool emergencyReturn(const std::string& reason) {
        return actions_.returnHome(reason);
    }

private:
    Actions& actions_;
};

}  // namespace flow
