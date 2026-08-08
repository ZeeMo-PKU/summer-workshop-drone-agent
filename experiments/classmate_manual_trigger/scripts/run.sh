#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="--dry-run"
preflight_confirmed=false

for argument in "$@"; do
    case "$argument" in
        --dry-run) mode="--dry-run" ;;
        --execute) mode="--execute" ;;
        --confirm-preflight) preflight_confirmed=true ;;
        *) echo "unknown argument: $argument" >&2; exit 2 ;;
    esac
done

if [[ "$mode" == "--execute" && "$preflight_confirmed" != true ]]; then
    echo "[launcher] --execute requires --confirm-preflight" >&2
    exit 2
fi

binary="$root_dir/build/match_manual"
if [[ ! -x "$binary" ]]; then
    echo "[launcher] match_manual is not built; run scripts/build.sh first" >&2
    exit 3
fi

check_other_controllers() {
    for process_dir in /proc/[0-9]*; do
        executable="$(readlink -f "$process_dir/exe" 2>/dev/null || true)"
        case "$executable" in
            /opt/iking/match_agent/match|\
            /opt/iking/match_agent_flow_test/build/match_flow|\
            /opt/iking/portable_performance_test/build/portable_performance_test)
                echo "[launcher] another controller is running: $executable" >&2
                return 1
                ;;
        esac
    done
}

check_other_controllers

if [[ "$mode" == "--execute" ]]; then
    secret_file="$root_dir/.secrets/dashscope_api_key"
    if [[ -z "${DASHSCOPE_API_KEY:-}" && -r "$secret_file" ]]; then
        IFS= read -r DASHSCOPE_API_KEY < "$secret_file"
        export DASHSCOPE_API_KEY
    fi
    if [[ -z "${DASHSCOPE_API_KEY:-}" ]]; then
        echo "[launcher] missing DASHSCOPE_API_KEY; use the Windows operator to install it" >&2
        exit 7
    fi

    camera_preflight="$root_dir/build/camera_preflight"
    if [[ ! -x "$camera_preflight" ]]; then
        echo "[launcher] camera_preflight is not built; run scripts/build.sh first" >&2
        exit 3
    fi
    if ! timeout -k 2s 15s "$camera_preflight"; then
        echo "[launcher] camera capability preflight failed" >&2
        exit 6
    fi
fi

check_other_controllers

mkdir -p "$root_dir/runs"
exec 9>"$root_dir/runs/controller.lock"
if ! flock -n 9; then
    echo "[launcher] another manual controller launcher holds the lock" >&2
    exit 4
fi

exec "$binary" "$mode"
