#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$root_dir/build/match_flow"

case "${1:-}" in
    first)
        signal="USR1"
        event="MATCH_STARTED"
        ;;
    next)
        signal="USR2"
        event="NEXT_ROUND_STARTED"
        ;;
    *)
        echo "usage: $0 first|next" >&2
        exit 2
        ;;
esac

pids=()
for process_dir in /proc/[0-9]*; do
    pid="${process_dir##*/}"
    executable="$(readlink -f "$process_dir/exe" 2>/dev/null || true)"
    [[ "$executable" == "$binary" ]] || continue
    command_line="$(tr '\0' ' ' < "$process_dir/cmdline" 2>/dev/null || true)"
    if [[ " $command_line " == *" --execute "* ]]; then
        pids+=("$pid")
    fi
done

if [[ "${#pids[@]}" -eq 0 ]]; then
    echo "[manual-control] Codex competition process is not running" >&2
    exit 3
fi
if [[ "${#pids[@]}" -ne 1 ]]; then
    echo "[manual-control] expected one Codex competition process, found ${#pids[@]}" >&2
    printf '%s\n' "${pids[@]}" >&2
    exit 4
fi

signal_mask="$(awk '$1 == "SigCgt:" {print $2}' "/proc/${pids[0]}/status")"
caught_signals=$((16#$signal_mask))
usr1_bit=$((1 << (10 - 1)))
usr2_bit=$((1 << (12 - 1)))
if (( (caught_signals & usr1_bit) == 0 ||
      (caught_signals & usr2_bit) == 0 )); then
    echo "[manual-control] running process does not support manual round signals" >&2
    exit 5
fi

kill -"$signal" "${pids[0]}"
echo "[manual-control] sent $event to PID ${pids[0]}"
