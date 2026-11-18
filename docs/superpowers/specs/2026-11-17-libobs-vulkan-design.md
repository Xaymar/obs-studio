# libobs-vulkan backend: design

Status: draft for approval. Nothing is built yet.

## Intent and why

OBS's D3D11 backend runs on the immediate context. Every `gs_*` call executes
at once on a single queue, and OBS raises its GPU process priority
(`d3d11-subsystem.cpp:444-454`: `REALTIME` class, which needs Administrator
rights, or `HIGH` with HAGS). The result is that OBS's commands preempt
whatever the GPU is running (usually the game being captured), so even a tiny
OBS render can cause a visible stutter in the game.

A Vulkan backend records work into command buffers and submits it on queues
OBS chooses. It can run at a lower priority than the foreground app without
administrator rights, hide GPU latency behind frames in flight, and wait only
where a caller truly needs a result.

## Goals

- A new module `libobs-vulkan` that implements the existing libobs graphics
  interface (`graphics-internal.h` vtable). The libobs graphics API does not
  change.
- OBS GPU work does not preempt the foreground application, and this does not
  depend on running as Administrator.
- The CPU never waits for the GPU except in the functions that must return
  data or take ownership (see Sync points).
- Windows and Linux in the first version.

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

- `gs_stagesurface_map`, `gs_texture_map`: wait on the fence for the copy.
- `gs_texture_acquire_sync` / `release_sync`: only meaningful for imported
  textures (see Interop); until then unsupported.
- `gs_flush` and `gs_timer_get_*`: submit; wait only when the caller asks for
  data.
- Swapchain acquire, on out-of-date or lost devices, via the rebuild path
  (equivalent of `d3d11-rebuild.cpp`).

### Interop (deferred)

Shared textures, the Windows duplicator, game capture, and encoder zero-copy
need `VK_KHR_external_memory` and `VK_KHR_external_semaphore`
(Win32 and fd). Until implemented, those exports return unsupported, and
software or readback encoding paths are used.

### Milestones

1. Clear and present on a window.
2. Textured quad through the translated shader path.
3. Full effect set: scene compositing and the built-in effects and filters.
4. Stage-surface readback, so recording and streaming work.
5. Queue priority behaviour measured against a foreground game.
6. Shared-texture interop and encoder zero-copy.

## Decisions made by the author (not yet confirmed)

- Vulkan 1.3 as the minimum.
- HLSL to GLSL to SPIR-V through glslang, instead of DXC.
- VMA for allocation.
- Default queue priority below the foreground app; realtime is opt-in.
- Windows and Linux only.

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
  the preemption it was meant to prevent. Milestone 5 measures it.
- Without interop, game capture and some encoders will not work on this
  backend initially.
