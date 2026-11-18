cmake_minimum_required(VERSION 3.28...3.30)

find_package(Vulkan REQUIRED)

add_library(libobs-vulkan MODULE)
add_library(OBS::libobs-vulkan ALIAS libobs-vulkan)

target_sources(libobs-vulkan PRIVATE vulkan-subsystem.cpp vulkan-subsystem.hpp)

target_compile_features(libobs-vulkan PRIVATE cxx_std_17)

target_link_libraries(libobs-vulkan PRIVATE OBS::libobs Vulkan::Vulkan)

target_enable_feature(libobs "Vulkan renderer")

set_target_properties_obs(
  libobs-vulkan
  PROPERTIES FOLDER core
             VERSION 0
             SOVERSION ${OBS_VERSION_MAJOR}
             COMPILE_WARNING_AS_ERROR FALSE
)
