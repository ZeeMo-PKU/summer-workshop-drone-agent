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

controller_pattern='(^|[[:space:]])\./(match|match_recognize)([[:space:]]|$)|/opt/iking/match_agent/(match|match_recognize)|/opt/iking/match_agent_flow_test/build/(match_flow|gimbal_probe)'
if pgrep -af "$controller_pattern" >/dev/null; then
    echo "[launcher] another match controller is running; refusing to start" >&2
    pgrep -af "$controller_pattern" >&2 || true
    exit 4
fi

if [[ "$mode" == "execute" ]]; then
    if [[ ! -r "$root_dir/.secrets.env" ]]; then
        echo "[launcher] missing $root_dir/.secrets.env" >&2
        exit 5
    fi
    # shellcheck disable=SC1091
    source "$root_dir/.secrets.env"
    if [[ "${IKING_SIMULATION_CONFIRMED:-}" != "1" ]]; then
        echo "[launcher] simulation has not been explicitly confirmed" >&2
        exit 6
    fi
    if [[ -z "${DASHSCOPE_API_KEY:-}" ]]; then
        echo "[launcher] DASHSCOPE_API_KEY is not configured" >&2
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
forward_signal() {
    if [[ -n "$child_pid" ]]; then
        kill -TERM "$child_pid" 2>/dev/null || true
    fi
}
trap forward_signal INT TERM

set +e
"$binary" "--$mode" --run-id "$run_id" &
child_pid=$!
wait "$child_pid"
result=$?
if kill -0 "$child_pid" 2>/dev/null; then
    wait "$child_pid"
    result=$?
fi
set -e
trap - INT TERM

run_dir="$root_dir/runs/$run_id"
if [[ -d "$run_dir" ]]; then
    sha256sum \
        "$root_dir/src/match_flow.cpp" \
        "$root_dir/src/flow_logic.hpp" \
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
