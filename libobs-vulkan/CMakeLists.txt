cmake_minimum_required(VERSION 3.28...3.30)

# glslang compiles shaders at runtime on the user's machine, so it is linked statically (/MD release libs, debug
# libs for Debug). The SDK's own glslang/SPIRV-Tools CMake packages point at wrong paths, so FindVulkan provides
# glslang and SPIRV-Tools, and the two libraries it does not know are linked by path from the same directory.
find_package(Vulkan REQUIRED COMPONENTS glslang SPIRV-Tools)

add_library(libobs-vulkan MODULE)
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

target_link_libraries(libobs-vulkan PRIVATE OBS::libobs OBS::vma Vulkan::Vulkan Vulkan::glslang Vulkan::SPIRV-Tools)
if(OS_WINDOWS)
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
  # Static archives resolve left to right: SPIRV-Tools-opt needs SPIRV-Tools after it, and the module must not
  # load with unresolved spvtools symbols.
  target_link_libraries(libobs-vulkan PRIVATE glslang-default-resource-limits SPIRV-Tools-opt Vulkan::SPIRV-Tools)
  # The module exports gs_* under the same names as libobs. Without -Bsymbolic its internal calls (e.g.
  # gs_shader_set_val from device_draw) bind to libobs's wrappers, which drop them outside a graphics context.
  target_link_options(libobs-vulkan PRIVATE LINKER:--no-undefined LINKER:-Bsymbolic)
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
             SOVERSION ${OBS_VERSION_MAJOR}
             COMPILE_WARNING_AS_ERROR FALSE
)
