# Vulkan interop spikes (Phase 1 of the Vulkan-only backend plan)

Throwaway test programs that check four interop assumptions on real hardware. They are not
part of the main build and nothing in libobs depends on them.

| Spike | Question | RTX 4070 Laptop | Radeon 780M |
|---|---|---|---|
| 1 | Can I render into NV12/P010 planes through per-plane R8/RG8 (R16/RG16) views? | **PASS** (8/8, validation clean) | **PASS** (8/8, validation clean) |
| 2 | Can Vulkan import D3D11 shared textures (KMT, NT, keyed mutex, NV12)? | BGRA **PASS** (4/4); NV12 **FAIL** (chroma plane wrong) | **PASS** (BGRA 4/4, NV12 16/16) |
| 3 | Vulkan → CUDA → NVENC without a copy? | Buffer/CUDADEVICEPTR **PASS**; NV12 image/CUDAARRAY **FAIL** | n/a |
| 4 | Can AMF on a second VkDevice encode a VkImage exported by the renderer device? | n/a | **PASS** with an app-created AMF device; `InitVulkan(nullptr)` **FAIL** |

## Test machine (2026-11-19)

* Windows 10 Pro 22H2 (10.0.19045), Vulkan loader 1.4.357.
* NVIDIA GeForce RTX 4070 Laptop GPU: driver 621.01 (32.0.16.2101), Vulkan 1.4.357,
  CUDA driver API 13050, NVENC API 13.1 (driver maximum 13.1).
* AMD Radeon 780M (RDNA 3 iGPU): Adrenalin 26.11.2 (32.0.32029.2010), Vulkan 1.4.359 (LLPC),
  AMF runtime 1.5.3.0 (the trace paths say it was built from the 26.20 branch).
* Validation: VK_LAYER_KHRONOS_validation `vulkan-sdk-1.4.363.1`, built from source (there is no Vulkan SDK
  on this machine), with synchronization validation enabled. `vvl_err` in the logs is the number of
  validation errors reported during that test case.
* MSVC 14.51 (VS 18), CMake 4.5. Headers: Vulkan-Headers 1.4.368, volk, FFmpeg nv-codec-headers
  (NVENC 13.1, `dynlink_cuda.h`), AMF 1.5.3 public headers.

The raw output is in `results/`.

## Spike 1: NV12/P010 plane views (`spike1-plane-views`)

The test creates `G8_B8R8_2PLANE_420_UNORM` and `G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` images
(256x128, optimal tiling) with `MUTABLE_FORMAT | EXTENDED_USAGE` and usage `COLOR_ATTACHMENT | TRANSFER_SRC | SAMPLED`.
It makes `R8`/`R8G8` (`R16`/`R16G16`) views of `PLANE_0`/`PLANE_1` and renders a background plus three
rectangles into each plane with dynamic rendering. It does this in two ways, loadOp CLEAR + `vkCmdClearAttachments`
and real draws (a solid-colour rectangle pipeline). Each runs with and without a `VkImageFormatListCreateInfo`.
Every plane is then copied back with `vkCmdCopyImageToBuffer` and every texel is checked.

* **Both GPUs: 8/8 PASS (2 formats × 2 methods × with/without format list), 0 bad texels, 0 validation errors or warnings.** For P010, the 6 padding LSBs
  also come back exactly as written.
* Neither vendor exposes `COLOR_ATTACHMENT` on the multi-planar formats themselves. The plane view formats have
  `SAMPLED STORAGE COLOR_ATTACHMENT TRANSFER_SRC TRANSFER_DST` on both GPUs, so `EXTENDED_USAGE` is required.
* `vkGetPhysicalDeviceImageFormatProperties2(MUTABLE|EXTENDED_USAGE, CA|TS|S)`: VK_SUCCESS on both,
  with a max extent of 32768² on NVIDIA and 16384² on AMD.
* NVIDIA reports `DISJOINT` for both formats. AMD does not, so the backend must not depend on disjoint planes.

## Spike 2: D3D11 texture import (`spike2-d3d11-import`)

The test creates a D3D11 device on the DXGI adapter whose LUID matches `VkPhysicalDeviceIDProperties::deviceLUID`.
It creates a texture, overwrites it with `UpdateSubresource` after creation (for BGRA), flushes and waits on an event
query, then imports it into a dedicated allocation and copies every plane back:

