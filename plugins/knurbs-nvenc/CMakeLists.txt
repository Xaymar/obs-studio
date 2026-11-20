cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_NVENC "Build NVIDIA Hardware Encoder Plugin" ON)

if(NOT ENABLE_NVENC)
  target_disable_feature(knurbs-nvenc "NVIDIA Hardware Encoder")
  return()
endif()

if(NOT TARGET OBS::opts-parser)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/opts-parser" "${CMAKE_BINARY_DIR}/shared/opts-parser")
endif()

if(OS_LINUX AND NOT TARGET OBS::glad)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/glad" "${CMAKE_BINARY_DIR}/deps/glad")
endif()

find_package(FFnvcodec 12 REQUIRED)

add_library(knurbs-nvenc MODULE)
add_library(OBS::nvenc ALIAS knurbs-nvenc)

add_subdirectory(knurbs-nvenc-test)

target_sources(
  knurbs-nvenc
  PRIVATE
    $<$<PLATFORM_ID:Linux>:nvenc-opengl.c>
    $<$<PLATFORM_ID:Windows>:nvenc-d3d11.c>
    cuda-helpers.c
    cuda-helpers.h
    knurbs-nvenc.c
    knurbs-nvenc.h
    nvenc-aliases.c
    nvenc-cuda.c
    nvenc-helpers.c
    nvenc-helpers.h
    nvenc-internal.h
    nvenc-migrate.c
    nvenc-migrate.h
    nvenc-opts-parser.c
    nvenc-properties.c
    nvenc.c
)

target_link_libraries(
  knurbs-nvenc
  PRIVATE OBS::libobs OBS::opts-parser FFnvcodec::FFnvcodec $<$<PLATFORM_ID:Linux>:OBS::glad>
)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in knurbs-nvenc.rc)
  target_sources(knurbs-nvenc PRIVATE knurbs-nvenc.rc)
endif()

set_target_properties_obs(knurbs-nvenc PROPERTIES FOLDER plugins/knurbs-nvenc PREFIX "")
