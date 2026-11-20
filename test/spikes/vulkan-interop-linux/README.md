# Vulkan interop spikes (Linux)

Phase 1 (Linux part) of the Knurbs Vulkan-only backend plan: hardware spikes for the interop paths the backend
depends on. Throwaway test code, not libobs changes (except the build fixes listed under Spike A).
Knurbs targets Wayland only, so there is no X11 spike.

Run on 2026-11-20 on `merenka-gpu1` (headless container):

| Component | Version |
|---|---|
| GPU | Tesla P100-PCIE-16GB (Pascal, GP100) |
| NVIDIA driver | 580.196.07 (Vulkan 1.4.312, conformance 1.4.1.3) |
| CUDA driver API / NVENC API | 13.0 / 13.0 (ffnvcodec headers 12.1) |
| Kernel | 6.12.107 (nvidia-drm loaded, `/dev/dri/renderD128`) |
| Vulkan loader / headers | 1.4.341 |
| GBM | Mesa 26.0.8 libgbm, backend `nvidia` (nvidia-drm_gbm.so) |
| libdrm / wayland / wayland-protocols | 2.4.131 / 1.24.0 / 1.47 |
| glslang / SPIRV-Tools | 16.2.0 / 2026.1 |
| Compilers | GCC 15.2.0, Clang 21.1.8, CMake 4.2 |
| Validation layer | **not installed**: nothing here was checked against it |

## Building and running

```sh
# Spikes B-G
cmake -S test/spikes/vulkan-interop-linux -B /tmp/spikes -G Ninja && ninja -C /tmp/spikes
test/spikes/vulkan-interop-linux/run-all.sh /tmp/spikes /tmp/spikes/results

# Spike A: libobs-vulkan, then the repo's Vulkan tests built against it (cmocka shim, see a-build/)
cmake -S . -B build-vk -G Ninja -DENABLE_VULKAN=ON -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF \
      -DENABLE_PLUGINS=OFF -DOPENGL_opengl_LIBRARY=/usr/lib/x86_64-linux-gnu/libOpenGL.so.0
ninja -C build-vk libobs-vulkan
cmake -S test/spikes/vulkan-interop-linux/a-build -B /tmp/spike-a -G Ninja -DOBS_BUILD_DIR=$PWD/build-vk
ninja -C /tmp/spike-a && test/spikes/vulkan-interop-linux/a-build/run-tests.sh /tmp/spike-a build-vk
```

The logs of the run described here are in `results/`.

## Summary

| Spike | Result | Main finding |
|---|---|---|
| A: build libobs-vulkan | **Pass** after 3 fixes | It builds with GCC and Clang, but on Linux it hung and dropped draws until the 3 fixes below. With them, all Vulkan tests pass except one Windows-only export check. |
| B: NV12/P010 plane views | **Pass** with a workaround | R8/RG8 and R16/RG16 plane views work as colour attachments and the pixels read back exact. But `loadOp = CLEAR` on the plane-0 (luma) view is dropped. Clearing the chroma plane works, and so does `vkCmdClearAttachments`. |
| C: dmabuf | **Pass** with one rule | BGRA (6 block-linear modifiers + LINEAR) and NV12 (LINEAR only) pass export, import and GBM import. **NV12 that Vulkan allocates and exports as LINEAR is not pitch-linear in memory.** Allocate NV12 with GBM, then import it. |
| D: sync | **Pass** | Timeline OPAQUE_FD and binary SYNC_FD work in both directions. A DRM syncobj cannot be imported directly as a Vulkan semaphore. The sync_file bridge (the usual PipeWire / compositor route) works both ways. |
| E: CUDA to NVENC | **Pass** | Both zero-copy inputs work on Linux: a pitch-linear buffer as CUDADEVICEPTR, and an optimal-tiled VkImage as CUarray (the image path failed on Windows). GPU-side timeline handoff, 30 H.264 frames, PSNR Y 50.4 dB at QP 18. |
| F: cross-process | **Pass** | dmabuf, OPAQUE_FD image, timeline semaphore and a pending sync_file passed with SCM_RIGHTS to an exec'd child. The child imports them, waits on the GPU, verifies, writes back and signals. |
| G: Wayland surface | **Pass** (in-process headless compositor) | VK_KHR_wayland_surface presents in FIFO, MAILBOX and FIFO_LATEST_READY. The compositor checked every frame's pixels. NVIDIA's WSI uses `wp_linux_drm_syncobj_v1` explicit sync when it is offered. |

## Spike A: build libobs-vulkan on Linux