| Case | Handle → Vulkan handle type | NVIDIA | AMD |
|---|---|---|---|
| BGRA `MISC_SHARED` (game-capture style) | `GetSharedHandle` → `D3D11_TEXTURE_KMT_BIT` | PASS | PASS |
| BGRA `SHARED_NTHANDLE` | `CreateSharedHandle` → `D3D11_TEXTURE_BIT` | PASS | PASS |
| BGRA `SHARED_NTHANDLE + KEYEDMUTEX` | NT → `D3D11_TEXTURE_BIT` + `VK_KHR_win32_keyed_mutex` | PASS | PASS |
| BGRA `SHARED_KEYEDMUTEX` (legacy) | KMT → `D3D11_TEXTURE_KMT_BIT` + keyed mutex | PASS | PASS |
| NV12 `SHARED_NTHANDLE`, 8 variants | NT → `D3D11_TEXTURE_BIT` | **FAIL** | PASS |
| NV12 `MISC_SHARED`, 8 variants | KMT → `D3D11_TEXTURE_KMT_BIT` | **FAIL** | PASS |

NV12 variants: 128x64 and 1920x1080; Vulkan flags none or `MUTABLE|EXTENDED_USAGE`; D3D11 bind `SRV` or `SRV|RTV`.

* Both vendors report `DEDICATED_ONLY | IMPORTABLE` for both handle types, and `requiresDedicatedAllocation = 1`.
  `vkGetMemoryWin32HandlePropertiesKHR` gives `0x2` on NVIDIA and `0x1111` on AMD.
* In the keyed-mutex cases Vulkan acquires key 1 and releases key 2 through `VkWin32KeyedMutexAcquireReleaseInfoKHR`,
  and D3D11 then gets key 2 back with `AcquireSync(2)`. This works on both GPUs.
* The import uses an acquire from `VK_QUEUE_FAMILY_EXTERNAL` with `oldLayout = GENERAL`. The validation layers accept this.
* **NVIDIA NV12 bug:** luma is correct in every variant, but chroma is wrong.
  * At 1920x1080, Vulkan's plane-1 row 0 contains D3D11's chroma row 64, so the plane-1 offset disagrees between
    the APIs by 64 chroma rows.
  * At 128x64 the chroma reads come from outside the texture (0xCD, zeros, or bytes from the previously freed BGRA texture).
  * When all 16 NV12 cases run in one process, the device is lost (`VK_ERROR_DEVICE_LOST`) after a few of them
    (`results/spike2.log`). Run one per process (`results/spike2-nv12-isolated.log`), they fail the same way but
    without the device loss.
  * Implication: on NVIDIA, importing D3D11 NV12 (from capture cards or decoders) needs a workaround. One option is
    a D3D11-side copy of the planes into R8/R8G8 shared textures. BGRA game-capture textures are fine on both vendors.

## Spike 3: CUDA → NVENC (`spike3-cuda-nvenc`, NVIDIA only)

The test exports Vulkan memory and a timeline semaphore as `OPAQUE_WIN32`. CUDA imports them on the device with the
matching UUID (`cuImportExternalMemory` with `DEDICATED`, `cuImportExternalSemaphore` with `TIMELINE_SEMAPHORE_WIN32`).
For each of 8 1080p frames, CUDA waits on the timeline value Vulkan signalled, then NVENC encodes H.264 (CONSTQP 16)
straight from the imported resource. The stream is decoded with ffmpeg and compared to the pattern.

* **(B) pitch-linear NV12 `VkBuffer` → `cuExternalMemoryGetMappedBuffer` → `NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR`:
  PASS.** Worst-frame PSNR is 98.7 dB (Y) and 64.3 dB (UV), with no copy anywhere between Vulkan's buffer and NVENC.
  This is the zero-copy form that works.
* **(A) NV12 `VkImage` (optimal) → `cuExternalMemoryGetMappedMipmappedArray(CU_AD_FORMAT_NV12)` → `CUDAARRAY`: FAIL.**
  * The mapping succeeds with `NumChannels = 3` and any combination of `COLOR_ATTACHMENT`, `SURFACE_LDST` and
    `VIDEO_ENCODE_DECODE`. `NumChannels = 1` returns `CUDA_ERROR_INVALID_VALUE`. `nvEncRegisterResource` accepts the array.
  * The data is wrong. A CUDA readback (`cuArrayGetPlane` + `cuMemcpy2D`) shows 2023424 of 2073600 Y bytes wrong.
    This happens for all 4 image variants tried (`MUTABLE|EXTENDED` + CA/TD/S, plain TD/S, plain TD, STORAGE).
  * The readback method is correct: an exported plain `R8_UNORM` image read back as a `UINT8` CUarray is byte-exact.
    The NV12 image's luma mapped the same way is not. So CUDA does not understand the Vulkan NV12 image layout
    on this driver.
  * Encoding the mismatched array anyway gives a garbage stream (PSNR 9.8 dB). On another run it raised
    `NV_ENC_ERR_INVALID_PARAM` on `nvEncLockBitstream` and left the CUDA context in `CUDA_ERROR_ILLEGAL_ADDRESS`.
    The spike now refuses to encode when the readback does not match. Set `SPIKE3_ENCODE_MISMATCH=1` to encode anyway.
