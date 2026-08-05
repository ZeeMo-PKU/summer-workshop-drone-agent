#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$root_dir/build/gimbal_probe"
status_command="/opt/iking/match_agent/codex/drone-status"
simulation_command="$root_dir/scripts/check-simulation.py"
lock_file="/run/lock/match_agent_flow_test.lock"
calibration_id="$(date +%Y%m%d-%H%M%S)"
output_dir="$root_dir/runs/calibration-$calibration_id/captures"

if [[ ! -x "$binary" ]]; then
    echo "[calibrate] binary is missing; run scripts/build.sh first" >&2
    exit 2
fi

exec 9>"$lock_file"
if ! flock -n 9; then
    echo "[calibrate] another isolated flow test owns $lock_file" >&2
    exit 3
fi

controller_pattern='(^|[[:space:]])\./(match|match_test|match_recognize)([[:space:]]|$)|/opt/iking/match_agent/(match|match_test|match_recognize)|/opt/iking/match_agent_flow_test/build/(match_flow|gimbal_probe|recovery_return)'
if pgrep -af "$controller_pattern" >/dev/null; then
    echo "[calibrate] another match controller is running; refusing to continue" >&2
    pgrep -af "$controller_pattern" >&2 || true
    exit 4
fi

if [[ ! -r "$root_dir/.secrets.env" ]]; then
    echo "[calibrate] missing $root_dir/.secrets.env" >&2
    exit 5
fi
# shellcheck disable=SC1091
source "$root_dir/.secrets.env"
if [[ "${IKING_SIMULATION_CONFIRMED:-}" != "1" ]]; then
    echo "[calibrate] simulation has not been explicitly confirmed" >&2
    exit 6
fi
if [[ ! -x "$simulation_command" ]]; then
    echo "[calibrate] simulation parameter checker is unavailable" >&2
    exit 7
fi
if ! simulation_json="$($simulation_command)"; then
    echo "[calibrate] CFG_FLIGHTSIM is not confirmed as simulation" >&2
    exit 7
fi
echo "[calibrate] $simulation_json"

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
    echo "[calibrate] grounded STANDBY state is required" >&2
    printf '%s\n' "$status_json" >&2
    exit 8
fi

mkdir -p "$output_dir"
for preset in center down pitch-negative pitch-positive; do
    "$binary" "$preset" "$output_dir/$preset.jpg"
done

(
    cd "$(dirname "$output_dir")"
    find . -type f ! -name SHA256SUMS -printf '%P\0' |
        sort -z |
        xargs -0 sha256sum > SHA256SUMS
)
echo "[calibrate] inspect $output_dir before choosing IKING_GIMBAL_PRESET"
