#!/bin/sh
# usage: run-all.sh <build dir> <results dir>  - runs spikes B-G and writes one log per spike
b="${1:-build}"; r="${2:-results}"
mkdir -p "$r" "$r/xdg" && chmod 700 "$r/xdg"
run() { name=$1; shift; timeout 120 "$@" > "$r/$name.log" 2>&1; echo "$name: rc=$? $(tail -1 "$r/$name.log")"; }
run b-plane-views "$b/b-plane-views/spike-b-plane-views"
run c-dmabuf "$b/c-dmabuf/spike-c-dmabuf"
run d-sync "$b/d-sync/spike-d-sync"
mkdir -p "$r/e"
run e-cuda-nvenc "$b/e-cuda-nvenc/spike-e-cuda-nvenc" "$r/e"
python3 "$(dirname "$0")/e-cuda-nvenc/psnr.py" "$r/e" | tee -a "$r/e-cuda-nvenc.log"
run f-cross-process "$b/f-cross-process/spike-f-cross-process"
XDG_RUNTIME_DIR="$r/xdg" run g-wayland-fifo "$b/g-wayland/spike-g-wayland" 2
XDG_RUNTIME_DIR="$r/xdg" run g-wayland-mailbox "$b/g-wayland/spike-g-wayland" 1
XDG_RUNTIME_DIR="$r/xdg" run g-wayland-syncobj "$b/g-wayland/spike-g-wayland" --syncobj 2
