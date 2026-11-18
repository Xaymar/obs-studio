# libobs-vulkan backend: design

Status: implemented through Task 9 of the plan (see the plan and the task
ledger). The queue priority measurement was inconclusive. Everything is
verified only through ctest: the OBS frontend cannot select this backend
(`frontend/OBSApp.cpp:1209`, `GetRenderModule` maps only "Direct3D 11" and
otherwise OpenGL), and the manual preview check in OBS was never done.

## Intent and why

OBS's D3D11 backend runs on the immediate context. Every `gs_*` call executes
at once on a single queue, and OBS raises its GPU process priority
(`d3d11-subsystem.cpp:444-454`: `REALTIME` class, which needs Administrator
rights, or `HIGH` with HAGS). The result is that OBS's commands preempt
whatever the GPU is running (usually the game being captured), so even a tiny
OBS render can cause a visible stutter in the game.
This premise is an unverified hypothesis. The first measurement
(`2026-11-17-libobs-vulkan-priority-measurement.md`) neither reproduced it
(D3D11 at `HIGH` added +0.29 ms to the foreground p99, within the noise) nor
refuted it.

A Vulkan backend records work into command buffers and submits it on queues
OBS chooses. It can run at a lower priority than the foreground app without
administrator rights, hide GPU latency behind frames in flight, and wait only
where a caller truly needs a result.

## Goals

- A new module `libobs-vulkan` that implements the existing libobs graphics
  interface (`graphics-internal.h` vtable). The libobs graphics API does not
  change.
- OBS GPU work does not preempt the foreground application, and this does not
  depend on running as Administrator. (This goal rests on the unverified
  preemption hypothesis above; it is not yet shown that it is achieved.)
- The CPU never waits for the GPU except in the functions that must return
  data or take ownership (see Sync points).
- Windows in the first version; Linux not started (see Risks).

## Non-goals

- Changing the libobs graphics API (async readback, fences exposed to callers).
  That is future work.
- A render graph or node-graph scheduler. That is a separate project; the
  backend is built so that part can be reused (see Resource tracking).
- macOS (the Metal backend exists; MoltenVK is not planned).
- Replacing the D3D11 or GL backends. They stay.
- Matching D3D11 feature-for-feature on day one (see Milestones).

## Anti-goals

- A backend that is correct but blocks on every call, which would give the
  same stalls as D3D11.
- A backend that needs elevated rights to avoid preempting the game.

## Constraints

- Shader source reaches the backend as HLSL-flavoured strings through
  `device_vertexshader_create` and `device_pixelshader_create`. D3D11 compiles
  them directly. GL translates them in `gl-shaderparser.c`. Vulkan needs
  SPIR-V.
- The backend is chosen by module name (`obs_video_info.graphics_module`,
  `gs_create` in `obs.c:490`). A new device-type constant
  `GS_DEVICE_VULKAN` (4) is needed in `graphics.h`.
- The API is synchronous in places: `gs_stagesurface_map`,
  `gs_texture_map`, `gs_texture_acquire_sync` and `gs_flush` return or require
  completed results. These are the sync points; everything else must be
  deferrable.
- Existing consumers of D3D11-specific objects (`gs_texture_get_obj`, shared
  handles, keyed mutexes, encoders taking D3D11 textures, game capture, the
  desktop duplicator) will not work on Vulkan at first.

## Approaches considered

1. **Record-and-submit port. Chosen.** Mirror the D3D11 backend's structure;
   each `gs_*` call records into a command buffer; submit and wait only at
   sync points.
2. Render-graph backend with queue splitting and memory aliasing. Rejected for
   this project: it is the node-graph project's job, and the immediate-style
   API gives it little to schedule from.
3. Translation layer over the D3D11 backend (DXVK-style). Rejected: no control
   over queues or sync, which is the goal.

Decided by the owner: approach 1.

## Design

### Structure

`libobs-vulkan/`, laid out like `libobs-d3d11` (subsystem, texture2d,
stagesurf, vertexbuffer, indexbuffer, zstencil, samplerstate, shader,
shaderprocessor, swapchain). Built from the top-level `CMakeLists.txt` and
loaded by module name.

