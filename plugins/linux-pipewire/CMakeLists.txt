cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PIPEWIRE "Enable PipeWire support" ON)
if(NOT ENABLE_PIPEWIRE)
  return()
endif()

if(NOT TARGET OBS::pipewire-common)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/pipewire" "${CMAKE_BINARY_DIR}/shared/pipewire")
endif()

add_library(linux-pipewire MODULE)
add_library(OBS::pipewire ALIAS linux-pipewire)

target_sources(
  linux-pipewire
  PRIVATE
    linux-pipewire.c
    screencast-portal.c
    screencast-portal.h
)

target_link_libraries(linux-pipewire PRIVATE OBS::libobs OBS::glad OBS::pipewire-common)

set_target_properties_obs(linux-pipewire PROPERTIES FOLDER plugins PREFIX "")
