#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace flow {

enum class Answer {
    A,
    B,
    C,
};

enum class PhysicalZone {
    Zone1,
    Zone2,
    Zone3,
};

inline std::optional<Answer> parseAnswerToken(std::string text) {
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    text.erase(std::find_if(text.rbegin(), text.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), text.end());

    if (text.size() != 1) return std::nullopt;
    switch (text[0]) {
        case 'A': return Answer::A;
        case 'B': return Answer::B;
        case 'C': return Answer::C;
        default: return std::nullopt;
    }
}

inline PhysicalZone fixedZoneForAnswer(Answer answer) {
    switch (answer) {
        case Answer::A: return PhysicalZone::Zone1;
        case Answer::B: return PhysicalZone::Zone2;
        case Answer::C: return PhysicalZone::Zone3;
    }
    return PhysicalZone::Zone1;
}

inline const char* answerName(Answer answer) {
    switch (answer) {
        case Answer::A: return "A";
        case Answer::B: return "B";
        case Answer::C: return "C";
    }
    return "unknown";
}

inline const char* zoneName(PhysicalZone zone) {
    switch (zone) {
        case PhysicalZone::Zone1: return "physical zone 1";
        case PhysicalZone::Zone2: return "physical zone 2";
        case PhysicalZone::Zone3: return "physical zone 3";
    }
    return "unknown physical zone";
}

enum class Phase {
    Idle,
    TakingOff,
    AwaitingSceneTrigger,
    AwaitingQuestion,
    ExecutingAnswer,
    Returning,
    Completed,
    Aborted,
};

enum class EventKind {
    MatchStarted,
    SceneTriggerSucceeded,
    Question,
    MatchFinished,
    SafetyLineViolation,
    NextRoundStarted,
    Other,
};

enum class EventAction {
    Ignore,
    StartMission,
    AwaitQuestion,
    ExecuteAnswer,
    EmergencyReturn,
};

class SingleRoundStateMachine {
public:
    EventAction handle(EventKind event) {
        if (event == EventKind::MatchFinished ||
            event == EventKind::SafetyLineViolation) {
            if (phase_ == Phase::Completed || phase_ == Phase::Aborted) {
                return EventAction::Ignore;
            }
            phase_ = Phase::Returning;
            return EventAction::EmergencyReturn;
        }

        switch (event) {
            case EventKind::MatchStarted:
                if (phase_ != Phase::Idle) return EventAction::Ignore;
                phase_ = Phase::TakingOff;
                return EventAction::StartMission;
            case EventKind::SceneTriggerSucceeded:
                if (phase_ != Phase::AwaitingSceneTrigger) {
                    return EventAction::Ignore;
                }
                phase_ = Phase::AwaitingQuestion;
                return EventAction::AwaitQuestion;
            case EventKind::Question:
                if (phase_ != Phase::AwaitingQuestion) {
                    return EventAction::Ignore;
                }
                phase_ = Phase::ExecutingAnswer;
                return EventAction::ExecuteAnswer;
            case EventKind::NextRoundStarted:
            case EventKind::Other:
            case EventKind::MatchFinished:
            case EventKind::SafetyLineViolation:
                return EventAction::Ignore;
        }
        return EventAction::Ignore;
    }

    Phase phase() const { return phase_; }
    void markSceneReady() { phase_ = Phase::AwaitingSceneTrigger; }
    void markReturning() { phase_ = Phase::Returning; }
    void markCompleted() { phase_ = Phase::Completed; }
    void markAborted() { phase_ = Phase::Aborted; }

private:
    Phase phase_ = Phase::Idle;
};

}  // namespace flow