### Device, queues and priority

- Vulkan 1.3 baseline (dynamic rendering, synchronization2, timeline
  semaphores).
- One graphics queue, plus a separate transfer queue when the device exposes
  one (for readback and uploads).
- Queue priority is requested through `VK_KHR_global_priority` when available:
  default below the foreground app's priority, `REALTIME` only as an opt-in
  that may need elevation. Whether a driver honours it varies by vendor.
  As built (Task 9): the graphics queue asks for `LOW`. Opt-in to more:
  environment variable `OBS_VULKAN_REALTIME=1` (asks for `REALTIME`), or the
  optional module export `int obs_vulkan_set_global_priority(int level)`
  (a `VkQueueGlobalPriorityKHR` value: 128, 256, 512, 1024; affects devices
  created afterwards; the environment variable wins). Only levels the queue
  family reports are asked for; a refused level falls to the next lower one,
  then to no request, without failing device creation. The transfer queue
  never asks for more than `LOW`. The offered, requested and granted levels
  are logged ("granted" means accepted by `vkCreateDevice`; Vulkan cannot
  query the scheduling priority actually used). NVIDIA 621.24 (RTX 4080
  SUPER, Windows 11) offers only `LOW` and `MEDIUM`, so `REALTIME` there
  yields `MEDIUM` with or without elevation.
- Nothing is submitted to the transfer queue yet: uploads, texture copies
  and stage-surface readback are all recorded into the graphics queue's
  frame command buffer. The queue is created and its priority set, but the
  async-copy path the bullet above intends is a follow-up: submit readback
  and uploads on the transfer queue; emit queue-family ownership transfers
  from the tracker (it accepts a family but never emits a transfer); and wait
  on the transfer queue's timeline from the graphics queue (and back).
- Memory through the Vulkan Memory Allocator (VMA).

### Recording and submission

- A ring of frames in flight (default 3). Each holds a command buffer, a
  descriptor pool, and an upload arena.
- State setters (`gs_load_*`, blend, depth, raster, viewport, scissor) update
  CPU-side state. A draw resolves a pipeline from a cache keyed on shader
  pair, blend, depth/stencil, raster, vertex layout and target formats, then
  records the draw.
- Submission happens at `gs_flush`, `present`, and at any sync point that
  needs results. Completion is tracked on one timeline semaphore per queue.

### Resource tracking

Each image and buffer records its current layout and last access. The backend
inserts barriers (`vkCmdPipelineBarrier2`) when an operation needs a different
state. This tracking lives in its own module so a future scheduler can reuse
it.

### Shaders

HLSL source from the effect parser is translated to GLSL by reusing or
adapting the logic in `gl-shaderparser.c`, then compiled to SPIR-V with
glslang. SPIR-V reflection builds descriptor layouts. Effect parameters go
into a per-draw uniform buffer.

### Sync points

The backend waits for the GPU only in:

- `gs_stagesurface_map`: submits the frame holding the copy if it is still
  being recorded, then waits for that copy's timeline value.
