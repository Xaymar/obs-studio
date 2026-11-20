# Initial cache for an LLVM source-based coverage build of libobs and its unit tests.
#
# Use with the Visual Studio generator and the ClangCL toolset (the Windows build requires a VS generator):
#   cmake -S . -B build_clangcov -G "Visual Studio 18 2026" -A x64 -T ClangCL -C test/coverage/clangcov.cmake
# test/coverage/run-coverage.ps1 does this, builds, runs ctest and writes the reports.

if(NOT DEFINED LLVM_ROOT)
  set(LLVM_ROOT "C:/Program Files/LLVM")
endif()

# Newest clang resource dir under LLVM_ROOT, e.g. lib/clang/23
file(GLOB _clang_resource_dirs LIST_DIRECTORIES TRUE "${LLVM_ROOT}/lib/clang/*")
list(SORT _clang_resource_dirs COMPARE NATURAL ORDER DESCENDING)
list(GET _clang_resource_dirs 0 _clang_resource_dir)
get_filename_component(_llvm_major "${_clang_resource_dir}" NAME)
file(TO_NATIVE_PATH "${LLVM_ROOT}" _llvm_root_native)

set(CMAKE_VS_GLOBALS "LLVMInstallDir=${_llvm_root_native};LLVMToolsVersion=${_llvm_major}" CACHE STRING "")
set(ENABLE_CCACHE OFF CACHE BOOL "")

set(ENABLE_UNIT_TESTS ON CACHE BOOL "")
set(ENABLE_TEST_OBS_MEM ON CACHE BOOL "")
set(ENABLE_FRONTEND OFF CACHE BOOL "")
set(ENABLE_BROWSER OFF CACHE BOOL "")
set(ENABLE_SCRIPTING OFF CACHE BOOL "")
set(ENABLE_PLUGINS OFF CACHE BOOL "")
set(VIRTUALCAM_GUID "A3FCE0F5-3493-419F-958A-ABA1250EC20B" CACHE STRING "")

# clang-cl reports warnings MSVC does not; keep them visible but non-fatal for coverage runs
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "")

# Setting these replaces CMake's defaults, so those are repeated here
set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS -fprofile-instr-generate -fcoverage-mapping" CACHE STRING "")
set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /EHsc /GR -fprofile-instr-generate -fcoverage-mapping" CACHE STRING "")

# VS links with link.exe, which does not add the profile runtime on its own
set(_cov_link_flags "\"/LIBPATH:${_clang_resource_dir}/lib/windows\" clang_rt.profile-x86_64.lib")
set(CMAKE_SHARED_LINKER_FLAGS "${_cov_link_flags}" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "${_cov_link_flags}" CACHE STRING "")
set(CMAKE_MODULE_LINKER_FLAGS "${_cov_link_flags}" CACHE STRING "")
