cmake_minimum_required(VERSION 3.28...3.30)

# glslang compiles shaders at runtime on the user's machine, so it is linked statically (/MD release libs, debug
# libs for Debug). The SDK's own glslang/SPIRV-Tools CMake packages point at wrong paths, so FindVulkan provides
# glslang and SPIRV-Tools, and the two libraries it does not know are linked by path from the same directory.
find_package(Vulkan REQUIRED COMPONENTS glslang SPIRV-Tools)

# Like the other renderers: a module next to libobs on Windows (libobs-d3d11), a prefix-less shared library in the
# library directory elsewhere (libobs-opengl), because libobs loads it by name with os_dlopen("libobs-vulkan").
if(OS_WINDOWS)
  add_library(libobs-vulkan MODULE)
else()
  add_library(libobs-vulkan SHARED)
endif()
add_library(OBS::libobs-vulkan ALIAS libobs-vulkan)

target_sources(
  libobs-vulkan
  PRIVATE
    vulkan-buffers.cpp
    vulkan-frame.cpp
    vulkan-frame.hpp
    vulkan-instance.cpp
    vulkan-pipeline.cpp
    vulkan-shader.cpp
    vulkan-shaderprocessor.cpp
    vulkan-subsystem.cpp
    vulkan-subsystem.hpp
    vulkan-swapchain.cpp
    vulkan-texture.cpp
    vulkan-tracker.cpp
    vulkan-tracker.hpp
)

target_compile_features(libobs-vulkan PRIVATE cxx_std_17)

if(OS_WINDOWS)
  target_link_libraries(libobs-vulkan PRIVATE OBS::libobs OBS::vma Vulkan::Vulkan Vulkan::glslang Vulkan::SPIRV-Tools)
  cmake_path(GET Vulkan_glslang_LIBRARY PARENT_PATH _vulkan_lib_dir)
  foreach(_lib IN ITEMS glslang-default-resource-limits SPIRV-Tools-opt)
    if(NOT EXISTS "${_vulkan_lib_dir}/${_lib}.lib")
      message(
        FATAL_ERROR
        "libobs-vulkan: ${_vulkan_lib_dir}/${_lib}.lib not found. "
        "Vulkan SDK with glslang static libs required (set VULKAN_SDK)."
      )
    endif()
  endforeach()
  target_link_libraries(
    libobs-vulkan
    PRIVATE
      "${_vulkan_lib_dir}/glslang-default-resource-limits$<$<CONFIG:Debug>:d>.lib"
      "${_vulkan_lib_dir}/SPIRV-Tools-opt$<$<CONFIG:Debug>:d>.lib"
  )
  # The SDK ships the static libs without their PDBs.
  target_link_options(libobs-vulkan PRIVATE /IGNORE:4099)
else()
  # Static archives resolve left to right: SPIRV-Tools-opt needs symbols from SPIRV-Tools, so it must come first.
  target_link_libraries(
    libobs-vulkan
    PRIVATE
      OBS::libobs
      OBS::vma
      Vulkan::Vulkan
      glslang-default-resource-limits
      Vulkan::glslang
      SPIRV-Tools-opt
      Vulkan::SPIRV-Tools
  )
  # Fail at link time, not at dlopen, if a symbol is left unresolved.
  target_link_options(libobs-vulkan PRIVATE LINKER:--no-undefined)
endif()

# Windows delay-loads the loader so a machine without vulkan-1.dll still loads the module and device_create
# returns GS_ERROR_NOT_SUPPORTED. Linux still hard-links libvulkan (untested in this branch).
if(OS_WINDOWS)
  target_link_libraries(libobs-vulkan PRIVATE delayimp)
  target_link_options(libobs-vulkan PRIVATE /DELAYLOAD:vulkan-1.dll)
endif()

target_enable_feature(libobs "Vulkan renderer")

set_target_properties_obs(
  libobs-vulkan
  PROPERTIES FOLDER core
             VERSION 0
             PREFIX ""
             SOVERSION ${OBS_VERSION_MAJOR}
             COMPILE_WARNING_AS_ERROR FALSE
)

# libobs dlopens "libobs-vulkan.so" by that exact name, and nothing links against it, so on Linux it carries no
# version suffix (set_target_properties_obs versions every shared library).
if(NOT OS_WINDOWS)
  set_property(TARGET libobs-vulkan PROPERTY VERSION)
  set_property(TARGET libobs-vulkan PROPERTY SOVERSION)
endif()
