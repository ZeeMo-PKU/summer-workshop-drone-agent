#pragma once

#include <optional>
#include <string>

#include "flow_logic.hpp"

namespace flow {

// High-level mission sequencing is SDK-independent so a fake action layer can
// verify order, failure handling, and the single gripper-open invariant.
template <typename Actions>
class SingleRoundMission {
public:
    explicit SingleRoundMission(Actions& actions) : actions_(actions) {}

    bool start() {
        return actions_.closeGripper() &&
               actions_.takeOff() &&
               actions_.flyToSceneB();
    }

    std::optional<PhysicalZone> observeQuestion(
        const std::string& question) {
        if (!actions_.captureScenePhoto()) return std::nullopt;
        const auto answer = actions_.recognizeAnswer(question);
        if (!answer) return std::nullopt;
        return fixedZoneForAnswer(*answer);
    }

    bool deliverAndLand(PhysicalZone target) {
        if (!actions_.flyToZone(PhysicalZone::Zone2)) return false;
        if (!actions_.setAnswerCameraPose()) return false;
        if (!actions_.captureAnswerLayoutPhoto()) return false;
        if (target != PhysicalZone::Zone2 && !actions_.flyToZone(target)) {
            return false;
        }
        if (!actions_.openGripperOnce()) return false;
        if (!actions_.returnToStart()) return false;
        return actions_.returnHome("single round completed");
    }

    bool emergencyReturn(const std::string& reason) {
        return actions_.returnHome(reason);
    }

private:
    Actions& actions_;
};

}  // namespace flow