`ENABLE_VULKAN=ON` configured and built with no compile errors (GCC 15: 82 warnings in libobs-vulkan, all
`-Wunused-parameter` and VMA nullability; Clang 21 builds too). The problems only showed up when the module was
loaded and the repo's Vulkan tests were run. Those tests were built by `a-build/`, against a cmocka stand-in
(`cmocka-shim/`), because `libcmocka-dev` is not in the image and the repo's `test/cmocka` CMake needs
`libobs-null`, which is Windows-only.

Fixes on this branch (all small and portable):

1. **Unresolved SPIRV-Tools symbols at load time** (`libobs-vulkan/CMakeLists.txt`). Static archives resolve left
   to right, and `SPIRV-Tools-opt` was linked after `SPIRV-Tools`. The MODULE linked anyway, because modules allow
   undefined symbols, and then failed in `dlopen`. Fix: `Vulkan::SPIRV-Tools` is linked again after `-opt`, and
   `--no-undefined` is added so this fails at link time.
2. **Every `device_draw` silently dropped its constants** (`libobs-vulkan/CMakeLists.txt`). The module exports
   `gs_shader_set_val`, `gs_shader_set_matrix4` and so on under the same names as libobs. ELF symbol interposition
   bound the module's own internal calls to libobs's wrappers. Those wrappers return early outside a graphics
   context ("called while not in a graphics context", 8400 times in one test run). Result: 22/26 draw tests and
   11/11 depth tests read back black. Fix: link with `-Bsymbolic`. Hidden visibility with explicit exports would be
   the cleaner long-term fix.
3. **`gs_draw_sprite` spun forever** (`libobs/graphics/graphics-imports.c`). On Linux, `dlsym(module, name)` also
   searches the module's dependencies. For an optional import the module does not define (`gs_texture_is_rect`,
   `gs_get_adapter_count`), it returned **libobs's own** `gs_*` function. That function calls itself through the
   export table: a tail-call loop at 100 % CPU (`test_texrender_roundtrip` hung). libobs-opengl defines both, so
   upstream never hits this. Fix: `graphics_dlsym()` rejects any symbol whose `dladdr` base is libobs itself.

Test-only portability fixes: `test_vulkan_shader` hard-coded the Vulkan loader name as `vulkan-1`. And `test_vulkan_effects` passed
a temporary string to `os_opendir`, which on Linux keeps the pointer without copying it (it read the plugin
directory from freed memory).

Results (`results/a-build-tests.log`):

| Test | OK | Failed | Skipped |
|---|---|---|---|
| tracker | 15 | 0 | 0 |
| exports | 4 | 1 | 0 |
| device | 6 | 0 | 1 (validation layer missing) |
| priority | 6 | 0 | 0 |
| readback | 6 | 0 | 0 |
| upload | 14 | 0 | 0 |
| shader | 8 | 0 | 0 |
| draw | 26 | 0 | 0 |
| depth | 11 | 0 | 0 |
| effects (all 52 .effect files) | 4 | 0 | 0 |
| swapchain | 0 | 0 | 6 (Win32 windows only) |
| video render (end-to-end OBS pipeline) | 1 | 0 | 0 |

- `exports` / `test_optional_not_exported` still fails. The test probes with plain `os_dlsym`, which on Linux
  finds libobs's `gs_texture_is_rect` and `gs_get_adapter_count` through the dependency chain (fix 3 above). It
  needs the same "defined in the module" check.
- Queue global priority: the driver offers only `medium` to this process. `low` is requested, and the driver
  default is granted.
- Not done on Linux yet: the libobs-vulkan swapchain (`TODO(linux)` in `vulkan-swapchain.cpp`; Spike G shows the
  driver side works), and the Linux shared-texture / dmabuf entry points (`device_texture_create_from_dmabuf` and
  others are stubs).

## Spike B: NV12 / P010 plane views as colour attachments

`b-plane-views/`: a `G8_B8R8_2PLANE_420_UNORM` image and a `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` image.
Both are created with `MUTABLE_FORMAT | EXTENDED_USAGE`, a format list, and usage COLOR_ATTACHMENT | SAMPLED |
TRANSFER_SRC. Per-plane views: R8 / RG8, and R16 / RG16 for P010. The test clears each plane, draws a pattern
inside a scissor rectangle, copies each plane out with `PLANE_n` aspect copies, and compares every texel. P010
values are 10-bit numbers in the top bits of 16-bit unorm.

- Support: both formats report `VK_SUCCESS` with these flags (max 32768²). The view formats have colour-attachment
  support. The multi-planar formats report SAMPLED, MIDPOINT_CHROMA and DISJOINT.
