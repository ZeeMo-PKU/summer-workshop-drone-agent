#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$root_dir/build/qwen_pair_probe"

if [[ $# -ne 3 ]]; then
    echo "usage: $0 SCENE.jpg LAYOUT.jpg QUESTION" >&2
    exit 2
fi
if [[ ! -x "$binary" ]]; then
    echo "[qwen-probe] binary is missing; run scripts/build.sh first" >&2
    exit 2
fi
if [[ ! -r "$root_dir/.secrets.env" ]]; then
    echo "[qwen-probe] missing $root_dir/.secrets.env" >&2
    exit 2
fi

# shellcheck disable=SC1091
source "$root_dir/.secrets.env"
if [[ -r "$root_dir/.vision.env" ]]; then
    # shellcheck disable=SC1091
    source "$root_dir/.vision.env"
fi
if [[ -n "${VISION_API_URL:-}${VISION_MODEL:-}" ]]; then
    if [[ -z "${VISION_API_KEY:-}" &&
          ! -r "${VISION_API_KEY_FILE:-/nonexistent}" ]]; then
        echo "[qwen-probe] configured vision provider has no credentials" >&2
        exit 2
    fi
elif [[ -z "${VISION_API_KEY:-${DASHSCOPE_API_KEY:-}}" &&
        ! -r "${VISION_API_KEY_FILE:-/nonexistent}" ]]; then
    echo "[qwen-probe] VISION_API_KEY is not configured" >&2
    exit 2
fi

exec "$binary" "$@"
