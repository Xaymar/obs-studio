# D3D11 NV12 shared texture → Vulkan import: chroma plane reads back wrong on NVIDIA

A standalone reproducer: one source file, which uses only the Vulkan headers, `vulkan-1.dll`, `d3d11` and `dxgi`.

## What it does

1. It picks a Vulkan physical device (`--gpu`), then creates a D3D11 device on the DXGI adapter with the same LUID
   (`VkPhysicalDeviceIDProperties::deviceLUID` → `IDXGIFactory4::EnumAdapterByLuid`).
2. It creates a `DXGI_FORMAT_NV12` texture (`USAGE_DEFAULT`, `BIND_SHADER_RESOURCE`, plus `BIND_RENDER_TARGET` with `--rt`).
   The texture is shared in one of two ways:
   * `--handle nt` (default): `MISC_SHARED | MISC_SHARED_NTHANDLE`, `IDXGIResource1::CreateSharedHandle`,
     imported as `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`.
   * `--handle kmt`: `MISC_SHARED`, `IDXGIResource::GetSharedHandle`, imported as
     `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT`.

   The pattern is passed as the initial data. A D3D11 staging copy checks it (the control), and an event query
   waits until D3D11 has finished.
3. It creates a `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` image (optimal tiling, `TRANSFER_SRC | SAMPLED`, no create flags)
   with `VkExternalMemoryImageCreateInfo`. It imports the handle into a **dedicated allocation**
   (`VkMemoryDedicatedAllocateInfo` + `VkImportMemoryWin32HandleInfoKHR`) and binds it.
4. It acquires the image from `VK_QUEUE_FAMILY_EXTERNAL` (`GENERAL` → `TRANSFER_SRC_OPTIMAL`, so the contents are kept).
   It copies `PLANE_0` (W×H) and `PLANE_1` (W/2×H/2) to a host-visible buffer with one `vkCmdCopyImageToBuffer`,
   then releases the image back to `VK_QUEUE_FAMILY_EXTERNAL`.
5. It compares the copy with the pattern and prints the first mismatches. It then traces where each received row,
   or each byte run, came from in the D3D11 data, and prints the detected offset of the chroma plane.

Pattern: the NV12 data is treated as H×3/2 rows of W bytes. Rows 0..H-1 are luma and rows H.. are the interleaved
CbCr rows. Byte (row, col) is a 32-bit hash of the pair (`pattern()` in `main.cpp`), so every row is unique and
misplaced data can be traced back to where it came from.

One case runs per process. Running many NV12 imports in one process on NVIDIA ended in
`VK_ERROR_DEVICE_LOST` in our earlier tests. Exit code: `0` match, `1` mismatch, `2` setup error.

## Expected vs observed

**Expected:** Vulkan `PLANE_0` and `PLANE_1` contain exactly the bytes D3D11 holds, as on the AMD control GPU.

**Observed on the RTX 4070 Laptop GPU:**

* **`PLANE_0` (luma) is always correct.**
* **`PLANE_1` (chroma) is wrong in every case**, with NT and KMT handles alike, with or without `BIND_RENDER_TARGET`,
  and on every run. The validation layers report no errors.
  * **1920×1080:** 1032767 of 1036800 chroma bytes differ. This is not a uniform row offset. The received plane is
    made of 64-byte pieces of the real chroma data. Received byte 0 comes from D3D11 chroma row 64 + 1024 bytes.
    Within one received row, the source advances by 512 bytes every 64 bytes and jumps 16 chroma rows every
    few pieces. About 2% of the bytes cannot be found in the D3D11 data at all. Both APIs seem to disagree on the
    chroma plane's base offset *and* its tiling or swizzle. (The earlier spike used a linear pattern, and read
    this as a simple "+64 rows" offset.)
  * **128×64:** the chroma plane reads back as zeros. None of it appears anywhere in the D3D11 data.
* The D3D11 staging readback of the same texture is correct, so D3D11 holds the right data.
* The AMD Radeon 780M passes all four cases (NT/KMT × 1920×1080/128×64) with a byte-exact match.

| Case | RTX 4070 Laptop (621.01) | Radeon 780M (26.11.2) |
|---|---|---|
| NT, 1920×1080 | **FAIL** (chroma pieced together, starts at chroma row 64 + 1024 B) | PASS |
| NT, 128×64 | **FAIL** (chroma all zero) | PASS |
| KMT, 1920×1080 | **FAIL** (same as NT) | PASS |
| KMT, 128×64 | **FAIL** (chroma all zero) | PASS |
| NT, 1920×1080, `--rt` | **FAIL** (same as without `--rt`) | not run |

