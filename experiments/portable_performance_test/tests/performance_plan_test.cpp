#include <cassert>
#include <string>

#include "performance_plan.hpp"

int main() {
    performance::Options options;
    std::string error;
    assert(performance::isValid(options, &error));
    assert(performance::maximumPlannedRadius(options) > 4.24);
    assert(performance::maximumPlannedRadius(options) < 4.25);

    const auto plan = performance::makeSquarePlan(options);
    assert(plan.size() == 24);
    assert(plan.front().kind == performance::StepKind::Takeoff);
    assert(plan.back().kind == performance::StepKind::Return);

    int moves = 0;
    int turns = 0;
    int captures = 0;
    for (const auto& step : plan) {
        if (step.kind == performance::StepKind::MoveForward) ++moves;
        if (step.kind == performance::StepKind::Turn) ++turns;
        if (step.kind == performance::StepKind::Capture) ++captures;
    }
    assert(moves == 4);
    assert(turns == 4);
    assert(captures == 5);

    auto invalid = options;
    invalid.altitude_meters = 10.1;
    assert(!performance::isValid(invalid, &error));
    invalid = options;
    invalid.site_radius_meters = 4.5;
    assert(!performance::isValid(invalid, &error));
    invalid = options;
    invalid.speed_meters_per_second = 2.1;
    assert(!performance::isValid(invalid, &error));
    return 0;
}
