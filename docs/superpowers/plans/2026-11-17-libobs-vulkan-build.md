# libobs-vulkan: minimal Windows test build

Verified on Windows 11, VS 2026 (generator `Visual Studio 18 2026`, as the `windows-x64` preset requires), CMake 4.4, from the worktree root. Git Bash syntax.

## One-time setup

The x86 helper build (`build_x86`, spawned automatically by `cmake/windows/architecture.cmake`) needs the `libdshowcapture` submodules:

    git submodule update --init --recursive deps/libdshowcapture/src

## Configure (about 10 minutes the first time, 12 s afterwards)

First run downloads obs-deps (x64, x86) and Qt6 into the git-ignored `.deps/` (about 1.7 GB with extracted copies). Qt is downloaded even with `ENABLE_FRONTEND=OFF`; CEF is not (`ENABLE_BROWSER=OFF`).

    "/c/Program Files/CMake/bin/cmake.exe" --preset windows-x64 -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF -DENABLE_UNIT_TESTS=ON -DENABLE_BROWSER=OFF -DENABLE_CORE_MODULES=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF

`CMAKE_COMPILE_WARNING_AS_ERROR=OFF` is needed because the current MSVC rejects `libobs/obs-properties.c(1252)` (C4244) under /WX.
cmocka comes from the prebuilt obs-deps (`cmocka::cmocka`).

## Build

    "/c/Program Files/CMake/bin/cmake.exe" --build build_x64 --config RelWithDebInfo --target test_serializer test_darray test_bitstream test_os_path -- -m

(builds `libobs` as a dependency; add `--target libobs` to build it alone.)

## Test

The test exes need `obs.dll`, `w32-pthreads.dll` and the obs-deps DLLs on PATH. PowerShell:

    $d = "$PWD\build_x64"
    $env:PATH = "$d\libobs\RelWithDebInfo;$d\deps\w32-pthreads\RelWithDebInfo;$PWD\.deps\obs-deps-2026-10-23-x64\bin;$env:PATH"
    cd build_x64; & "C:\Program Files\CMake\bin\ctest.exe" -C RelWithDebInfo --output-on-failure

Result: 4/4 passed (test_serializer, test_darray, test_bitstream, test_os_path), 0.08 s.

## CMake changes this needed

- Root `CMakeLists.txt`: `ENABLE_UNIT_TESTS` was never wired in; added `enable_testing()` + `add_subdirectory(test/cmocka)` under that option.
- `test/cmocka/CMakeLists.txt`: link `cmocka::cmocka` (`CMOCKA_LIBRARIES` is empty with the config package) and `add_test(<name> <target>)` so ctest finds the multi-config exe.

## glslang (Task 6)

libobs-vulkan compiles shaders at runtime, so glslang is linked **statically** into `libobs-vulkan.dll`; end users need no SDK, glslang DLL or compiler. That makes the glslang static libraries a **build-time** requirement:

- Windows: the Vulkan SDK (`VULKAN_SDK` set, e.g. `C:\VulkanSDK\1.4.363.1`). `find_package(Vulkan COMPONENTS glslang SPIRV-Tools)` finds `glslang.lib` and `SPIRV-Tools.lib`; `glslang-default-resource-limits.lib` and `SPIRV-Tools-opt.lib` are linked by path from the same `Lib/` directory (the SDK's own glslang/SPIRV-Tools CMake packages point at wrong paths and cannot be used). Release libs (/MD) for RelWithDebInfo/Release, the `*d.lib` variants for Debug (Debug is untested).
- Linux (untested): the distro's static glslang and SPIRV-Tools-opt (`glslang-default-resource-limits`, `SPIRV-Tools-opt` are linked by name).
- An OBS deps entry with these static libs would remove the SDK requirement for builders; not done.

Versions used: Vulkan SDK 1.4.363.1, glslang 16.6.0, SPIRV-Tools v2026.4. Licences: see `libobs-vulkan/THIRD_PARTY.md`.
Effect on the module: `libobs-vulkan.dll` grows from 662,528 to 9,105,920 bytes (RelWithDebInfo); `dumpbin /dependents` still lists only obs.dll, the CRT and system DLLs, with vulkan-1.dll delay-loaded.