## Test system (2026-11-20)

* Windows 10 Pro 22H2, build 10.0.19045.6466
* Vulkan loader: `C:\Windows\System32\vulkan-1.dll` 1.4.363.0 (`vkEnumerateInstanceVersion` = 1.4.363)
* **NVIDIA GeForce RTX 4070 Laptop GPU** (PCI 0x10de:0x2860), driver **621.01** (DXGI UMD 32.0.16.2101).
  Vulkan API 1.4.357, `driverVersion` 0x9b404000, conformance 1.4.3.3.
* Control: **AMD Radeon 780M Graphics** (PCI 0x1002:0x15bf, RDNA 3 iGPU, same laptop), Adrenalin
  **26.11.2** (DXGI UMD 32.0.32029.2010). Vulkan "AMD proprietary driver 26.11.2 (LLPC)", API 1.4.359,
  `driverVersion` 0x008001a0.
* MSVC 14.51 (Visual Studio 18 2026), CMake 4.5, Vulkan-Headers 1.4.368.
* Also run with `VK_LAYER_KHRONOS_validation` (built from source, `vulkan-sdk-1.4.363.1`): no validation messages on
  either GPU, and the same results.

## Build and run

```
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 [-D VULKAN_HEADERS_DIR=<Vulkan-Headers checkout>]
cmake --build build --config Release

build\Release\d3d11-nv12-vulkan-import.exe --list
build\Release\d3d11-nv12-vulkan-import.exe                                   # GPU 0, NT handle, 1920x1080
build\Release\d3d11-nv12-vulkan-import.exe --gpu NVIDIA --handle kmt --size 128x64
build\Release\d3d11-nv12-vulkan-import.exe --gpu AMD                         # control
```

Options: `--handle nt|kmt` (default `nt`), `--size WxH` (default `1920x1080`, even values only),
`--gpu <index|name substring>` (default `0`), `--rt` (adds `D3D11_BIND_RENDER_TARGET`), `--list`.
Without `VULKAN_HEADERS_DIR`, CMake uses the Vulkan SDK's headers if it finds them and downloads Vulkan-Headers otherwise.
To enable validation: `set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`.

## Output: RTX 4070 Laptop GPU, default case (FAIL)

