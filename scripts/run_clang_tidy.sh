#!/usr/bin/env sh
set -eu

build_dir="${1:-build}"
source_dir="${MESON_SOURCE_ROOT:-.}"

if [ ! -f "$build_dir/compile_commands.json" ]; then
    echo "missing compile database: $build_dir/compile_commands.json" >&2
    echo "run: meson setup $build_dir" >&2
    exit 2
fi

clang-tidy --quiet -p "$build_dir" \
    "$source_dir"/src/*.c \
    "$source_dir"/tests/unit/*.c \
    "$source_dir"/tests/benchmarks/*.c
