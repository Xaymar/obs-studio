cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_QSV11 "Build Intel QSV Hardware Encoder Plugin" TRUE)
if(NOT ENABLE_QSV11)
  target_disable_feature(knurbs-qsv "Intel QSV Hardware Encoder")
  return()
endif()

add_library(knurbs-qsv MODULE)
add_library(OBS::knurbs-qsv ALIAS knurbs-qsv)

find_package(VPL 2.9 REQUIRED)

if(OS_LINUX)
  find_package(Libva REQUIRED)
endif()

target_sources(
  knurbs-qsv
  PRIVATE
    $<$<PLATFORM_ID:Linux>:common_utils_linux.cpp>
    $<$<PLATFORM_ID:Windows>:common_directx11.cpp>
    $<$<PLATFORM_ID:Windows>:common_directx11.h>
    $<$<PLATFORM_ID:Windows>:common_utils_windows.cpp>
    common_utils.cpp
    common_utils.h
    knurbs-qsv-plugin-main.c
    knurbs-qsv.c
    qsv-aliases.c
    qsv-migrate.c
    qsv-migrate.h
    QSV_Encoder.cpp
    QSV_Encoder.h
    QSV_Encoder_Internal.cpp
    QSV_Encoder_Internal.h
)

target_compile_options(
  knurbs-qsv
  PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wno-error=switch>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wno-error=switch>
)

target_link_libraries(
  knurbs-qsv
  PRIVATE
    OBS::libobs
    VPL::VPL
    $<$<PLATFORM_ID:Linux>:Libva::va>
    $<$<PLATFORM_ID:Linux>:Libva::drm>
    $<$<PLATFORM_ID:Windows>:d3d11>
    $<$<PLATFORM_ID:Windows>:dxgi>
    $<$<PLATFORM_ID:Windows>:dxguid>
)

target_link_options(knurbs-qsv PRIVATE $<$<PLATFORM_ID:Windows>:/IGNORE:4099>)

if(OS_WINDOWS)
  add_subdirectory(knurbs-qsv-test)

  configure_file(cmake/windows/obs-module.rc.in knurbs-qsv.rc)
  target_sources(knurbs-qsv PRIVATE knurbs-qsv.rc)
endif()

set_target_properties_obs(knurbs-qsv PROPERTIES FOLDER plugins/knurbs-qsv PREFIX "")
