#!/bin/sh
# usage: run-tests.sh <spike-a build dir> <knurbs build dir>  - runs the repo's Vulkan tests built by a-build/
b="$1"; obs="$2"
for t in tracker exports device priority readback upload shader draw depth effects swapchain; do
	log="$b/out_$t.log"
	timeout 600 "$b/test_vulkan_$t" > "$log" 2>&1
	rc=$?
	printf '%-10s rc=%-3s %3s ok %3s failed %3s skipped\n' "$t" "$rc" \
		"$(grep -cE '^\[       OK' "$log")" "$(grep -cE '^\[  FAILED  \] [a-z]' "$log")" \
		"$(grep -cE '^\[  SKIPPED \] [a-z]' "$log")"
	grep -B3 '^\[  FAILED  \] [a-z]' "$log" | grep -E 'ERROR|absent' | sed 's/^/           /'
done
timeout 600 "$b/test_vulkan_video" render "$obs/libobs-vulkan/liblibobs-vulkan" vulkan "$b/video_vulkan.bin" > "$b/out_video.log" 2>&1
printf '%-10s rc=%-3s %s\n' video $? "$(grep -E '^\[  PASSED' "$b/out_video.log")"
