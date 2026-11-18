cmake_minimum_required(VERSION 3.28...3.30)

find_package(Vulkan REQUIRED)

add_library(libobs-vulkan MODULE)
add_library(OBS::libobs-vulkan ALIAS libobs-vulkan)

target_sources(
  libobs-vulkan
  PRIVATE
    vulkan-frame.cpp
    vulkan-frame.hpp
    vulkan-instance.cpp
    vulkan-subsystem.cpp
    vulkan-subsystem.hpp
    vulkan-tracker.cpp
    vulkan-tracker.hpp
)

target_compile_features(libobs-vulkan PRIVATE cxx_std_17)

target_link_libraries(libobs-vulkan PRIVATE OBS::libobs OBS::vma Vulkan::Vulkan)

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