* Implication: the NVIDIA encoder path should render or convert into a pitch-linear NV12 (or P010) buffer, for example
  with a compute shader writing the exported `VkBuffer`, and hand NVENC a `CUDADEVICEPTR`.
* CUDA errors are sticky, so every case runs in its own process (`SPIKE_FILTER`).

## Spike 4: AMF on a second VkDevice (`spike4-amf-vulkan`, AMD only)

A "renderer" `VkDevice` creates an NV12 image (`MUTABLE|EXTENDED_USAGE`, CA/TS/TD/S, optimal tiling) with
`OPAQUE_WIN32` export and writes a pattern into it. The same image is created on AMF's `VkDevice` (the UUID is
checked: same GPU) and the memory is imported into it. It is wrapped with `CreateSurfaceFromVulkanNative` and
encoded to H.264 (`AMFVideoEncoderVCE_AVC`, CQP 16, 8 frames). The two devices are synchronised on the CPU
(renderer `vkWaitForFences`, then wait for each AMF output before overwriting).

* **App-created AMF device (separate `VkDevice`, same `VkInstance`): PASS, zero copy.** Worst-frame PSNR is
  99.0 dB (Y) and 64.3 dB (UV). The result is the same whether AMF is told `eCurrentLayout = GENERAL` or `UNDEFINED`.
  Requirements, all found by trial:
  * The instance must enable `VK_KHR_get_physical_device_properties2`, `VK_KHR_external_memory_capabilities`,
    `VK_KHR_external_semaphore_capabilities`, `VK_KHR_surface` and `VK_KHR_win32_surface`. This applies even with
    `apiVersion = 1.3`, because AMF resolves `vkGetPhysicalDeviceProperties2KHR` by its KHR name. Without these,
    `InitVulkan` returns `AMF_FAIL` ("Failed to aquire entrypoint vkGetPhysicalDeviceProperties2KHR").
  * The device needs the extensions from `AMFContext1::GetVulkanDeviceExtensions`: swapchain, sync2,
    external_memory_win32, AMD_device_coherent_memory, timeline_semaphore, external_semaphore_win32,
    video_queue/decode h264/h265/av1. It also needs the features AMF's `DeviceVulkan.cpp` sample enables.
  * Create every queue of each family. AMF calls `vkGetDeviceQueue` for queue indices up to 7, and the validation
    layers report each index that was not created.
  * One validation error is left, and AMF causes it: it calls `vkGetMemoryWin32HandleKHR` on the memory imported into
    its device, which was never allocated as exportable. It does not affect the result.
* **`InitVulkan(nullptr)` (AMF creates its own device): FAIL** with or without validation layers. The trace says
  `LoadFunctionsTable() failed: It may be wrong Vulkan driver version` (after a warning that
  `VK_AMD_device_coherent_memory` is missing). So the encoder device has to be created by Knurbs.
* Not tested: AMF's device in a separate `VkInstance`, and GPU-side semaphore sync through `AMFVulkanSync`.

## Building and running

```
cmake -S test/spikes/vulkan-interop -B build-spikes -G "Visual Studio 18 2026" -A x64 ^
      -D SPIKES_VULKAN_HEADERS_DIR=... -D SPIKES_VOLK_DIR=... ^
      -D SPIKES_NVCODEC_HEADERS_DIR=... -D SPIKES_AMF_HEADERS_DIR=...
cmake --build build-spikes --config Release
```

Leave the `*_DIR` variables empty to fetch the dependencies with FetchContent. The shaders are pre-compiled into
`common/shaders/*.h` (`glslang -V --target-env vulkan1.3 --vn <name>`), so no shader compiler is needed.

Environment variables:

* `VK_ADD_LAYER_PATH=<dir with VkLayer_khronos_validation.json>`: validation is enabled automatically when the layer is found.
* `SPIKE_NO_VALIDATION=1`: turns validation off.
* `SPIKE_FFMPEG=<ffmpeg.exe>`: decodes the encoder output and checks its PSNR against the pattern (spikes 3 and 4).
* `SPIKE_GPU=<name substring>`: runs only the matching GPU.
* `SPIKE_FILTER=<case substring>`: runs only the matching cases.

Each program prints a `RESULTS` table and exits with a non-zero code if any case failed.
