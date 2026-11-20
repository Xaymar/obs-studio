# Standalone reproducer: D3D11 NV12 shared texture -> Vulkan import.
#
#   cmake -S . -B build [-D VULKAN_HEADERS_DIR=<Vulkan-Headers checkout>]
#   cmake --build build --config Release
#
# Needs only the Vulkan headers. vulkan-1.dll (the loader) is loaded at runtime, so no Vulkan SDK
# or import library is required. Without VULKAN_HEADERS_DIR, the Vulkan SDK's headers are used if
# found, otherwise Vulkan-Headers is downloaded.
cmake_minimum_required(VERSION 3.20)
project(d3d11_nv12_vulkan_import CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers checkout (contains include/vulkan/vulkan.h)")
if(VULKAN_HEADERS_DIR)
  set(VK_INCLUDE "${VULKAN_HEADERS_DIR}/include")
else()
  find_package(Vulkan QUIET)
  if(Vulkan_INCLUDE_DIRS)
    set(VK_INCLUDE "${Vulkan_INCLUDE_DIRS}")
  else()
    include(FetchContent)
    FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                                        GIT_TAG main GIT_SHALLOW TRUE)
    FetchContent_GetProperties(vulkan_headers)
    if(NOT vulkan_headers_POPULATED)
      FetchContent_Populate(vulkan_headers)
    endif()
    set(VK_INCLUDE "${vulkan_headers_SOURCE_DIR}/include")
  endif()
endif()

add_executable(d3d11-nv12-vulkan-import main.cpp)
target_include_directories(d3d11-nv12-vulkan-import PRIVATE "${VK_INCLUDE}")
target_link_libraries(d3d11-nv12-vulkan-import PRIVATE d3d11 dxgi)
target_compile_definitions(d3d11-nv12-vulkan-import PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(d3d11-nv12-vulkan-import PRIVATE /W3 /utf-8)
endif()
