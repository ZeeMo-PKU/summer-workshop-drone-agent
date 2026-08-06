#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

enum class SceneZone {
    A,
    B,
};

enum class ReturnMode {
    Standby,
    Landing,
    Position,
    Mission,
    Takeoff,
    Unknown,
};

enum class ReturnAction {
    Complete,
    Wait,
    SendCommand,
};

enum class RecoveryDecision {
    AlreadyGrounded,
    SendReturnCommand,
    RefuseUnownedFlight,
};

struct GroundTelemetryCheck {
    bool valid = false;
    bool fresh = false;
    bool flight_state_valid = false;
    bool armed = true;
    bool sdk_mode = false;
    double altitude = 0.0;
};

inline bool isGroundReady(const GroundTelemetryCheck& telemetry,
                          double altitude_tolerance) {
    return telemetry.valid && telemetry.fresh &&
           telemetry.flight_state_valid && !telemetry.armed &&
           telemetry.sdk_mode && std::isfinite(telemetry.altitude) &&
           std::abs(telemetry.altitude) <= altitude_tolerance;
}

inline bool isAltitudeWithinEnvelope(double altitude,
                                     double minimum,
                                     double maximum) {
    return std::isfinite(altitude) && altitude >= minimum &&
           altitude <= maximum;
}

inline RecoveryDecision recoveryDecision(bool flight_commanded_by_process,
                                          bool ground_ready) {
    if (flight_commanded_by_process) {
        return RecoveryDecision::SendReturnCommand;
    }
    return ground_ready ? RecoveryDecision::AlreadyGrounded
                        : RecoveryDecision::RefuseUnownedFlight;
}

inline bool shouldRecoverStalledLanding(bool return_command_sent,
                                        double landing_elapsed_seconds,
                                        double command_elapsed_seconds) {
    if (landing_elapsed_seconds < 0.0 || command_elapsed_seconds < 0.0) {
        return false;
    }
    return return_command_sent ? command_elapsed_seconds >= 60.0
                               : landing_elapsed_seconds >= 15.0;
}

inline ReturnAction returnActionForMode(ReturnMode mode) {
    switch (mode) {
        case ReturnMode::Standby: return ReturnAction::Complete;
        case ReturnMode::Landing:
        case ReturnMode::Takeoff:
        case ReturnMode::Unknown:
            return ReturnAction::Wait;
        case ReturnMode::Position:
        case ReturnMode::Mission:
            return ReturnAction::SendCommand;
    }
    return ReturnAction::Wait;
}

struct AnswerLayout {
    PhysicalZone zone_a = PhysicalZone::Zone1;
    PhysicalZone zone_b = PhysicalZone::Zone2;
    PhysicalZone zone_c = PhysicalZone::Zone3;
};

inline bool operator==(const AnswerLayout& lhs, const AnswerLayout& rhs) {
    return lhs.zone_a == rhs.zone_a &&
           lhs.zone_b == rhs.zone_b &&
           lhs.zone_c == rhs.zone_c;
}

inline std::optional<AnswerLayout> parseAnswerLayoutToken(std::string text);

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

inline std::optional<Answer> parseSimulationOracleExpected(
    const std::string& text) {
    constexpr const char* kBlockStart = "[SIM_ORACLE ";
    constexpr const char* kExpected = "expected=";
    const auto block = text.find(kBlockStart);
    if (block == std::string::npos) return std::nullopt;
    const auto block_end = text.find(']', block);
    if (block_end == std::string::npos) return std::nullopt;
    const auto expected = text.find(kExpected, block);
    if (expected == std::string::npos || expected >= block_end) {
        return std::nullopt;
    }

    const auto token = expected + std::char_traits<char>::length(kExpected);
    if (token >= block_end) return std::nullopt;
    if (token + 1 < block_end && !std::isspace(
            static_cast<unsigned char>(text[token + 1]))) {
        return std::nullopt;
    }
    return parseAnswerToken(text.substr(token, 1));
}

inline std::optional<AnswerLayout> parseSimulationOracleLayout(
    const std::string& text) {
    constexpr const char* kBlockStart = "[SIM_ORACLE ";
    const auto block = text.find(kBlockStart);
    if (block == std::string::npos) return std::nullopt;
    const auto block_end = text.find(']', block);
    if (block_end == std::string::npos) return std::nullopt;

    std::array<char, 3> labels{};
    const std::array<std::string, 3> markers = {
        "slotA=", "slotB=", "slotC="};
    for (size_t index = 0; index < markers.size(); ++index) {
        const auto marker = text.find(markers[index], block);
        if (marker == std::string::npos || marker >= block_end) {
            return std::nullopt;
        }
        const auto token = marker + markers[index].size();
        if (token >= block_end || text[token] < 'A' || text[token] > 'C') {
            return std::nullopt;
        }
        labels[index] = text[token];
    }

    std::string mapping;
    for (size_t zone = 0; zone < labels.size(); ++zone) {
        if (!mapping.empty()) mapping += ',';
        mapping += labels[zone];
        mapping += '=';
        mapping += static_cast<char>('1' + zone);
    }
    return parseAnswerLayoutToken(mapping);
}

inline PhysicalZone zoneForAnswer(const AnswerLayout& layout, Answer answer) {
    switch (answer) {
        case Answer::A: return layout.zone_a;
        case Answer::B: return layout.zone_b;
        case Answer::C: return layout.zone_c;
    }
    return PhysicalZone::Zone1;
}

