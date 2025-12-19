#!/usr/bin/env bash
set -euo pipefail

# Simple rebuild helper for Smile
# Usage: ./build.sh [meson compile args...]

cd "$(dirname "$0")"

BUILD_DIR="_build"

# Allow overriding compiler, e.g. CC=clang ./build.sh
export CC="${CC:-cc}"

meson setup "$BUILD_DIR" --wipe
meson compile -C "$BUILD_DIR" "$@"
