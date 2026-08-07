#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PKG_CONFIG_PATH="/opt/iking/drone_sdk/share/iking_drone_sdk:/usr/local/lib/pkgconfig:/usr/lib/aarch64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

cmake -S "$root_dir" -B "$root_dir/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$root_dir/build" --parallel 2
(
    cd "$root_dir/build"
    ctest --output-on-failure
)
