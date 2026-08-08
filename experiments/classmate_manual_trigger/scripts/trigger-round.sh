#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
    first) signal="USR1"; event="MATCH_STARTED" ;;
    next) signal="USR2"; event="NEXT_ROUND_STARTED" ;;
    *) echo "usage: $0 {first|next}" >&2; exit 2 ;;
esac

expected="/opt/iking/match_agent_manual/build/match_manual"
mapfile -t pids < <(
    for process_dir in /proc/[0-9]*; do
        pid="${process_dir##*/}"
        executable="$(readlink -f "$process_dir/exe" 2>/dev/null || true)"
        [[ "$executable" == "$expected" ]] && printf '%s\n' "$pid"
    done
)

if [[ "${#pids[@]}" -eq 0 ]]; then
    echo "[manual-control] match_manual is not running" >&2
    exit 3
fi
if [[ "${#pids[@]}" -ne 1 ]]; then
    echo "[manual-control] expected one match_manual process, found ${#pids[@]}" >&2
    exit 4
fi

kill "-$signal" "${pids[0]}"
printf '[manual-control] sent %s to PID %s\n' "$event" "${pids[0]}"
