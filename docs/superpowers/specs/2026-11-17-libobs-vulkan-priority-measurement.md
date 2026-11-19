# libobs-vulkan: queue priority measurement

Milestone 5 of the design. Question: does OBS's GPU work on the Vulkan backend
(default `LOW` global priority, no Administrator) hurt a GPU-bound foreground
app less than the D3D11 backend does?

**Result: the data do not show a benefit.** Every OBS scenario raised the
foreground p99 by 0.2 to 0.4 ms over the baseline. The spread between
repetitions of the same scenario was larger than that (up to 1.5 ms). Vulkan at
default priority added +0.42 ms to the mean p99. D3D11 with the release-build
priority settings added +0.29 ms. Taken at face value, Vulkan does *not* meet
the pass criterion ("does not worsen the foreground p99 more than D3D11 at
HIGH"), but the gap (0.13 ms) is within the noise, so neither a pass nor a fail
is established. The only consistent ordering in the data: Vulkan `LOW`'s p50
was above both the baseline and D3D11 `HIGH` in every repetition. A fixed run
order confounds it, so it is unresolved. This setup could not have shown a
benefit (see Reading). It does **not** show that Vulkan queue priority
protects the foreground app.

## Machine

- GPU: NVIDIA GeForce RTX 4080 SUPER, driver 621.24 (Windows 32.0.16.2124),
  HAGS enabled. The CPU's integrated AMD Radeon Graphics is also present. It
  was not used or measured.
- CPU: AMD Ryzen 7 9800X3D. OS: Windows 11 Pro 10.0.26100.
- Shell not elevated (no Administrator). Power plan, driver settings and
  Windows settings were left as they were.
- Build: `build_x64` RelWithDebInfo at commit b49085340. `GPU_PRIORITY_VAL`
  is empty in this build, so `libobs-d3d11` itself makes no priority calls.
- Vulkan: the graphics family offers only `LOW` and `MEDIUM` global priority.
  `OBS_VULKAN_REALTIME=1` therefore got `MEDIUM` (logged: "requested realtime
  (1024), granted medium (256)"). Realtime cannot be granted on this driver,
  with or without elevation.

## Procedure

- Foreground load (`test/cmocka/vulkan_priority_fg_load.cpp`, not OBS): a
  D3D11 device with a 1920x1080 window. Each frame is one fullscreen triangle
  whose pixel shader runs a 1536-iteration sin/cos loop. It presents with
  `Present(0, 0)` (no vsync) and then spins on an event query until the GPU
  is done. The frame time is the QueryPerformanceCounter delta between
  consecutive GPU completions, about 6 ms (about 160 fps). Each run is 5 s of
  warm-up (discarded) plus 30 s measured. The runs below used the
  tool as of b49085340, which shows the window activated and calls
  `SetForegroundWindow` (Windows refused it in 14 of 15 runs). The tool now
  shows the window without activation by default, so it does not take
  keyboard focus. `--focus` (runner: `-Focus`) restores the measured
  behaviour.
- OBS side load (`test_vulkan_video load`): the real libobs video pipeline at
  1920x1080, NV12, 60 fps, GPU conversion, BT.709 partial range. The scene has
  a full-canvas colour source, four texture sources upscaled 9x to 60x
  (bicubic, lanczos, bilinear) and a 50% translucent overlay. A raw video
  callback copies every NV12 frame out (the encoder stand-in), so each frame
  is read back through a stage surface. It starts about 1 s before the
  foreground load and runs 41 s.
- Scenarios:
  - (a) foreground alone.
  - (b0) + OBS on D3D11 as built here (no priority calls).
  - (b1) + OBS on D3D11 doing what release builds do without Administrator:
    with HAGS on, `D3DKMTSetProcessSchedulingPriorityClass(HIGH)` and then
    `IDXGIDevice::SetGPUThreadPriority(7)`. Both succeeded. The release
    value of `GPU_PRIORITY_VAL` is not public; 7 (the maximum) is assumed.
  - (c) + OBS on Vulkan at default (`LOW`, granted `LOW`).
  - (d) + OBS on Vulkan with `OBS_VULKAN_REALTIME=1` (granted `MEDIUM`).
- Three repetitions, interleaved: a, b0, b1, c, d, then the same order again
  twice, with 3 s between runs. Validation layers were off.
- D3D11 at REALTIME (the non-HAGS path, which needs Administrator) was not
  measured: no elevated shell is available, and on this machine HAGS is on,
  so release D3D11 would use HIGH anyway.
- Observed GPU use: Windows "GPU Engine" 3D utilisation counters, sampled once
  per second over the measured window, per process. CPU time comes from
  `GetProcessTimes` at exit.

Command (PowerShell, worktree root, after building `vulkan_priority_fg_load`
and `test_vulkan_video`):

    powershell -ExecutionPolicy Bypass -File test\cmocka\vulkan_priority_bench.ps1 -Out build_x64\priority-measurement

Defaults: `-Seconds 30 -Warmup 5 -Reps 3 -Iters 1536`. Add `-Focus` to reproduce
the runs below exactly (the window then takes the foreground and the keyboard
focus). Per-frame CSVs, logs and
`summary.csv` are in `build_x64/priority-measurement/` (not committed).

## Raw results (foreground frame time, ms)

| scenario | rep | frames | mean | p50 | p95 | p99 | max | OBS lagged frames | OBS CPU s / 41 s |
|---|---|---|---|---|---|---|---|---|---|
| a baseline | 1 | 4861 | 6.173 | 6.011 | 7.375 | 7.829 | 8.683 | - | - |
| a baseline | 2 | 4843 | 6.195 | 5.995 | 7.490 | 8.167 | 12.643 | - | - |
| a baseline | 3 | 4730 | 6.343 | 6.255 | 7.609 | 8.043 | 8.980 | - | - |
| b0 D3D11 as built | 1 | 5022 | 5.975 | 5.798 | 7.237 | 7.759 | 43.219 | 8 | 5.7 |
| b0 D3D11 as built | 2 | 4525 | 6.631 | 6.646 | 7.895 | 8.590 | 14.323 | 0 | 3.5 |
| b0 D3D11 as built | 3 | 4570 | 6.566 | 6.618 | 7.783 | 8.208 | 9.376 | 0 | 2.9 |
| b1 D3D11 release (HIGH, 7) | 1 | 4648 | 6.456 | 6.511 | 7.538 | 8.011 | 8.893 | 0 | 2.9 |
| b1 D3D11 release (HIGH, 7) | 2 | 4582 | 6.550 | 6.506 | 7.851 | 8.381 | 14.373 | 0 | 5.1 |
| b1 D3D11 release (HIGH, 7) | 3 | 4483 | 6.692 | 6.670 | 8.017 | 8.530 | 9.598 | 0 | 3.9 |
| c Vulkan default (LOW) | 1 | 4606 | 6.515 | 6.615 | 7.577 | 7.999 | 13.948 | 4 | 5.9 |
| c Vulkan default (LOW) | 2 | 4554 | 6.588 | 6.595 | 7.858 | 8.512 | 9.784 | 7 | 4.2 |
| c Vulkan default (LOW) | 3 | 4375 | 6.858 | 6.886 | 8.215 | 8.787 | 10.111 | 0 | 4.4 |
| d Vulkan realtime opt-in (got MEDIUM) | 1 | 4724 | 6.351 | 6.223 | 7.606 | 8.010 | 9.638 | 0 | 4.8 |
| d Vulkan realtime opt-in (got MEDIUM) | 2 | 4568 | 6.569 | 6.469 | 7.915 | 9.168 | 96.652 | 7 | 3.6 |
| d Vulkan realtime opt-in (got MEDIUM) | 3 | 4961 | 6.049 | 5.910 | 7.301 | 7.699 | 8.158 | 23 | 5.2 |

OBS lagged frames are counted over the whole 41 s OBS run. That includes
start-up and the seconds after the foreground load exits, so the window is
wider than the measured one.

Every OBS run delivered 2459 frames to the callback and rendered 2460, with 0
output-skipped frames. The foreground process used 6.6 to 7.9 s of CPU per
run, mostly from spinning on the event query.

3D engine utilisation over the measured window (percent, per process): the
foreground load 78 to 87; the OBS load 0.8 (Vulkan LOW), 1.1 (D3D11) and 1.3
to 1.4 (Vulkan MEDIUM); **an unrelated user process, `D:\Tools\ves-host\ves.exe`,
96 to 100 in every run including the baseline**; dwm, wallpaper64 and rustdesk
about 1 each. These are per-process engine-busy percentages and can overlap,
so they do not add up to 100.

## Summary (mean of the 3 repetitions)

| scenario | p50 | p95 | p99 | p99 vs baseline | worst max |
|---|---|---|---|---|---|
| a baseline | 6.087 | 7.491 | 8.013 | - | 12.6 |
| b0 D3D11 as built | 6.354 | 7.638 | 8.186 | +0.17 | 43.2 |
| b1 D3D11 release (HIGH) | 6.562 | 7.802 | 8.307 | +0.29 | 14.4 |
| c Vulkan default (LOW) | 6.699 | 7.883 | 8.433 | +0.42 | 13.9 |
| d Vulkan opt-in (MEDIUM) | 6.201 | 7.607 | 8.292 | +0.28 | 96.7 |

Between repetitions of one scenario, p99 varied by 0.34 ms (baseline) up to
1.47 ms (d). Single stalls (43 ms in b0, 97 ms in d) happened once each and
did not repeat. They are not attributed to the backend.

## Reading

- Pass criterion: not shown. Vulkan `LOW` is numerically 0.13 ms worse than
  D3D11 `HIGH` in mean p99, inside the noise. The benchmark was not tuned
  after seeing this.
- The only consistent ordering in the data: in every repetition, Vulkan
  `LOW`'s p50 is above the baseline (+0.60, +0.60 and +0.63 ms, about 10%)
  and above D3D11 `HIGH` (+0.10, +0.09 and +0.22 ms). Being above the
  baseline is not distinctive: b0 and b1 are above it too in reps 2 and 3
  (b0 +0.65 and +0.36 ms, b1 +0.51 and +0.42 ms; in rep 1, b1 +0.50 and b0
  -0.21). That fits the order and drift explanation. The margin over D3D11
  `HIGH` points to Vulkan hurting the foreground slightly *more*. But c
  always ran right after b1,
  and within each repetition the mean frame time tends to rise from a to c
  (rep 3 means in the raw table: 6.343, 6.566, 6.692, 6.858). Drift (thermal
  or clock) from the fixed order may explain some or all of it, so it is
  unresolved.
- Apart from that, no priority effect on the foreground app was measurable
  on this driver. That does not mean there is none.
- OBS lagged frames show no pattern by priority level:
  - Vulkan `MEDIUM` (d) lagged more than Vulkan `LOW` (c): 0/7/23 against
    4/7/0.
  - D3D11 without any priority calls (b0) also lagged (8/0/0).
  - D3D11 `HIGH` (b1) lagged none (0/0/0).

  All counts are under 1% of 2460 frames, vary widely between repetitions,
  and come from a window wider than the measured one. No trade-off can be
  read from them.
- **This setup could not have shown a benefit:**
  - The OBS load is 0.8 to 1.4% of the 3D engine, about 0.05 to 0.09 ms per
    6 ms frame. That is far below the 0.34 to 1.47 ms noise between
    repetitions.
  - The foreground load syncs every frame, so it never keeps the GPU
    continuously busy.
  - A third process (`ves.exe`) saturated the 3D engine in every run.
  - The run order was fixed.
  - Each p99 rests on about 45 frames, and there were only 3 repetitions.
  - The load window was the foreground window in only 1 of 15 runs.
  - The lag counts cover a wider window than the measurement.

  It therefore does **not** show that Vulkan queue priority protects the
  foreground app.

## A better design

- An isolated machine state: `ves.exe` and other GPU users stopped, and the
  load window focused (`-Focus`) in every run.
- A foreground load that queues 2 to 3 frames and saturates the GPU, as games
  do.
- An OBS load heavy enough to cost about 10 to 20% of a foreground frame.
- A randomised or counterbalanced scenario order, with at least 10 repetitions
  of at least 60 s each.
- p99.9, max and a stutter count (frames over 2x the median), reported
  together with OBS lag counted inside the same measured window.
- An assertion on the OBS side that the intended device type is in use.
- A second GPU vendor whose driver offers `HIGH` and `REALTIME`.

## Limitations

- One GPU vendor, one driver, one machine. AMD and Intel were not measured
  (the AMD iGPU present was not used). NVIDIA offers no `HIGH` or `REALTIME`
  global priority here.
- A third-party process (`ves.exe`) kept the 3D engine busy throughout. It is
  a user tool and was not stopped. All scenarios share this, but it adds
  contention and noise that a clean machine would not have.
- The foreground window was never the foreground window (Windows' foreground
  lock blocks `SetForegroundWindow` from a background launcher), except in
  d rep 3. Any foreground boost the scheduler gives games is therefore absent,
  and d rep 3 is not comparable to the others.
- Synthetic load: one serialised draw per frame with a GPU sync, no queue of
  frames, no compute or copy work. Real games may expose preemption
  differently.
- Small sample: 3 repetitions of 30 s, fixed order, about 4,500 frames per
  run (p99 rests on about 45 frames).
- `GPU_PRIORITY_VAL` of release builds is unknown; 7 is assumed for b1.
- HAGS was on for every run. It changes how both D3D11 process priority
  classes and Vulkan queue priorities are scheduled. Without HAGS (where
  release D3D11 tries `REALTIME`) the results may differ, and that case was
  not measured.
- "Granted" Vulkan priority is what `vkCreateDevice` accepted. The effective
  scheduling priority cannot be queried.
- The transfer queue carries no work (all uploads and readback use the
  graphics queue), so its `LOW` priority has no effect yet.
