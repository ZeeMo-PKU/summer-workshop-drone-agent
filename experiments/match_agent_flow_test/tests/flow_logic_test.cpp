#include <cassert>
#include <iostream>

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

    assert(fixedZoneForAnswer(Answer::A) == PhysicalZone::Zone1);
    assert(fixedZoneForAnswer(Answer::B) == PhysicalZone::Zone2);
    assert(fixedZoneForAnswer(Answer::C) == PhysicalZone::Zone3);

    SingleRoundStateMachine state;
    assert(state.handle(EventKind::Question) == EventAction::Ignore);
    assert(state.handle(EventKind::MatchStarted) == EventAction::StartMission);
    assert(state.handle(EventKind::MatchStarted) == EventAction::Ignore);
    state.markSceneReady();
    assert(state.handle(EventKind::SceneTriggerSucceeded) ==
           EventAction::AwaitQuestion);
    assert(state.handle(EventKind::SceneTriggerSucceeded) == EventAction::Ignore);
    assert(state.handle(EventKind::Question) == EventAction::ExecuteAnswer);
    state.markReturning();
    state.markCompleted();
    assert(state.handle(EventKind::MatchFinished) == EventAction::Ignore);

    SingleRoundStateMachine reordered;
    assert(reordered.handle(EventKind::MatchStarted) ==
           EventAction::StartMission);
    reordered.markSceneReady();
    assert(reordered.handle(EventKind::Question) ==
           EventAction::CacheQuestion);
    assert(reordered.handle(EventKind::Question) == EventAction::Ignore);
    assert(reordered.handle(EventKind::SceneTriggerSucceeded) ==
           EventAction::ExecuteCachedQuestion);
    assert(reordered.handle(EventKind::Question) == EventAction::Ignore);

    SingleRoundStateMachine emergency;
    assert(emergency.handle(EventKind::MatchStarted) == EventAction::StartMission);
    assert(emergency.handle(EventKind::SafetyLineViolation) ==
           EventAction::EmergencyReturn);
    emergency.markAborted();

    std::cout << "flow_logic_test: PASS\n";
    return 0;
}
