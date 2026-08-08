from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "match_manual.cpp").read_text(encoding="utf-8")
TRIGGER = (ROOT / "scripts" / "trigger-round.sh").read_text(encoding="utf-8")
RUNNER = (ROOT / "scripts" / "run.sh").read_text(encoding="utf-8")
CAMERA_PREFLIGHT = (ROOT / "src" / "camera_preflight.cpp").read_text(
    encoding="utf-8"
)


def require(fragment: str, text: str) -> None:
    if fragment not in text:
        raise AssertionError(f"missing contract fragment: {fragment}")


require("bool execute = false;", SOURCE)
require("std::signal(SIGUSR1, onSignal);", SOURCE)
require("std::signal(SIGUSR2, onSignal);", SOURCE)
require('enqueueManualEvent("MATCH_STARTED")', SOURCE)
require('enqueueManualEvent("NEXT_ROUND_STARTED")', SOURCE)
require("if (!match_started || match_finished || answer_flow_started)", SOURCE)
require('config.client_id = "summer_workshop_match_agent_manual";', SOURCE)
require('/opt/iking/match_agent_manual/captures', SOURCE)
require("execute refused: Front and PodVisibleLight must both be available", SOURCE)
require('first) signal="USR1"; event="MATCH_STARTED"', TRIGGER)
require('next) signal="USR2"; event="NEXT_ROUND_STARTED"', TRIGGER)
require('timeout -k 2s 15s "$camera_preflight"', RUNNER)
if RUNNER.count("check_other_controllers") < 3:
    raise AssertionError("controller check must run before and after camera preflight")
require('camera_type == "light"', CAMERA_PREFLIGHT)
require("Front and PodVisibleLight", CAMERA_PREFLIGHT)
require("g_controller_conflict.store(true)", SOURCE)
require("return command suppressed", SOURCE)

print("manual trigger contract passed")
