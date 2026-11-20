cmake_minimum_required(VERSION 3.28...3.30)

# PipeWire stream, format and portal helpers shared by the PipeWire screen
# capture in linux-pipewire and the PipeWire camera backend of
# knurbs-video-device-capture.

find_package(PipeWire 0.3.33 REQUIRED)
find_package(Gio 2.76 REQUIRED)
find_package(Libdrm REQUIRED)
get_target_property(libdrm_include_directories Libdrm::Libdrm INTERFACE_INCLUDE_DIRECTORIES)

add_library(pipewire-common OBJECT)
add_library(OBS::pipewire-common ALIAS pipewire-common)

target_sources(
  pipewire-common
  PRIVATE formats.c pipewire.c portal.c
  PUBLIC formats.h pipewire.h portal.h
)

target_include_directories(pipewire-common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" ${libdrm_include_directories})

target_link_libraries(pipewire-common PUBLIC OBS::libobs OBS::glad PipeWire::PipeWire gio::gio)

set_target_properties(pipewire-common PROPERTIES FOLDER deps POSITION_INDEPENDENT_CODE TRUE)
