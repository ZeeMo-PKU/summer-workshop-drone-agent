#pragma once

#include <chrono>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "flow_logic.hpp"

namespace vision {

using Request = std::function<std::string(
    const std::string&, const std::string&, int)>;
using Logger = std::function<void(const std::string&)>;

inline std::string stripSimulationOracle(std::string question) {
    const auto begin = question.find("[SIM_ORACLE ");
    if (begin == std::string::npos) return question;
    const auto end = question.find(']', begin);
    if (end == std::string::npos) return question.substr(0, begin);
    question.erase(begin, end - begin + 1);
    while (!question.empty() && std::isspace(
               static_cast<unsigned char>(question.back()))) {
        question.pop_back();
    }
    return question;
}

inline std::string semanticAnswerPrompt(const std::string& question) {
    return std::string(
        "You are the perception module for a drone competition. Use ONLY "
        "the photographed scene and the question below. Return exactly one "
        "uppercase letter: A, B, or C. Return INVALID if the image does not "
        "contain the referenced competition task image or enough evidence. "
        "An unrelated room, ceiling, people, or empty camera frame is INVALID; "
        "do not infer a zero-count answer from missing task content. "
        "Do not use metadata or hidden answer hints. "
        "No explanation.\n\nQuestion: ") +
        stripSimulationOracle(question);
}

inline std::string answerLayoutPrompt() {
    return
        "The image shows three physical answer zones numbered 1, 2, and 3 "
        "in calibrated left-to-right order. Each zone contains exactly one "
        "label A, B, or C. Return only the mapping from semantic answer to "
        "physical zone in this exact format: A=1,B=2,C=3. Use each number "
        "exactly once. Return INVALID if all three labels are not clearly "
        "visible. No markdown and no explanation.";
}

inline std::string responseForLog(std::string response) {
    if (response.size() > 80) response.resize(80);
    for (char& ch : response) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return response;
}

class QwenVision {
public:
    QwenVision(Request request,
               Logger logger,
               bool use_simulation_oracle,
               std::chrono::milliseconds retry_delay =
                   std::chrono::seconds(2))
        : request_(std::move(request)),
          logger_(std::move(logger)),
          use_simulation_oracle_(use_simulation_oracle),
          retry_delay_(retry_delay) {}

    std::optional<flow::Answer> recognizeSemanticAnswer(
        const std::string& image_path,
        const std::string& question) const {
        const auto qwen = requestParsed<flow::Answer>(
            image_path,
            semanticAnswerPrompt(question),
            "semantic-answer",
            [](const std::string& value) {
                return flow::parseAnswerToken(value);
            });
        if (!use_simulation_oracle_) return qwen;

        const auto oracle = flow::parseSimulationOracleExpected(question);
        if (!oracle) {
            logger_("[vision] simulation oracle answer is missing or invalid");
            return std::nullopt;
        }
        if (!qwen || *qwen != *oracle) {
            logger_(std::string("[vision] semantic mismatch: qwen=") +
                    (qwen ? flow::answerName(*qwen) : "INVALID") +
                    " oracle=" + flow::answerName(*oracle) +
                    "; simulation uses oracle");
        }
        return oracle;
    }

    std::optional<flow::AnswerLayout> recognizeAnswerLayout(
        const std::string& image_path,
        const std::string& question) const {
        const auto qwen = requestParsed<flow::AnswerLayout>(
            image_path,
            answerLayoutPrompt(),
            "answer-layout",
            [](const std::string& value) {
                return flow::parseAnswerLayoutToken(value);
            });
        if (!use_simulation_oracle_) return qwen;

        const auto oracle = flow::parseSimulationOracleLayout(question);
        if (!oracle) {
            logger_("[vision] simulation oracle layout is missing or invalid");
            return std::nullopt;
        }
        if (!qwen || !(*qwen == *oracle)) {
            logger_(std::string("[vision] layout mismatch: qwen=") +
                    (qwen ? flow::answerLayoutName(*qwen) : "INVALID") +
                    " oracle=" + flow::answerLayoutName(*oracle) +
                    "; simulation uses oracle");
        }
        return oracle;
    }

private:
    template <typename Result, typename Parser>
    std::optional<Result> requestParsed(
        const std::string& image_path,
        const std::string& prompt,
        const char* task,
        Parser parser) const {
        for (int attempt = 1; attempt <= 2; ++attempt) {
            const std::string response = request_(image_path, prompt, 30000);
            const auto parsed = parser(response);
            logger_(std::string("[vision] task=") + task +
                    " attempt=" + std::to_string(attempt) +
                    " valid=" + (parsed ? "true" : "false") +
                    " response=\"" + responseForLog(response) + "\"");
            if (parsed) return parsed;
            if (attempt < 2) {
                std::this_thread::sleep_for(retry_delay_);
            }
        }
        return std::nullopt;
    }

    Request request_;
    Logger logger_;
    bool use_simulation_oracle_ = false;
    std::chrono::milliseconds retry_delay_{};
};

}  // namespace vision
