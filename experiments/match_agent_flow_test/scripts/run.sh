#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$root_dir/build/match_flow"
status_command="/opt/iking/match_agent/codex/drone-status"
simulation_command="$root_dir/scripts/check-simulation.py"
gimbal_verification_file="$root_dir/.gimbal-preset-verified"
lock_file="/run/lock/match_agent_flow_test.lock"
run_id="$(date +%Y%m%d-%H%M%S)"
mode="dry-run"

for arg in "$@"; do
    case "$arg" in
        --execute) mode="execute" ;;
        --dry-run) mode="dry-run" ;;
        --help|-h) exec "$binary" --help ;;
        *)
            echo "[launcher] unsupported argument: $arg" >&2
            exit 2
            ;;
    esac
done

if [[ ! -x "$binary" ]]; then
    echo "[launcher] binary is missing; run scripts/build.sh first" >&2
    exit 2
fi

exec 9>"$lock_file"
if ! flock -n 9; then
    echo "[launcher] another isolated flow test owns $lock_file" >&2
    exit 3
fi

controller_processes() {
    local process_dir pid executable
    for process_dir in /proc/[0-9]*; do
        pid="${process_dir##*/}"
        executable="$(readlink -f "$process_dir/exe" 2>/dev/null || true)"
        case "$executable" in
            /opt/iking/match|/opt/iking/match.*|\
            /opt/iking/match_agent/match|/opt/iking/match_agent/match_*|\
            /opt/iking/match_agent_flow_test/build/match_flow|\
            /opt/iking/match_agent_flow_test/build/gimbal_probe|\
            /opt/iking/match_agent_flow_test/build/recovery_return)
                printf '%s %s\n' "$pid" "$executable"
                ;;
        esac
    done
}

if [[ -n "$(controller_processes)" ]]; then
    echo "[launcher] another match controller is running; refusing to start" >&2
    controller_processes >&2
    exit 4
fi

if [[ "$mode" == "execute" ]]; then
    if [[ ! -r "$root_dir/.secrets.env" ]]; then
        echo "[launcher] missing $root_dir/.secrets.env" >&2
        exit 5
    fi
    # shellcheck disable=SC1091
    source "$root_dir/.secrets.env"
    if [[ -r "$root_dir/.vision.env" ]]; then
        # shellcheck disable=SC1091
        source "$root_dir/.vision.env"
    fi
    if [[ "${IKING_SIMULATION_CONFIRMED:-}" != "1" ]]; then
        echo "[launcher] simulation has not been explicitly confirmed" >&2
        exit 6
    fi
    if [[ -n "${VISION_API_URL:-}${VISION_MODEL:-}" ]]; then
        if [[ -z "${VISION_API_KEY:-}" &&
              ! -r "${VISION_API_KEY_FILE:-/nonexistent}" ]]; then
            echo "[launcher] configured vision provider has no credentials" >&2
            exit 7
        fi
    elif [[ -z "${VISION_API_KEY:-${DASHSCOPE_API_KEY:-}}" &&
            ! -r "${VISION_API_KEY_FILE:-/nonexistent}" ]]; then
        echo "[launcher] VISION_API_KEY is not configured" >&2
        exit 7
    fi
    if [[ ! -r "$gimbal_verification_file" ]]; then
        echo "[launcher] no downward gimbal preset has passed visual verification" >&2
        exit 8
    fi
    verified_gimbal_preset="$(tr -d '[:space:]' < "$gimbal_verification_file")"
    if [[ -z "$verified_gimbal_preset" ||
          "$verified_gimbal_preset" != "${IKING_GIMBAL_PRESET:-}" ]]; then
        echo "[launcher] verified gimbal preset does not match IKING_GIMBAL_PRESET" >&2
        exit 8
    fi
    if [[ ! -x "$status_command" ]]; then
        echo "[launcher] drone-status is unavailable" >&2
        exit 8
    fi
    if [[ ! -x "$simulation_command" ]]; then
        echo "[launcher] simulation parameter checker is unavailable" >&2
        exit 8
    fi

    if ! simulation_json="$($simulation_command)"; then
        echo "[launcher] CFG_FLIGHTSIM is not confirmed as simulation" >&2
        exit 9
    fi
    echo "[launcher] $simulation_json"

    status_json="$($status_command)"
    if ! printf '%s' "$status_json" | python3 -c '
import json, sys
s = json.load(sys.stdin)
ok = (
    str(s.get("flight_path", "")).startswith("STANDBY")
    and s.get("armed") is False
    and s.get("sdk_mode") is True
    and abs(float(s.get("position", {}).get("altitude", 999))) <= 0.10
)
raise SystemExit(0 if ok else 1)
'; then
        echo "[launcher] preflight requires STANDBY, armed=false, sdk_mode=true, altitude<=0.10" >&2
        printf '%s\n' "$status_json" >&2
        exit 10
    fi
fi

child_pid=""
watchdog_pid=""
conflict_file="${lock_file}.conflict.$$"
rm -f "$conflict_file"

stop_watchdog() {
    if [[ -n "$watchdog_pid" ]]; then
        kill "$watchdog_pid" 2>/dev/null || true
        wait "$watchdog_pid" 2>/dev/null || true
        watchdog_pid=""
    fi
}

forward_signal() {
    stop_watchdog
    if [[ -n "$child_pid" ]]; then
        kill -TERM "$child_pid" 2>/dev/null || true
    fi
}
trap forward_signal INT TERM

set +e
"$binary" "--$mode" --run-id "$run_id" &
child_pid=$!

(
    while kill -0 "$child_pid" 2>/dev/null; do
        competitors="$(controller_processes | awk -v own="$child_pid" '$1 != own')"
        if [[ -n "$competitors" ]]; then
            echo "[launcher] competing controller appeared; stopping isolated flow" >&2
            printf '%s\n' "$competitors" >&2
            printf '%s\n' "$competitors" > "$conflict_file"
            kill -TERM "$child_pid" 2>/dev/null || true
            exit 0
        fi
        sleep 1
    done
) &
watchdog_pid=$!

wait "$child_pid"
result=$?
if kill -0 "$child_pid" 2>/dev/null; then
    wait "$child_pid"
    result=$?
fi
stop_watchdog
if [[ -s "$conflict_file" ]]; then
    result=11
fi
rm -f "$conflict_file"
set -e
trap - INT TERM

run_dir="$root_dir/runs/$run_id"
if [[ -d "$run_dir" ]]; then
    sha256sum \
        "$root_dir/src/match_flow.cpp" \
        "$root_dir/src/flow_logic.hpp" \
        "$root_dir/src/mission_sequence.hpp" \
        "$root_dir/src/qwen_vision.hpp" \
        "$root_dir/src/recognize_image.hpp" \
        "$binary" > "$run_dir/source-binary.sha256"
    (
        cd "$run_dir"
        find . -type f ! -name SHA256SUMS -printf '%P\0' |
            sort -z |
            xargs -0 sha256sum > SHA256SUMS
    )
fi

exit "$result"