```
> d3d11-nv12-vulkan-import.exe --gpu 0 --handle nt --size 1920x1080
Windows        10.0.19045.6466
Vulkan loader  1.4.363
GPU            NVIDIA GeForce RTX 4070 Laptop GPU (vendor 0x10de device 0x2860)
Vulkan driver  NVIDIA 621.01, conformance 1.4.3.3, API 1.4.357, driverVersion 0x9b404000
DXGI adapter   NVIDIA GeForce RTX 4070 Laptop GPU, UMD 32.0.16.2101 (LUID matched)
Case           1920x1080 NV12, NT handle (MISC_SHARED|MISC_SHARED_NTHANDLE -> D3D11_TEXTURE_BIT), D3D11 bind SHADER_RESOURCE
D3D11 staging readback: matches pattern (0 bad rows, RowPitch 1920)
vkGetPhysicalDeviceImageFormatProperties2: VK_SUCCESS, externalMemoryFeatures 0x5 DEDICATED_ONLY IMPORTABLE, compatibleHandleTypes 0x8
image memory requirements: size 3473408, alignment 65536, memoryTypeBits 0x3; handle memoryTypeBits 0x2; prefersDedicated 1 requiresDedicated 1

Comparing Vulkan readback against the D3D11 data:
  mismatch PLANE_1 (x 0, y 0) Cb: got 0x3a expected 0x22
  mismatch PLANE_1 (x 0, y 0) Cr: got 0x79 expected 0x3c
  mismatch PLANE_1 (x 1, y 0) Cb: got 0x1b expected 0x72
  mismatch PLANE_1 (x 1, y 0) Cr: got 0x25 expected 0xcd
  mismatch PLANE_1 (x 2, y 0) Cb: got 0x7f expected 0x6e
  mismatch PLANE_1 (x 2, y 0) Cr: got 0xe4 expected 0x3c
  mismatch PLANE_1 (x 3, y 0) Cb: got 0x27 expected 0x3d
  mismatch PLANE_1 (x 3, y 0) Cr: got 0x95 expected 0x32
PLANE_0 (Y):    0 of 2073600 bytes differ
PLANE_1 (CbCr): 1032767 of 1036800 bytes differ

Where the received rows come from:
  PLANE_0 rows 0..1079 = D3D11 luma row 0..D3D11 luma row 1079  (correct)
  PLANE_1 rows 0..539: no matching D3D11 row (row 0 starts: 3a 79 1b 25 7f e4 27 95 df bc 57 9b )
  PLANE_1 byte trace (received plane offset <- D3D11 linear offset, pitch 1920):
    [0..64) row 0+0 <- chroma row 64 + 1024 (delta +2197504 bytes), 64 bytes
    [64..128) row 0+64 <- chroma row 64 + 1536 (delta +2197952 bytes), 64 bytes
    [128..192) row 0+128 <- chroma row 80 + 128 (delta +2227200 bytes), 64 bytes
    [192..256) row 0+192 <- chroma row 80 + 640 (delta +2227648 bytes), 64 bytes
    [256..320) row 0+256 <- chroma row 80 + 1152 (delta +2228096 bytes), 64 bytes
    [320..384) row 0+320 <- chroma row 80 + 1664 (delta +2228544 bytes), 64 bytes
    [384..448) row 0+384 <- chroma row 96 + 256 (delta +2257792 bytes), 64 bytes
    [448..512) row 0+448 <- chroma row 96 + 768 (delta +2258240 bytes), 64 bytes
    [512..576) row 0+512 <- chroma row 96 + 1280 (delta +2258688 bytes), 64 bytes
    [576..640) row 0+576 <- chroma row 96 + 1792 (delta +2259136 bytes), 64 bytes
    [640..704) row 0+640 <- chroma row 112 + 384 (delta +2288384 bytes), 64 bytes
    [704..768) row 0+704 <- chroma row 112 + 896 (delta +2288832 bytes), 64 bytes
    [768..832) row 0+768 <- chroma row 112 + 1408 (delta +2289280 bytes), 64 bytes
    [832..896) row 0+832 <- chroma row 128 + 0 (delta +2318528 bytes), 64 bytes
    [896..960) row 0+896 <- chroma row 128 + 512 (delta +2318976 bytes), 64 bytes
    [960..1024) row 0+960 <- chroma row 128 + 1024 (delta +2319424 bytes), 64 bytes
    [1024..1088) row 0+1024 <- chroma row 128 + 1536 (delta +2319872 bytes), 64 bytes
    [1088..1152) row 0+1088 <- chroma row 144 + 128 (delta +2349120 bytes), 64 bytes
    [1152..1216) row 0+1152 <- chroma row 144 + 640 (delta +2349568 bytes), 64 bytes
    [1216..1280) row 0+1216 <- chroma row 144 + 1152 (delta +2350016 bytes), 64 bytes
    [1280..1344) row 0+1280 <- chroma row 144 + 1664 (delta +2350464 bytes), 64 bytes
    [1344..1408) row 0+1344 <- chroma row 160 + 256 (delta +2379712 bytes), 64 bytes
    [1408..1472) row 0+1408 <- chroma row 160 + 768 (delta +2380160 bytes), 64 bytes
    [1472..1536) row 0+1472 <- chroma row 160 + 1280 (delta +2380608 bytes), 64 bytes
    ... (further segments not shown)
  PLANE_1: 1016833 of 1036800 bytes traced to D3D11 data in 21479 segments (longest 65 bytes), 19967 not found
Detected chroma row offset: not a uniform shift. Vulkan PLANE_1 byte 0 = D3D11 chroma row 64 + 1024 bytes (+64 rows), the rest of the plane is pieced together from other rows (see byte trace)

RESULT: FAIL (Vulkan readback differs from D3D11 data)
```

The KMT handle (`--handle kmt`) gives the same output, apart from `compatibleHandleTypes 0x10`. The mismatch count
can change by a few bytes between runs (1032767 or 1032769 here), because the bytes that cannot be traced are not stable.
The byte trace stays the same.

## Output: RTX 4070 Laptop GPU, 128×64 (FAIL)

