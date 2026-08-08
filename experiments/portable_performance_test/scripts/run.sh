#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$root_dir/build/portable_performance_test"
parameter_reader="$root_dir/scripts/read-parameter.py"
status_command="/opt/iking/match_agent/codex/drone-status"
lock_file="/run/lock/portable_performance_test.lock"

mode="dry-run"
environment=""
site_clear=0
battery_ready=0
site_altitude_limit="20"
run_root="$root_dir/runs"
run_id="$(date +%Y%m%d-%H%M%S)"
binary_args=()

while (($#)); do
    case "$1" in
        --execute)
            mode="execute"
            binary_args+=("$1")
            shift
            ;;
        --dry-run)
            mode="dry-run"
            binary_args+=("$1")
            shift
            ;;
        --environment)
            [[ $# -ge 2 ]] || { echo "--environment needs sim or real" >&2; exit 2; }
            environment="$2"
            shift 2
            ;;
        --confirm-site-clear)
            site_clear=1
            shift
            ;;
        --confirm-battery-ready)
            battery_ready=1
            shift
            ;;
        --site-altitude-limit)
            [[ $# -ge 2 ]] || { echo "--site-altitude-limit needs a value" >&2; exit 2; }
            site_altitude_limit="$2"
            binary_args+=("$1" "$2")
            shift 2
            ;;
        --run-root)
            [[ $# -ge 2 ]] || { echo "--run-root needs a path" >&2; exit 2; }
            run_root="$2"
            binary_args+=("$1" "$2")
            shift 2
            ;;
        --run-id)
            [[ $# -ge 2 ]] || { echo "--run-id needs a value" >&2; exit 2; }
            run_id="$2"
            binary_args+=("$1" "$2")
            shift 2
            ;;
        *)
            binary_args+=("$1")
            shift
            ;;
    esac
done

if [[ ! -x "$binary" ]]; then
    echo "[launcher] binary is missing; run scripts/build.sh first" >&2
    exit 2
fi

if [[ ! " ${binary_args[*]} " =~ " --run-id " ]]; then
    binary_args+=(--run-id "$run_id")
fi

finalize_artifacts() {
    local run_dir="$run_root/$run_id"
    if [[ ! -d "$run_dir" ]]; then
        return
    fi
    sha256sum \
        "$root_dir/src/portable_performance_test.cpp" \
        "$root_dir/src/performance_plan.hpp" \
        "$binary" > "$run_dir/source-binary.sha256"
    (
        cd "$run_dir"
        find . -type f ! -name SHA256SUMS -printf '%P\0' |
            sort -z |
            xargs -0 sha256sum > SHA256SUMS
    )
}

if [[ "$mode" == "dry-run" ]]; then
    set +e
    "$binary" "${binary_args[@]}"
    result=$?
    set -e
    finalize_artifacts
    exit "$result"
fi

if [[ "$environment" != "sim" && "$environment" != "real" ]]; then
    echo "[launcher] --execute requires --environment sim or real" >&2
    exit 3
fi
if [[ "$site_clear" != 1 ]]; then
    echo "[launcher] --confirm-site-clear is required" >&2
    exit 3
fi
if [[ "$environment" == "real" && "$battery_ready" != 1 ]]; then
    echo "[launcher] real flight requires --confirm-battery-ready" >&2
    exit 3
fi
if [[ ! -x "$parameter_reader" || ! -x "$status_command" ]]; then
    echo "[launcher] preflight tools are unavailable" >&2
    exit 4
fi

exec 9>"$lock_file"
if ! flock -n 9; then
    echo "[launcher] another performance test owns $lock_file" >&2
    exit 5
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
            /opt/iking/match_agent_flow_test/build/recovery_return|\
            /opt/iking/portable_performance_test/build/portable_performance_test)
                printf '%s %s\n' "$pid" "$executable"
                ;;
        esac
    done
}

if [[ -n "$(controller_processes)" ]]; then
    echo "[launcher] another flight controller is running" >&2
    controller_processes >&2
    exit 6
fi

cfg_json="$($parameter_reader CFG_FLIGHTSIM)"
cfg_value="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["integer"])' <<<"$cfg_json")"
if [[ "$environment" == "sim" && "$cfg_value" != "1" ]] ||
   [[ "$environment" == "real" && "$cfg_value" != "0" ]]; then
    echo "[launcher] CFG_FLIGHTSIM=$cfg_value does not match $environment" >&2
    exit 7
fi

return_json="$($parameter_reader RETURN_HEIGHT)"
return_height="$(python3 -c '
import json
import sys
data = json.load(sys.stdin)
value = data.get("real")
if value is None:
    value = data.get("integer")
print(value)
' <<<"$return_json")"
if ! python3 - "$return_height" "$site_altitude_limit" <<'PY'
import math
import sys
height = float(sys.argv[1])
limit = float(sys.argv[2])
raise SystemExit(0 if math.isfinite(height) and height <= limit else 1)
PY
then
    echo "[launcher] RETURN_HEIGHT=$return_height exceeds site limit $site_altitude_limit" >&2
    exit 8
fi

status_json="$($status_command)"
if ! python3 -c '
import json
import sys
environment = sys.argv[1]
s = json.load(sys.stdin)
position = s.get("position") or {}
relative_altitude = position.get("relative_dock_altitude")
if relative_altitude is None:
    relative_altitude = position.get("altitude", 999)
speed = s.get("speed") or {}
ok = (
    str(s.get("flight_path", "")).startswith("STANDBY")
    and s.get("armed") is False
    and s.get("sdk_mode") is True
    and abs(float(relative_altitude)) <= 0.10
    and abs(float(speed.get("total", 999))) <= 0.10
    and not s.get("control_processes")
)
if environment == "real":
    navigation = s.get("navigation") or {}
    ok = ok and int(navigation.get("rtk_status", 0)) >= 4
    ok = ok and int(navigation.get("satellite_count", 0)) >= 10
raise SystemExit(0 if ok else 1)
' "$environment" <<<"$status_json"
then
    echo "[launcher] ground, mode, process, or navigation preflight failed" >&2
    printf '%s\n' "$status_json" >&2
    exit 9
fi

echo "[launcher] $cfg_json"
echo "[launcher] $return_json"
export IKING_PERFORMANCE_EXECUTION_CONFIRMED=1
export IKING_TEST_ENVIRONMENT="$environment"
export IKING_RETURN_HEIGHT="$return_height"

set +e
"$binary" "${binary_args[@]}"
result=$?
set -e

finalize_artifacts

exit "$result"
