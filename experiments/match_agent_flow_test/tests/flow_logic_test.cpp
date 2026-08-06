#include <cassert>
#include <iostream>
#include <limits>

#include "flow_logic.hpp"

int main() {
    using namespace flow;

    assert(parseAnswerToken("A") == Answer::A);
    assert(parseAnswerToken(" B\n") == Answer::B);
    assert(parseAnswerToken("C\r\n") == Answer::C);
    assert(!parseAnswerToken("answer A"));
    assert(!parseAnswerToken(""));
    assert(!parseAnswerToken("D"));
    assert(!parseAnswerToken("a"));

    assert(parseSimulationOracleExpected(
               "question [SIM_ORACLE expected=A slotA=C slotB=A slotC=B]") ==
           Answer::A);
    assert(parseSimulationOracleExpected(
               "[SIM_ORACLE question=Q expected=C slotA=B]") == Answer::C);
    assert(!parseSimulationOracleExpected("expected=B"));
    assert(!parseSimulationOracleExpected("[SIM_ORACLE expected=AB]"));
    assert(!parseSimulationOracleExpected("[SIM_ORACLE expected=D]"));

    const auto identity = parseAnswerLayoutToken("A=1,B=2,C=3");
    assert(identity);
    assert(zoneForAnswer(*identity, Answer::A) == PhysicalZone::Zone1);
    assert(zoneForAnswer(*identity, Answer::B) == PhysicalZone::Zone2);
    assert(zoneForAnswer(*identity, Answer::C) == PhysicalZone::Zone3);
    assert(answerLayoutName(*identity) == "A=1,B=2,C=3");

    const auto shuffled = parseAnswerLayoutToken(" C=1, A=3, B=2\n");
    assert(shuffled);
    assert(zoneForAnswer(*shuffled, Answer::A) == PhysicalZone::Zone3);
    assert(zoneForAnswer(*shuffled, Answer::B) == PhysicalZone::Zone2);
    assert(zoneForAnswer(*shuffled, Answer::C) == PhysicalZone::Zone1);
    assert(!parseAnswerLayoutToken("A=1,B=1,C=3"));
    assert(!parseAnswerLayoutToken("A=1,B=2"));
    assert(!parseAnswerLayoutToken("A=1,B=2,C=4"));
    assert(!parseAnswerLayoutToken("A=1,B=2,C=3,INVALID"));

    const auto oracle_layout = parseSimulationOracleLayout(
        "question [SIM_ORACLE expected=A slotA=B slotB=A slotC=C]");
    assert(oracle_layout);
    assert(answerLayoutName(*oracle_layout) == "A=2,B=1,C=3");
    assert(!parseSimulationOracleLayout(
        "[SIM_ORACLE expected=A slotA=A slotB=A slotC=C]"));

    assert(returnActionForMode(ReturnMode::Standby) ==
           ReturnAction::Complete);
    assert(returnActionForMode(ReturnMode::Landing) == ReturnAction::Wait);
    assert(returnActionForMode(ReturnMode::Takeoff) == ReturnAction::Wait);
    assert(returnActionForMode(ReturnMode::Unknown) == ReturnAction::Wait);
    assert(returnActionForMode(ReturnMode::Position) ==
           ReturnAction::SendCommand);
    assert(returnActionForMode(ReturnMode::Mission) ==
           ReturnAction::SendCommand);

    const GroundTelemetryCheck ground_ready{
        true, true, true, false, true, 0.04};
    assert(isGroundReady(ground_ready, 0.10));
    auto unsafe_ground = ground_ready;
    unsafe_ground.valid = false;
    assert(!isGroundReady(unsafe_ground, 0.10));
    unsafe_ground = ground_ready;
    unsafe_ground.fresh = false;
    assert(!isGroundReady(unsafe_ground, 0.10));
    unsafe_ground = ground_ready;
    unsafe_ground.flight_state_valid = false;
    assert(!isGroundReady(unsafe_ground, 0.10));
    unsafe_ground = ground_ready;
    unsafe_ground.armed = true;
    assert(!isGroundReady(unsafe_ground, 0.10));
    unsafe_ground = ground_ready;
    unsafe_ground.sdk_mode = false;
    assert(!isGroundReady(unsafe_ground, 0.10));
    unsafe_ground = ground_ready;
    unsafe_ground.altitude = 0.11;
    assert(!isGroundReady(unsafe_ground, 0.10));
    unsafe_ground.altitude = std::numeric_limits<double>::quiet_NaN();
    assert(!isGroundReady(unsafe_ground, 0.10));

    assert(isAltitudeWithinEnvelope(1.57, -0.10, 2.50));
    assert(isAltitudeWithinEnvelope(2.50, -0.10, 2.50));
    assert(!isAltitudeWithinEnvelope(2.51, -0.10, 2.50));
    assert(!isAltitudeWithinEnvelope(-0.11, -0.10, 2.50));
    assert(!isAltitudeWithinEnvelope(
        std::numeric_limits<double>::quiet_NaN(), -0.10, 2.50));

    ResidentMatchStateMachine state;
    assert(state.phase() == Phase::Ready);
    assert(state.handle(EventKind::NextRoundStarted) == EventAction::Ignore);
    assert(state.handle(EventKind::Question) == EventAction::Ignore);
    assert(state.handle(EventKind::MatchStarted) == EventAction::StartRound);
    assert(state.roundIndex() == 1);
    assert(state.currentScene() == SceneZone::B);
    assert(state.matchActive());
    assert(state.handle(EventKind::MatchStarted) == EventAction::Ignore);
    state.markSceneReady();
    assert(state.handle(EventKind::SceneTriggerSucceeded) ==
           EventAction::AwaitQuestion);
    assert(state.handle(EventKind::SceneTriggerSucceeded) == EventAction::Ignore);
    assert(state.handle(EventKind::Question) == EventAction::ExecuteAnswer);
    state.markRoundCompleted();

    assert(state.handle(EventKind::NextRoundStarted) == EventAction::StartRound);
    assert(state.roundIndex() == 2);
    assert(state.currentScene() == SceneZone::A);
    assert(state.handle(EventKind::NextRoundStarted) == EventAction::Ignore);
    state.markSceneReady();
    assert(state.handle(EventKind::SceneTriggerSucceeded) ==
           EventAction::AwaitQuestion);
    assert(state.handle(EventKind::Question) == EventAction::ExecuteAnswer);
    state.markRoundCompleted();

    assert(state.handle(EventKind::NextRoundStarted) == EventAction::StartRound);
    assert(state.roundIndex() == 3);
    assert(state.currentScene() == SceneZone::B);
    state.markRoundCompleted();

    // A new MATCH_STARTED always begins a new match at round 1 / scene B.
    assert(state.handle(EventKind::MatchStarted) == EventAction::StartRound);
    assert(state.roundIndex() == 1);
    assert(state.currentScene() == SceneZone::B);
    state.markRoundCompleted();
    assert(state.handle(EventKind::MatchFinished) == EventAction::EndMatch);
    assert(state.phase() == Phase::Ready);
    assert(!state.matchActive());
    assert(state.handle(EventKind::MatchStarted) == EventAction::StartRound);

    ResidentMatchStateMachine reordered;
    assert(reordered.handle(EventKind::MatchStarted) ==
           EventAction::StartRound);
    reordered.markSceneReady();
    assert(reordered.handle(EventKind::Question) ==
           EventAction::CacheQuestion);
    assert(reordered.handle(EventKind::Question) == EventAction::Ignore);
    assert(reordered.handle(EventKind::SceneTriggerSucceeded) ==
           EventAction::ExecuteCachedQuestion);
    assert(reordered.handle(EventKind::Question) == EventAction::Ignore);
    reordered.markRoundCompleted();
    assert(reordered.handle(EventKind::NextRoundStarted) ==
           EventAction::StartRound);
    reordered.markSceneReady();
    assert(reordered.handle(EventKind::SceneTriggerSucceeded) ==
           EventAction::AwaitQuestion);

    ResidentMatchStateMachine emergency;
    assert(emergency.handle(EventKind::MatchStarted) == EventAction::StartRound);
    assert(emergency.handle(EventKind::SafetyLineViolation) ==
           EventAction::EmergencyReturn);
    emergency.markFaulted();
    assert(emergency.handle(EventKind::NextRoundStarted) == EventAction::Ignore);
    assert(emergency.handle(EventKind::MatchStarted) == EventAction::Ignore);
    assert(emergency.handle(EventKind::MatchFinished) == EventAction::EndMatch);
    assert(emergency.handle(EventKind::MatchStarted) == EventAction::StartRound);

    std::cout << "flow_logic_test: PASS\n";
    return 0;
}