inline std::optional<AnswerLayout> parseAnswerLayoutToken(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), text.end());

    AnswerLayout layout;
    std::array<bool, 3> seen_answers{};
    std::array<bool, 3> seen_zones{};
    size_t start = 0;
    int entry_count = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string entry = text.substr(
            start,
            comma == std::string::npos ? std::string::npos : comma - start);
        if (entry.size() != 3 || entry[1] != '=' ||
            entry[0] < 'A' || entry[0] > 'C' ||
            entry[2] < '1' || entry[2] > '3') {
            return std::nullopt;
        }

        const size_t answer_index = static_cast<size_t>(entry[0] - 'A');
        const size_t zone_index = static_cast<size_t>(entry[2] - '1');
        if (seen_answers[answer_index] || seen_zones[zone_index]) {
            return std::nullopt;
        }
        seen_answers[answer_index] = true;
        seen_zones[zone_index] = true;

        const auto zone = static_cast<PhysicalZone>(zone_index);
        if (entry[0] == 'A') layout.zone_a = zone;
        if (entry[0] == 'B') layout.zone_b = zone;
        if (entry[0] == 'C') layout.zone_c = zone;
        ++entry_count;

        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    if (entry_count != 3) return std::nullopt;
    return layout;
}

inline std::string answerLayoutName(const AnswerLayout& layout) {
    const auto zone_number = [](PhysicalZone zone) {
        return std::to_string(static_cast<int>(zone) + 1);
    };
    return "A=" + zone_number(layout.zone_a) +
           ",B=" + zone_number(layout.zone_b) +
           ",C=" + zone_number(layout.zone_c);
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

inline const char* sceneName(SceneZone scene) {
    return scene == SceneZone::A ? "A" : "B";
}

enum class Phase {
    Ready,
    TakingOff,
    AwaitingSceneTrigger,
    AwaitingQuestion,
    ExecutingAnswer,
    Returning,
    RoundCompleted,
    Faulted,
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
    StartRound,
    AwaitQuestion,
    CacheQuestion,
    ExecuteAnswer,
    ExecuteCachedQuestion,
    EmergencyReturn,
    EndMatch,
};

class ResidentMatchStateMachine {
public:
    EventAction handle(EventKind event) {
        if (event == EventKind::MatchFinished ||
            event == EventKind::SafetyLineViolation) {
            const bool airborne = phase_ != Phase::Ready &&
                                  phase_ != Phase::RoundCompleted &&
                                  phase_ != Phase::Faulted;
            match_active_ = false;
            question_pending_ = false;
            if (!airborne) {
                phase_ = Phase::Ready;
                return EventAction::EndMatch;
            }
            phase_ = Phase::Returning;
            return EventAction::EmergencyReturn;
        }

        switch (event) {
            case EventKind::MatchStarted:
                if (phase_ != Phase::Ready &&
                    phase_ != Phase::RoundCompleted) {
                    return EventAction::Ignore;
                }
                match_active_ = true;
                round_index_ = 1;
                question_pending_ = false;
                phase_ = Phase::TakingOff;
                return EventAction::StartRound;
            case EventKind::NextRoundStarted:
                if (!match_active_ || phase_ != Phase::RoundCompleted) {
                    return EventAction::Ignore;
                }
                ++round_index_;
                question_pending_ = false;
                phase_ = Phase::TakingOff;
                return EventAction::StartRound;
            case EventKind::SceneTriggerSucceeded:
                if (phase_ != Phase::AwaitingSceneTrigger) {
                    return EventAction::Ignore;
                }
                if (question_pending_) {
                    question_pending_ = false;
                    phase_ = Phase::ExecutingAnswer;
                    return EventAction::ExecuteCachedQuestion;
                }
                phase_ = Phase::AwaitingQuestion;
                return EventAction::AwaitQuestion;
            case EventKind::Question:
                if (phase_ == Phase::AwaitingSceneTrigger) {
                    if (question_pending_) return EventAction::Ignore;
                    question_pending_ = true;
                    return EventAction::CacheQuestion;
                }
                if (phase_ != Phase::AwaitingQuestion) {
                    return EventAction::Ignore;
                }
                phase_ = Phase::ExecutingAnswer;
                return EventAction::ExecuteAnswer;
            case EventKind::Other:
            case EventKind::MatchFinished:
            case EventKind::SafetyLineViolation:
                return EventAction::Ignore;
        }
        return EventAction::Ignore;
    }

    Phase phase() const { return phase_; }
    int roundIndex() const { return round_index_; }
    SceneZone currentScene() const {
        return round_index_ % 2 == 0 ? SceneZone::A : SceneZone::B;
    }
    bool matchActive() const { return match_active_; }

    void markSceneReady() { phase_ = Phase::AwaitingSceneTrigger; }
    void markReturning() {
        question_pending_ = false;
        phase_ = Phase::Returning;
    }
    void markRoundCompleted() {
        question_pending_ = false;
        phase_ = Phase::RoundCompleted;
    }
    void markFaulted() {
        question_pending_ = false;
        match_active_ = false;
        phase_ = Phase::Faulted;
    }
    void markReady() {
        question_pending_ = false;
        match_active_ = false;
        phase_ = Phase::Ready;
    }

private:
    Phase phase_ = Phase::Ready;
    int round_index_ = 0;
    bool match_active_ = false;
    bool question_pending_ = false;
};

}  // namespace flow
