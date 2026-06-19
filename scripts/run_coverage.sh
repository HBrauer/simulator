#!/usr/bin/env sh
set -eu

build_dir="${1:-build-coverage}"

if [ ! -f "$build_dir/build.ninja" ]; then
    meson setup "$build_dir" -Db_coverage=true
else
    meson setup "$build_dir" -Db_coverage=true --reconfigure
fi

meson compile -C "$build_dir"
meson test -C "$build_dir"
ninja -C "$build_dir" coverage