- Pattern draws through all four views: **exact**.
- **Driver bug (or an undiagnosed app error, because the validation layer is missing): `loadOp = CLEAR` on the
  plane-0 view is dropped.** Luma reads 0 outside the draw, and 0 everywhere when nothing is drawn. The same code
  on the plane-1 view clears correctly. Rendering the chroma plane first does not change this. Workaround that
  passes exactly: `loadOp = DONT_CARE` (or LOAD) plus `vkCmdClearAttachments` inside the rendering. Recheck this
  with the validation layer before it shapes the design.

## Spike C: dmabuf export / import

`c-dmabuf/`. Modifiers offered for 256x128 (usage for BGRA: transfer, sampled, colour attachment; for NV12:
transfer, sampled):

- **BGRA8 (DRM AR24)**: `BLOCK_LINEAR_2D` HEIGHT=0..5 (`0x03000000004fe010`..`015`, KIND=254, GEN=0, SECTOR=1,
  COMPRESSION=0) plus `LINEAR`. All are importable and exportable. LINEAR has no colour-attachment feature.
- **NV12**: `LINEAR` only, 2 memory planes, not renderable.

Results:

- Vulkan export into a second VkDevice with explicit modifier and plane layouts: **pass** for every BGRA modifier
  and for NV12. The driver picks `BLOCK_LINEAR_2D,HEIGHT=4` for BGRA from a list.
- CPU `mmap` of the exported dmabuf: BGRA LINEAR **matches**. **NV12 LINEAR does not match**: every row is wrong,
  and the data is not at the reported offset and pitch (0 / 256, 65536 / 256). If the exporter forces an explicit
  pitch layout, a second VkDevice cannot read it back either. A Vulkan-exported "LINEAR" NV12 can only be read by
  the NVIDIA Vulkan driver itself. **A foreign consumer (VA-API, a compositor, PipeWire) would read garbage.**
- GBM (`nvidia` backend on renderD128):
  - BGRA `gbm_bo_create_with_modifiers2` with Vulkan's list picks BLOCK_LINEAR HEIGHT=4. Device A imports it and
    writes, device B imports the same fd and reads it back exactly.
  - NV12: `gbm_bo_create_with_modifiers2` and `RENDERING|LINEAR` fail with EINVAL, but `GBM_BO_USE_LINEAR` alone
    works (offsets 0 / 65536, pitch 256). Vulkan imports it and writes, a second device reads it back, and a CPU
    `mmap` of the dmabuf **matches**.
  - `gbm_bo_map` is not usable (ENOENT for BGRA, ENOSYS for NV12). Use `mmap` on the dmabuf fd.
- Design consequence: **allocate multi-planar dmabufs with GBM (LINEAR) and import them into Vulkan; do not export
  NV12 from Vulkan.** Single-plane BGRA can be exported from Vulkan.

## Spike D: semaphores and DRM syncobj

`d-sync/`. External semaphore properties: binary OPAQUE_FD and SYNC_FD can be exported and imported; timeline
OPAQUE_FD too; timeline SYNC_FD cannot. Even so, `vkGetSemaphoreFdKHR(SYNC_FD)` on a timeline semaphore returned
VK_SUCCESS (outside the spec, do not rely on it).

- Timeline OPAQUE_FD between two VkDevices: a host signal is seen by the other device. A GPU wait blocks until the
  other device's GPU signal arrives, and the signal back is seen. **Pass.**
- Binary SYNC_FD export of a pending signal: `poll()` stays 0 while the work is gated and becomes readable after.
  A temporary import into the other device makes it wait correctly. **Pass.**
- DRM syncobj (renderD128 has `DRM_CAP_SYNCOBJ_TIMELINE`):
  - A Vulkan OPAQUE_FD **is not a DRM syncobj** on NVIDIA (`drmSyncobjFDToHandle`: EPERM). Importing a syncobj fd
    as a timeline OPAQUE_FD fails (`VK_ERROR_INITIALIZATION_FAILED`).
  - The PipeWire explicit-sync shape through sync_file **passes**. Acquire: wait for the point to have a fence
    (WAIT_AVAILABLE), `drmSyncobjTransfer` it into a binary syncobj, export a sync_file, and import it into Vulkan
    as a temporary SYNC_FD wait. Release: export a SYNC_FD from the submit, `drmSyncobjImportSyncFile`, then
    transfer it to the release point. The GPU only ran after the producer signalled, and the release point
    signalled.

## Spike E: Vulkan to CUDA to NVENC, Linux handles

