# Third-party code linked into libobs-vulkan

These are linked statically into `libobs-vulkan.dll` (from the Vulkan SDK at build time), so their licences must ship with OBS builds that include the Vulkan renderer.

| Component | Version | Used for | Licence |
|---|---|---|---|
| glslang (`glslang.lib`, `glslang-default-resource-limits.lib`) | 16.6.0 (Vulkan SDK 1.4.363.1) | GLSL to SPIR-V compilation, reflection | Mixed, per its `LICENSE.txt`: BSD-3-Clause (most files), BSD-2-Clause, MIT, Apache-2.0, and GPL-3.0-or-later **with the Bison special exception** for the generated parser (the exception allows distributing it under the other terms) |
| SPIRV-Tools (`SPIRV-Tools.lib`, `SPIRV-Tools-opt.lib`) | v2026.4 (v2026.4-0-gef96ed76) | Pulled in by glslang's SPIR-V back end; the optimizer and validator are not called | Apache-2.0 |
| SPIRV-Headers (headers only, via glslang/SPIRV-Tools) | as bundled with the SDK | SPIR-V enums | MIT-style Khronos licence |
| Vulkan Memory Allocator | see `deps/` (OBS::vma) | GPU memory | MIT |

The SDK does not ship the licence texts (only `Licenses/LICENSE.txt`, which points to the LunarG licensing registry). The full texts must be taken from the upstream repositories at the tags above (KhronosGroup/glslang, KhronosGroup/SPIRV-Tools, KhronosGroup/SPIRV-Headers) when packaging; they were not downloaded here.
