# libobs-vulkan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `libobs-vulkan` graphics module that implements the existing libobs graphics interface with queued (record-and-submit) execution.

**Architecture:** A module laid out like `libobs-d3d11`, exporting the same C symbols that `load_graphics_imports` (`libobs/graphics/graphics-imports.c`) resolves with `os_dlsym`. `gs_*` calls record into per-frame command buffers; a resource tracker inserts barriers; the backend submits at `gs_flush`/present and waits on a timeline semaphore only in the sync points the spec lists.

**Tech Stack:** C++17 (like `libobs-d3d11`), Vulkan 1.3, VMA, glslang (Vulkan SDK), cmocka tests under `test/cmocka`.

**Spec:** `docs/superpowers/specs/2026-11-17-libobs-vulkan-design.md`

## Global Constraints

- Module name `libobs-vulkan`; device type constant `GS_DEVICE_VULKAN` = 4 in `libobs/graphics/graphics.h`.
- The libobs graphics API (`graphics.h`, `graphics-internal.h`) does not change, except adding the constant.
- Vulkan 1.3 is the minimum (dynamic rendering, synchronization2, timeline semaphores). Older devices make `device_create` return `GS_ERROR_NOT_SUPPORTED`.
- One timeline semaphore per queue; a ring of frames in flight, default 3.
- Memory through VMA.
- Shaders: HLSL from the effect parser -> GLSL (adapting `libobs-opengl/gl-shaderparser.c`) -> SPIR-V with glslang.
- Queue priority below the foreground app by default through `VK_KHR_global_priority`; realtime is opt-in. Must not require Administrator.
- Windows and Linux only. The D3D11 and GL backends stay untouched.
- Waits on the GPU only in: `gs_stagesurface_map`, `gs_texture_map`, `gs_texture_acquire_sync`/`release_sync`, `gs_flush`, `gs_timer_get_*`, swapchain acquire and device-lost rebuild.
- Build is opt-in: CMake option `ENABLE_VULKAN`, default OFF (author's call, not in the spec).
- Out of scope for this plan: shared-texture interop, desktop duplicator, game capture, encoder zero-copy (spec milestone 6; separate plan).

## Review Focus

1. **No usable Vulkan device or driver older than 1.3**: `device_create` returns `GS_ERROR_NOT_SUPPORTED` and leaves no leaked instance, so OBS can fall back to another backend. (Task 2)
2. **Window minimized or resized to 0x0**: swapchain creation is skipped, `present` is a no-op, nothing crashes, and it recovers on restore. (Task 5)
3. **Map of a stage surface whose copy was recorded but never submitted**: `gs_stagesurface_map` submits first, then waits; it must not deadlock. (Task 4)
4. **Destroying a resource the GPU is still using** (texture destroyed right after a draw): destruction is deferred until its frame's fence signals. (Task 4)
5. **Several swapchains (projectors) presenting at once**: one that is not ready is skipped; the others do not stall. (Task 5)

---

## File Structure

```
libobs-vulkan/
  CMakeLists.txt
  vulkan-subsystem.hpp/.cpp   gs_device, exports for device/context/state setters
  vulkan-instance.cpp         instance, physical device pick, queues, VMA
  vulkan-frame.hpp/.cpp       frames in flight, timeline semaphores, deferred destruction
  vulkan-tracker.hpp/.cpp     image/buffer layout+access tracking -> barriers (no Vulkan calls)
  vulkan-texture.cpp          texture2d, render targets, zstencil, stagesurf
  vulkan-swapchain.cpp        swapchain, present, rebuild
  vulkan-shader.cpp           shader create, reflection, parameter upload
  vulkan-shaderprocessor.cpp  HLSL -> GLSL -> SPIR-V
  vulkan-pipeline.cpp         pipeline cache, draw
  vulkan-buffers.cpp          vertex/index buffers, sampler states
test/cmocka/test_vulkan_*.cpp
```

### Task 1: Module skeleton and wiring

**Files:**
- Modify: `libobs/graphics/graphics.h` (add `#define GS_DEVICE_VULKAN 4` after `GS_DEVICE_METAL`, line ~503)
- Modify: `CMakeLists.txt` (option `ENABLE_VULKAN`; `add_subdirectory(libobs-vulkan)` under it)
- Create: `libobs-vulkan/CMakeLists.txt`, `libobs-vulkan/vulkan-subsystem.cpp`, `libobs-vulkan/vulkan-subsystem.hpp`
- Create: `deps/vma/` (header-only VMA), `cmake/finders/` entry only if `find_package(Vulkan)` is not enough
- Test: `test/cmocka/test_vulkan_exports.cpp`, `test/cmocka/CMakeLists.txt`

**Interfaces:**
- Produces: a loadable module exporting every symbol that `graphics-imports.c` marks `GRAPHICS_IMPORT` (158 required). Unimplemented ones are `extern "C" EXPORT` stubs returning `nullptr`/`false`/`GS_ERROR_NOT_SUPPORTED`; the 34 `GRAPHICS_IMPORT_OPTIONAL` ones (including interop) are simply not exported. `device_get_name()` returns `"Vulkan"`, `device_get_type()` returns `GS_DEVICE_VULKAN`.

- [ ] **Step 1: Write the failing test** `test_vulkan_exports`: `os_dlopen("libobs-vulkan")` is non-null; the symbol list parsed from `libobs/graphics/graphics-imports.c` lines using `GRAPHICS_IMPORT(` (excluding `_OPTIONAL`) all resolve with `os_dlsym`; `device_get_type()` equals `GS_DEVICE_VULKAN`; `device_get_name()` equals `"Vulkan"`.
- [ ] **Step 2: Run to verify it fails.** Run: `ctest -R test_vulkan_exports -V`. Expected: FAIL, module not found.
- [ ] **Step 3: Implement** the CMake target (`add_library(libobs-vulkan MODULE)`, link `OBS::libobs Vulkan::Vulkan`, `target_enable_feature(libobs "Vulkan renderer")`) and the stub exports. Generate the stub list from `graphics-imports.c`; keep each stub's signature taken from the matching `gs_exports` member in `libobs/graphics/graphics-internal.h`.
- [ ] **Step 4: Run test to verify it passes.** Expected: PASS.
- [ ] **Step 5: Commit** `libobs-vulkan: Add module skeleton`.

### Task 2: Device, queues, allocator

**Files:**
- Create: `libobs-vulkan/vulkan-instance.cpp`, `libobs-vulkan/vulkan-frame.hpp`, `libobs-vulkan/vulkan-frame.cpp`
- Modify: `libobs-vulkan/vulkan-subsystem.hpp/.cpp`
- Test: `test/cmocka/test_vulkan_device.cpp`

**Interfaces:**
- Consumes: Task 1 skeleton.
- Produces: `struct gs_device` with `VkInstance instance; VkPhysicalDevice phys; VkDevice dev; vk_queue graphics; vk_queue transfer; VmaAllocator vma; vk_frame_ring frames;`. `struct vk_queue { VkQueue q; uint32_t family; VkSemaphore timeline; uint64_t next_value; float priority; }`. `vk_frame_ring::begin() -> vk_frame &` (waits only if the ring slot is still in flight), `vk_frame::cmd` (`VkCommandBuffer`), `vk_frame_ring::submit(vk_queue &, VkSemaphore *wait, VkSemaphore *signal)`, `vk_frame::defer_destroy(std::function<void()>)`. Real exports: `device_create(gs_device_t **, uint32_t adapter)`, `device_destroy`, `device_enum_adapters`, `device_enter_context`, `device_leave_context`, `gpu_get_driver_version`, `gpu_get_renderer`.

- [ ] **Step 1: Write failing tests** (each calls `skip()` when `device_create` returns `GS_ERROR_NOT_SUPPORTED`): `device_create_succeeds_on_adapter_0`; `device_create_rejects_invalid_adapter` (adapter 9999 returns `GS_ERROR_FAIL`, no crash); `device_destroy_leaves_no_validation_errors` (run with `VK_LAYER_KHRONOS_validation` enabled, assert zero messages); `graphics_queue_priority_is_below_realtime` (reads `gs_device::graphics.priority`, expects `< 1.0f`).
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** instance creation with the validation layer when `OBS_VULKAN_VALIDATION=1`, a physical-device picker that requires 1.3 plus `synchronization2`, `dynamicRendering`, `timelineSemaphore` (otherwise `GS_ERROR_NOT_SUPPORTED`, destroying everything created so far), a graphics queue, a transfer-only queue when a distinct family exists, optional `VK_KHR_global_priority` (request below-foreground priority, record what the driver granted in `vk_queue::priority` and log it), and VMA.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `libobs-vulkan: Add device, queues and allocator`.

### Task 3: Resource tracker

**Files:**
- Create: `libobs-vulkan/vulkan-tracker.hpp`, `libobs-vulkan/vulkan-tracker.cpp`
- Test: `test/cmocka/test_vulkan_tracker.cpp` (compiles `vulkan-tracker.cpp` directly; needs no GPU)

**Interfaces:**
- Produces:
  ```cpp
  struct vk_access { VkPipelineStageFlags2 stage; VkAccessFlags2 access; VkImageLayout layout; };
  struct vk_barriers { std::vector<VkImageMemoryBarrier2> images; std::vector<VkBufferMemoryBarrier2> buffers; };
  class vk_tracker {
  public:
      void use_image(VkImage, uint32_t family, const vk_access &needed, vk_barriers &out);
      void use_buffer(VkBuffer, uint32_t family, const vk_access &needed, vk_barriers &out);
      void forget(VkImage);
      void forget(VkBuffer);
  };
  ```
  The first use of an image has old layout `UNDEFINED` and is not a hazard.

- [ ] **Step 1: Write failing tests**: `first_use_emits_undefined_to_needed_layout`; `same_access_twice_emits_nothing`; `write_then_read_emits_one_barrier_with_write_in_src`; `read_then_read_emits_nothing_when_layout_matches`; `layout_change_emits_barrier_with_old_and_new_layout`; `forget_resets_to_undefined`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** the tracker as a map from handle to last `vk_access`, coalescing read-after-read.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `libobs-vulkan: Add resource tracker`.

### Task 4: Textures, render targets, clear, stage-surface readback

**Files:**
- Create: `libobs-vulkan/vulkan-texture.cpp`
- Modify: `libobs-vulkan/vulkan-subsystem.cpp` (recording of copies and clears, deferred destruction)
- Test: `test/cmocka/test_vulkan_readback.cpp` (loads exports by `os_dlsym` from the module; skips without a device)

**Interfaces:**
- Consumes: `gs_device`, `vk_frame_ring`, `vk_tracker`.
- Produces real exports: `device_texture_create`, `gs_texture_destroy`, `device_set_render_target`, `device_clear`, `device_stagesurface_create`, `device_stage_texture`, `gs_stagesurface_map`, `gs_stagesurface_unmap`, `gs_stagesurface_destroy`, `device_flush`, plus `gs_color_format` -> `VkFormat` mapping function `VkFormat vk_format(enum gs_color_format)` (BGRA8, RGBA8, R8, RGBA16F, RGBA32F, R10G10B10A2, sRGB variants).

- [ ] **Step 1: Write failing tests**: `clear_rgba8_then_readback_matches` (clear 64x64 to (0.25, 0.5, 0.75, 1.0), stage, map, assert every pixel is within 1/255); `map_without_flush_submits_and_returns` (Review Focus 3, with a 5 s timeout via a watchdog thread); `destroy_texture_after_draw_is_safe` (Review Focus 4, validation layer reports zero errors); `unsupported_format_returns_null`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** textures with VMA; `device_clear` and `device_stage_texture` record into the current frame's command buffer through the tracker's barriers; `gs_stagesurface_map` submits if work is pending, then waits on the timeline value of the copy; `gs_*_destroy` uses `vk_frame::defer_destroy`.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `libobs-vulkan: Add textures and readback`.

### Task 5: Swapchain and present

**Files:**
- Create: `libobs-vulkan/vulkan-swapchain.cpp`
- Test: `test/cmocka/test_vulkan_swapchain.cpp` (creates a hidden window; skips where no windowing is available)

**Interfaces:**
- Produces real exports: `device_swapchain_create`, `gs_swapchain_destroy`, `device_resize`, `device_load_swapchain`, `device_present`, `device_get_color_space`, `device_update_color_space`.

- [ ] **Step 1: Write failing tests**: `present_after_clear_succeeds`; `resize_to_zero_then_present_is_noop` (Review Focus 2); `resize_back_recovers`; `two_swapchains_present_independently` (Review Focus 5: one marked not ready is skipped, the other presents).
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** Win32 (`VK_KHR_win32_surface`) and Xlib/Wayland surfaces matching `gs_init_data`; on `VK_ERROR_OUT_OF_DATE_KHR` or `SUBOPTIMAL`, recreate; per-swapchain "ready" via `VK_KHR_present_wait` when available, otherwise skip when `vkAcquireNextImageKHR` reports not ready.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Manual check:** start OBS with `graphics_module=libobs-vulkan`, confirm the preview clears and presents.
- [ ] **Step 6: Commit** `libobs-vulkan: Add swapchain and present`.

### Task 6: Shader pipeline

**Files:**
- Create: `libobs-vulkan/vulkan-shaderprocessor.cpp`, `libobs-vulkan/vulkan-shader.cpp`
- Reference: `libobs-opengl/gl-shaderparser.c`
- Test: `test/cmocka/test_vulkan_shader.cpp`

**Interfaces:**
- Produces: `std::vector<uint32_t> hlsl_to_spirv(const char *hlsl, enum gs_shader_type, std::string &error)`; real exports `device_vertexshader_create`, `device_pixelshader_create`, `gs_shader_destroy`, `gs_shader_get_num_params`, `gs_shader_get_param_by_idx`, `gs_shader_get_param_by_name`, `gs_shader_get_param_info`, `gs_shader_set_*`, `gs_shader_get_viewproj_matrix`, `gs_shader_get_world_matrix`. Parameters are packed into one uniform buffer per shader with std140 offsets taken from SPIR-V reflection.

- [ ] **Step 1: Write failing tests**: `solid_effect_vertex_and_pixel_compile` (strings extracted from `libobs/data/default.effect` by the shared effect parser); `invalid_hlsl_returns_error_not_crash`; `param_info_names_and_types_match_effect` (`ViewProj` is `GS_SHADER_PARAM_MATRIX4X4`); `set_float_writes_std140_offset`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** by adapting the HLSL token handling in `gl-shaderparser.c` to emit Vulkan GLSL (`#version 450`, explicit `set`/`binding`), compile with glslang, and reflect with glslang's reflection API.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `libobs-vulkan: Add HLSL to SPIR-V shader path`.

### Task 7: Pipeline cache, draw, buffers, samplers

**Files:**
- Create: `libobs-vulkan/vulkan-pipeline.cpp`, `libobs-vulkan/vulkan-buffers.cpp`
- Test: `test/cmocka/test_vulkan_draw.cpp`

**Interfaces:**
- Produces: `struct vk_pipeline_key { shader pair; blend; depth_stencil; raster; vertex layout; color/depth target formats }` with `operator==` and `std::hash`; `VkPipeline vk_pipeline_cache::get(const vk_pipeline_key &)`. Real exports: `device_draw`, `device_load_vertexbuffer`, `device_load_indexbuffer`, `device_vertexbuffer_create`, `gs_vertexbuffer_flush`, `gs_vertexbuffer_flush_direct`, `device_indexbuffer_create`, `gs_indexbuffer_flush`, `device_samplerstate_create`, `device_load_samplerstate`, `device_load_texture`, `device_blend_function*`, `device_enable_depth_test`, `device_set_cull_mode`, `device_set_viewport`, `device_set_scissor_rect`, `device_load_vertexshader`, `device_load_pixelshader`.

- [ ] **Step 1: Write failing tests**: `same_state_returns_same_pipeline`; `different_blend_returns_different_pipeline`; `textured_quad_readback_matches_source` (upload a 4x4 gradient, draw a full-target quad through the default effect, readback equals source); `blend_alpha_matches_reference` (50% alpha over a known color, assert within 1/255); `scissor_limits_clear_area`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** state setters as CPU-side state; `device_draw` resolves the pipeline from the cache, binds descriptors from the frame's descriptor pool, writes uniforms to the frame's upload arena, and records the draw with dynamic rendering; dynamic state for viewport, scissor and cull mode.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Commit** `libobs-vulkan: Add pipelines and drawing`.

### Task 8: Full effect set and scene compositing

**Files:**
- Modify: whichever files the failures point to (`vulkan-shaderprocessor.cpp` expected)
- Test: `test/cmocka/test_vulkan_effects.cpp`

**Interfaces:**
- Consumes: Tasks 4-7.

- [ ] **Step 1: Write failing tests**: `all_core_effects_compile` (every `.effect` in `libobs/data/` and `plugins/obs-filters/data/` compiles for both stages; the test lists any failure by file); `texrender_roundtrip` (`gs_texrender_begin/end` via the generic API with `gs_create("libobs-vulkan")`); `two_source_composite_matches_d3d11_reference` (golden PNG produced once from the D3D11 backend, tolerance 2/255).
- [ ] **Step 2: Run to verify they fail**; the first failure list is the work list.
- [ ] **Step 3: Fix** translator gaps one effect at a time until the set passes; anything not translatable is recorded in `docs/superpowers/specs/2026-11-17-libobs-vulkan-design.md` under Risks with the effect name.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Manual check:** run OBS with the Vulkan module, confirm scene preview, a recording, and a stream test output.
- [ ] **Step 6: Commit** `libobs-vulkan: Support the full core effect set`.

### Task 9: Queue priority behaviour

**Files:**
- Modify: `libobs-vulkan/vulkan-instance.cpp`
- Create: `docs/superpowers/specs/2026-11-17-libobs-vulkan-priority-measurement.md` (procedure and results)
- Test: `test/cmocka/test_vulkan_priority.cpp`

**Interfaces:**
- Consumes: `vk_queue::priority` from Task 2.

- [ ] **Step 1: Write failing tests**: `granted_priority_is_logged` (device creation logs the requested and granted global priority); `realtime_is_not_requested_by_default`; `realtime_requested_when_env_obs_vulkan_realtime_is_1`.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement** the opt-in realtime path (env var plus settings hook), and fall back to the default level without error when the driver refuses.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Measure on hardware** (NVIDIA, AMD, Intel where available): run a GPU-bound foreground app, record its frame-time p99 with OBS idle, with the D3D11 backend at `REALTIME`, and with the Vulkan backend at default priority. Write the numbers into the measurement doc. Pass criterion from the spec: Vulkan at default priority without Administrator does not worsen the foreground p99 more than D3D11 at `HIGH`.
- [ ] **Step 6: Commit** `libobs-vulkan: Add queue priority control and measurements`.

---

## Self-review notes

- **Spec coverage:** device, queues, priority (2, 9); recording, submission, sync points (4, 5); resource tracking (3); shaders (6, 8); pipelines (7); milestones 1-5 (5, 4, 7, 8, 9); interop and milestone 6 are explicitly out of scope per the spec's deferral.
- **Order differs from the spec's milestones:** readback (spec milestone 4) comes before present (milestone 1) because it allows headless tests. This is the author's call.
- **Open items for the implementer:** frame-in-flight count and arena sizes, descriptor pool sizing, pipeline cache eviction, mipmap/cube/volume textures (not covered by any task above; add them if the effect set in Task 8 needs them).