`e-cuda-nvenc/`, 640x368 NV12, 30 frames, H.264 at constant QP 18. Vulkan writes each frame into exported memory
and signals timeline 2f+1. CUDA (`cuWaitExternalSemaphoresAsync` on the stream given to `nvEncSetIOCudaStreams`)
waits, NVENC encodes, and CUDA signals 2f+2, which Vulkan waits for before reusing the memory. The CUDA device is
matched by UUID. CUDA and NVENC are loaded with dlopen through the ffnvcodec headers, so no CUDA toolkit is
needed.

- `buffer-devptr`: a pitch-linear NV12 VkBuffer (pitch 768), `cuImportExternalMemory(OPAQUE_FD, dedicated)` and
  `cuExternalMemoryGetMappedBuffer`, registered as `CUDADEVICEPTR`: **pass**.
- `image-cuarray`: an optimal-tiled `R8_UNORM` VkImage of W x 1.5H, holding the NV12 bytes,
  `cuExternalMemoryGetMappedMipmappedArray` (1 channel, uint8), registered as `CUDAARRAY`: **pass**. This path
  did not work on Windows. On Linux, the image never leaves Vulkan-native tiling.
- Timeline semaphore as `CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD`: **pass**.
- PSNR (`psnr.py`, frames paired by index): Y average 50.41 dB, UV 50.83 dB, worst frame 47.99 dB, the same for
  both variants (the bitstreams are bit-identical). Note: ffmpeg's `psnr` filter pairs the wrong frames on a raw
  .h264 without timestamps, and reported about 9 dB.

## Spike F: cross-process

`f-cross-process/`. The parent forks before any Vulkan call and execs itself as `--child`. Sent over an
`AF_UNIX SOCK_SEQPACKET` socket with SCM_RIGHTS:

- a BGRA dmabuf (BLOCK_LINEAR HEIGHT=4, the modifier and plane layout in the message)
- an OPAQUE_FD optimal-tiled image (allocation size and device/driver UUIDs in the message)
- a timeline semaphore (OPAQUE_FD)
- a pending binary sync_file

The parent's GPU work is gated until after the send. The child imports everything (the timeline reads 0 at
import), waits on the GPU for timeline 1 and the sync_file, and verifies both images exactly. It then writes a new
pattern into the dmabuf and signals timeline 2 from its GPU. The parent sees 2 and verifies the child's pixels.
**All pass.**

## Spike G: Wayland surface (headless)

No compositor is installed, so `g-wayland/` contains a minimal one on a thread: `wl_compositor` v6,
`zwp_linux_dmabuf_v1` v4 with a format table and main device = renderD128, and optionally
`wp_linux_drm_syncobj_manager_v1`. On every commit it imports the client's dmabuf `wl_buffer` into its own
VkDevice and reads back the pixels. The client uses `VK_KHR_wayland_surface` and a swapchain (256x128, 3 images,
`B8G8R8A8_UNORM`), and presents 6 frames, each cleared to a different colour.

- Surface: images 2..8, formats `B8G8R8A8_SRGB` and `B8G8R8A8_UNORM`, present modes MAILBOX, FIFO_LATEST_READY,
  FIFO.
- FIFO, MAILBOX: 6 commits, every buffer imported, all 6 colours uniform and in order. Buffers use modifier
  BLOCK_LINEAR HEIGHT=4. **Pass.**
- With `--syncobj` the WSI binds `wp_linux_drm_syncobj_manager_v1` and imports 3 timelines (one per swapchain
  image). It sets an acquire and a release point on every commit. **No acquire point had signalled at commit
  time**, so the compositor has to wait. After waiting on it (and the client no longer idles between frames), all
  6 frames verify. **Pass.**
- The WSI asks for `get_surface_feedback`, never for the default feedback, and binds `zwp_linux_dmabuf_v1` once
  per swapchain plus once for the surface. Without the syncobj global it falls back to implicit sync, and that
  case is not tested here (the client idles between frames).

## Missing packages (for `extra-packages.txt`)

- `vulkan-validationlayers`: nothing on Linux has run under validation (Spike B's clear issue needs it).
- `libcmocka-dev`: the repo's `test/cmocka` suite (`a-build/` used a stand-in).
- `libopengl-dev`: `libobs-opengl` needs the `libOpenGL.so` link (worked around with `-DOPENGL_opengl_LIBRARY`).
- `gdb`: for debugging hangs like the one in fix 3 (an LD_PRELOAD backtrace was used instead).
- Optional: `weston`, to test against a real compositor (Spike G used its own).
