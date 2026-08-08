#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PKG_CONFIG_PATH="/opt/iking/drone_sdk/share/iking_drone_sdk:/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

source_tree_digest() {
    {
        printf '%s\0' "$root_dir/CMakeLists.txt"
        find "$root_dir/src" "$root_dir/scripts" "$root_dir/tests" \
            -type f -print0 | sort -z
    } | xargs -0 sha256sum | sha256sum | awk '{print $1}'
}

source_digest_before="$(source_tree_digest)"
cmake -S "$root_dir" -B "$root_dir/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$root_dir/build" --parallel 2
(
    cd "$root_dir/build"
    ctest --output-on-failure
)
source_digest_after="$(source_tree_digest)"

if [[ "$source_digest_before" != "$source_digest_after" ]]; then
    echo "[build] source changed during build; rebuild required" >&2
    exit 1
fi

printf '%s\n' "$source_digest_after" > "$root_dir/build/source-tree.sha256"
