cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PIPEWIRE "Enable PipeWire support" ON)
if(NOT ENABLE_PIPEWIRE)
  return()
endif()

if(NOT TARGET OBS::pipewire-common)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/pipewire" "${CMAKE_BINARY_DIR}/shared/pipewire")
endif()

# For PipeWire_VERSION in this scope.
find_package(PipeWire 0.3.33 REQUIRED)

add_library(linux-pipewire MODULE)
add_library(OBS::pipewire ALIAS linux-pipewire)

if(PipeWire_VERSION VERSION_GREATER_EQUAL 0.3.60)
  target_enable_feature(linux-pipewire "PipeWire 0.3.60+ camera support")
  set(_HAS_PIPEWIRE_CAMERA TRUE)
else()
  set(_HAS_PIPEWIRE_CAMERA FALSE)
endif()

target_sources(
  linux-pipewire
  PRIVATE
    $<$<BOOL:${_HAS_PIPEWIRE_CAMERA}>:camera-portal.c>
    $<$<BOOL:${_HAS_PIPEWIRE_CAMERA}>:camera-portal.h>
    linux-pipewire.c
    screencast-portal.c
    screencast-portal.h
)

target_link_libraries(linux-pipewire PRIVATE OBS::libobs OBS::glad OBS::pipewire-common)

set_target_properties_obs(linux-pipewire PROPERTIES FOLDER plugins PREFIX "")
