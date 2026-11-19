# Vulkan interop spikes for the Knurbs Vulkan-only backend plan. Standalone, not part of the main build.
#
#   cmake -S test/spikes/vulkan-interop -B build-spikes [-D SPIKES_<DEP>_DIR=...]
#   cmake --build build-spikes --config Release
#
# Dependencies are header-only (plus volk). Point the *_DIR variables at existing checkouts,
# or leave them empty to have FetchContent download them. Nothing links against a Vulkan SDK:
# vulkan-1.dll, nvcuda.dll, nvEncodeAPI64.dll and amfrt64.dll are all loaded at runtime.
cmake_minimum_required(VERSION 3.24)
project(knurbs_vulkan_interop_spikes CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(SPIKES_VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers checkout (contains include/vulkan)")
set(SPIKES_VOLK_DIR "" CACHE PATH "volk checkout (contains volk.c/volk.h)")
set(SPIKES_NVCODEC_HEADERS_DIR "" CACHE PATH "FFmpeg nv-codec-headers checkout (contains include/ffnvcodec)")
set(SPIKES_AMF_HEADERS_DIR "" CACHE PATH "AMF public include dir (contains core/ and components/)")

function(spike_dep var name repo tag)
  if(NOT ${var})
    FetchContent_Declare(${name} GIT_REPOSITORY ${repo} GIT_TAG ${tag} GIT_SHALLOW TRUE)
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
      FetchContent_Populate(${name})
    endif()
    set(${var} "${${name}_SOURCE_DIR}" PARENT_SCOPE)
  endif()
endfunction()

spike_dep(SPIKES_VULKAN_HEADERS_DIR vulkan_headers https://github.com/KhronosGroup/Vulkan-Headers.git main)
spike_dep(SPIKES_VOLK_DIR volk https://github.com/zeux/volk.git master)
spike_dep(SPIKES_NVCODEC_HEADERS_DIR nvcodec https://github.com/FFmpeg/nv-codec-headers.git master)
if(NOT SPIKES_AMF_HEADERS_DIR)
  spike_dep(SPIKES_AMF_ROOT amf https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git master)
  set(SPIKES_AMF_HEADERS_DIR "${SPIKES_AMF_ROOT}/amf/public/include")
endif()

message(STATUS "Vulkan-Headers: ${SPIKES_VULKAN_HEADERS_DIR}")
message(STATUS "volk:           ${SPIKES_VOLK_DIR}")
message(STATUS "nv-codec-hdrs:  ${SPIKES_NVCODEC_HEADERS_DIR}")
message(STATUS "AMF headers:    ${SPIKES_AMF_HEADERS_DIR}")

add_library(spike_volk STATIC "${SPIKES_VOLK_DIR}/volk.c")
target_include_directories(spike_volk PUBLIC "${SPIKES_VOLK_DIR}" "${SPIKES_VULKAN_HEADERS_DIR}/include")
target_compile_definitions(spike_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

add_library(spike_common INTERFACE)
target_link_libraries(spike_common INTERFACE spike_volk)
target_include_directories(spike_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common")
if(MSVC)
  target_compile_options(spike_common INTERFACE /W3 /utf-8)
endif()

foreach(spike spike1-plane-views spike2-d3d11-import spike3-cuda-nvenc spike4-amf-vulkan)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${spike}/main.cpp")
    add_subdirectory(${spike})
  endif()
endforeach()