```
> d3d11-nv12-vulkan-import.exe --gpu 0 --handle nt --size 128x64
...
Case           128x64 NV12, NT handle (MISC_SHARED|MISC_SHARED_NTHANDLE -> D3D11_TEXTURE_BIT), D3D11 bind SHADER_RESOURCE
D3D11 staging readback: matches pattern (0 bad rows, RowPitch 128)
vkGetPhysicalDeviceImageFormatProperties2: VK_SUCCESS, externalMemoryFeatures 0x5 DEDICATED_ONLY IMPORTABLE, compatibleHandleTypes 0x8
image memory requirements: size 131072, alignment 65536, memoryTypeBits 0x3; handle memoryTypeBits 0x2; prefersDedicated 1 requiresDedicated 1

Comparing Vulkan readback against the D3D11 data:
  mismatch PLANE_1 (x 0, y 0) Cb: got 0x00 expected 0x21
  mismatch PLANE_1 (x 0, y 0) Cr: got 0x00 expected 0x77
  mismatch PLANE_1 (x 1, y 0) Cb: got 0x00 expected 0x47
  mismatch PLANE_1 (x 1, y 0) Cr: got 0x00 expected 0xbc
  mismatch PLANE_1 (x 2, y 0) Cb: got 0x00 expected 0xf8
  mismatch PLANE_1 (x 2, y 0) Cr: got 0x00 expected 0x5d
  mismatch PLANE_1 (x 3, y 0) Cb: got 0x00 expected 0x63
  mismatch PLANE_1 (x 3, y 0) Cr: got 0x00 expected 0x22
PLANE_0 (Y):    0 of 8192 bytes differ
PLANE_1 (CbCr): 4075 of 4096 bytes differ

Where the received rows come from:
  PLANE_0 rows 0..63 = D3D11 luma row 0..D3D11 luma row 63  (correct)
  PLANE_1 rows 0..31: no matching D3D11 row (row 0 starts: 00 00 00 00 00 00 00 00 00 00 00 00 )
  PLANE_1 byte trace (received plane offset <- D3D11 linear offset, pitch 128):
  PLANE_1: 0 of 4096 bytes traced to D3D11 data in 0 segments (longest 0 bytes), 4096 not found
Detected chroma row offset: none (PLANE_1 byte 0 is not found anywhere in the D3D11 data)

RESULT: FAIL (Vulkan readback differs from D3D11 data)
```

(The 21 chroma bytes that "match" are most likely pattern bytes that happen to be 0x00. 4096 random bytes would contain about 16 of them.)

## Output: Radeon 780M, control (PASS)

```
> d3d11-nv12-vulkan-import.exe --gpu 1 --handle nt --size 1920x1080
Windows        10.0.19045.6466
Vulkan loader  1.4.363
GPU            AMD Radeon 780M Graphics (vendor 0x1002 device 0x15bf)
Vulkan driver  AMD proprietary driver 26.11.2 (LLPC), conformance 1.4.3.3, API 1.4.359, driverVersion 0x008001a0
DXGI adapter   AMD Radeon 780M Graphics, UMD 32.0.32029.2010 (LUID matched)
Case           1920x1080 NV12, NT handle (MISC_SHARED|MISC_SHARED_NTHANDLE -> D3D11_TEXTURE_BIT), D3D11 bind SHADER_RESOURCE
D3D11 staging readback: matches pattern (0 bad rows, RowPitch 2048)
vkGetPhysicalDeviceImageFormatProperties2: VK_SUCCESS, externalMemoryFeatures 0x5 DEDICATED_ONLY IMPORTABLE, compatibleHandleTypes 0x8
image memory requirements: size 3932160, alignment 65536, memoryTypeBits 0x1; handle memoryTypeBits 0x1111; prefersDedicated 1 requiresDedicated 1

Comparing Vulkan readback against the D3D11 data:
PLANE_0 (Y):    0 of 2073600 bytes differ
PLANE_1 (CbCr): 0 of 1036800 bytes differ

Where the received rows come from:
  PLANE_0 rows 0..1079 = D3D11 luma row 0..D3D11 luma row 1079  (correct)
  PLANE_1 rows 0..539 = D3D11 chroma row 0..D3D11 chroma row 539  (correct)
Detected chroma row offset: +0 (Vulkan PLANE_1 row 0 = D3D11 chroma row 0; +0 bytes at pitch 1920)

RESULT: PASS (Vulkan sees exactly what D3D11 wrote)
```

`--handle kmt` and `--size 128x64` also pass on the 780M, with the same byte-exact result.