- `gs_texture_map` never waits (as D3D11's `WRITE_DISCARD`): it hands out
  space in the recording frame's upload arena, and `gs_texture_unmap` records
  the copy from it into the frame being recorded. A slot keeps its arena while
  a mapping in it is outstanding; a mapping held across a submit is first
  copied into the current frame's arena. Its only wait is the frame ring's
  `begin()` below, when the map starts a frame.
- `gs_texture_acquire_sync` / `release_sync`: only meaningful for imported
  textures (see Interop); until then unsupported.
- `gs_flush` and `gs_timer_get_*`: submit; wait only when the caller asks for
  data.
- Swapchain acquire on out-of-date, and a `SUBOPTIMAL` whose surface size
  changed: the rebuild path below. There is no device-loss recovery (nothing
  like `d3d11-rebuild.cpp`): on a lost device every wait returns
  `VK_ERROR_DEVICE_LOST` (`vulkan-frame.cpp` submit, `vk_consume_semaphore`),
  so nothing hangs, but errors are logged every frame and OBS must be
  restarted.
- Frame ring `begin()` (frames-in-flight throttle): waits only when the slot
  being reused (3 submissions back) is still in flight. Every `gs_present`
  is its own submission (`vulkan-swapchain.cpp` `device_present`), so with N
  displays `begin()` waits on work from the current or previous OBS frame,
  not 3 OBS frames back.
- Swapchain destroy and resize (rebuild path): a graphics queue idle wait
  before the images and present semaphores are released.
- Error paths: after a failed submit, `vkQueueWaitIdle` before the timeline
  value is signalled from the host (`vulkan-frame.cpp`); `vk_consume_semaphore`
  waits for the graphics queue (or the device, if that fails) to recover an
  acquire semaphore whose submission failed (`vulkan-swapchain.cpp`).
- Device teardown: `vkDeviceWaitIdle` in the frame ring's `destroy()` and in
  `vk_device_free`.

### Interop (deferred)

Shared textures, the Windows duplicator, game capture, and encoder zero-copy
need `VK_KHR_external_memory` and `VK_KHR_external_semaphore`
(Win32 and fd). Until implemented, those exports return unsupported, and
software or readback encoding paths are used.

### Milestones

| # | Milestone | Status |
|---|-----------|--------|
| 1 | Clear and present on a window. | ctest only (`test_vulkan_swapchain`, hidden window); not verified in OBS UI |
| 2 | Textured quad through the translated shader path. | ctest only (`test_vulkan_draw`); not verified in OBS UI |
| 3 | Full effect set: scene compositing and the built-in effects and filters. | ctest only (`test_vulkan_effects` compiles every in-tree effect; `test_vulkan_video` runs the OBS video pipeline end to end and compares it with D3D11); not verified in OBS UI |
| 4 | Stage-surface readback, so recording and streaming work. | ctest only (`test_vulkan_readback`, `test_vulkan_video`); recording and streaming not verified in OBS |
| 5 | Queue priority behaviour measured against a foreground game. | measured once, inconclusive (see Risks) |
| 6 | Shared-texture interop and encoder zero-copy. | not started |

## Decisions made by the author (not yet confirmed)

- Vulkan 1.3 as the minimum.
- HLSL to GLSL to SPIR-V through glslang, instead of DXC.
- VMA for allocation.
- Default queue priority below the foreground app; realtime is opt-in.
- Windows and Linux only (Linux not started).

## Left to the builder

- Exact frame-in-flight count and arena sizes.
- Pipeline cache eviction.
- Descriptor management details.
- Format mapping for `gs_color_format`, including sRGB views and NV12 and P010.
- Handling of `gs_texrender`, mipmaps, cube and volume textures.

## Risks

- The HLSL to GLSL translator may not cover every effect used by plugins.
  Status (Task 8, `test_vulkan_effects` `all_effects_compile`): all 52
  `.effect` files in the tree (libobs/data, frontend/data, test/test-input/data,
  plugins/obs-filters, obs-transitions, nv-filters, text-freetype2) translate,
  create shader modules and create a pipeline for every pass under validation.
  Found and fixed: `sdr_on_hdr_filter.effect` (`pow(float3, float)`: HLSL
  broadcasts the scalar; the GLSL prelude adds the overloads) and
  `color_grade_filter.effect` (`texture3d clut_3d`: volume textures were
  missing). Features no effect in the tree uses, still unsupported: `float3x3`
  uniforms (values ignored with a warning), texture arrays (every element gets
  the same texture), cube textures and `texture_cube`, volume textures as render
  targets, dynamic or with `GS_BUILD_MIPMAPS`, `GS_DXT1/3/5` textures, and
  `GS_A8` as a render target (the R8 attachment receives red, not alpha).
  Out-of-tree plugin effects may still need these.
- Barrier tracking is the likeliest source of bugs.
- Queue priority may behave differently across vendors, and could fail to stop
  the preemption it was meant to prevent. Milestone 5 measures it. Result
  (`2026-11-17-libobs-vulkan-priority-measurement.md`, one NVIDIA GPU): no
  benefit shown. Vulkan `LOW` and D3D11 `HIGH` differ in p99 by less than the
  noise; Vulkan `LOW`'s p50 was slightly higher in every repetition, which a
  fixed run order confounds. OBS lagged frames showed no pattern by priority
  level and were noisy (D3D11 without priority calls also lagged). The setup
  could not have shown a benefit; the measurement doc lists a better design.
- Only `VK_KHR_global_priority` is probed. A driver that exposes only
  `VK_EXT_global_priority` gets no priority request (driver default).
- Without interop, game capture and some encoders will not work on this
  backend initially.
- Deferred, recorded by reviewers, not implemented: cube textures, `GS_DXT1/3/5`
  formats, `float3x3` uniforms, `GS_A8` as a render target, volume textures as
  render targets, dynamic or mipmapped; timers (null-safe stubs); HDR and
  colour spaces (SDR only); Linux surfaces and loader linking (untested);
  the shader disk cache (design only); interop (milestone 6 not started);
  the transfer queue carries no work (see Device, queues and priority).
  Devices with a single queue family get no transfer queue at all.
- Linux is not started and untested: there are no Linux surfaces
  (`device_swapchain_create` in `vulkan-swapchain.cpp` fails), libvulkan is
  hard-linked (on Windows `vulkan-1.dll` is delay-loaded), so a machine
  without it cannot even load the module, and `plugins/linux-pipewire/pipewire.c:291`
  dereferences `gs_texture_get_obj()`, which returns NULL here
  (`vulkan-subsystem.cpp`).
- The frame ring advances per submission, not per OBS frame (see Sync points):
  with several displays, `begin()` can wait on the current or previous OBS
  frame. Follow-up: advance the ring at `gs_flush` and append presents to the
  same submission, or raise the ring count.
- A failed submit resets the tracker to `UNDEFINED`, so every live texture's
  contents are undefined afterwards. Textures rendered every frame recover;
  static ones (image sources, LUTs, glyph atlases,
  `obs->video.transparent_texture`) stay corrupt until reloaded. Follow-up:
  snapshot the tracker state at each successful submit and restore it on
  failure, or log "restart OBS".
- Shaders are compiled under the graphics lock with no disk cache: 77-95 ms
  for the first `default.effect`, a hitch whenever a source or filter is
  added.
- Dynamic vertex buffers re-upload the CPU data at draw time on a new frame,
  including changes not yet flushed with `gs_vertexbuffer_flush`; D3D11
  snapshots the data at flush.
- A texture parameter left unset skips the draw (logged once); D3D11 samples
  zero.
- A mapping held across many submissions (no libobs caller does this) keeps
  its slot's upload arena from being reused, which grows meanwhile. The arena
  is per slot and grows by doubling chunks, so dynamic texture uploads occupy
  host-visible memory once per ring slot (three), plus growth slack; the
  previous per-texture staging buffer held them once.
- The SPIRV-Tools optimizer and validator are linked only because the SDK's
  glslang is built with `ENABLE_OPT`; the module disables both
  (`vulkan-shaderprocessor.cpp:833-834`). A glslang built with
  `ENABLE_OPT=OFF` would shrink the module.
- Licensing: the module statically contains Apache-2.0 code (SPIRV-Tools,
  parts of glslang) inside a GPL-2.0-or-later program. Needs a licensing
  review; `THIRD_PARTY.md` notes that the licence texts were not collected.
- A CI runner without a GPU makes `test_vulkan_video_compare` fail by design
  (`test/cmocka/CMakeLists.txt`, the `test_vulkan_video` comment): the Vulkan
  render skips and writes nothing, so the comparison never passes without
  comparing.
- Shipping `LOW` queue priority as the default is an unmeasured product
  decision: OBS itself might lag under a GPU-bound game.
- libobs defects found while building this, not fixed here:
  `libobs/util/cf-lexer.c` overflows the stack on a recursive `#define`;
  a one-byte over-read/over-write when a block comment or string ends with a
  line splice right before the NUL; macro lookup is quadratic.
