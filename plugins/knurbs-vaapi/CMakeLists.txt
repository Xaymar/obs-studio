cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_VAAPI "Build VA-API Hardware Encoder Plugin" ON)

if(NOT ENABLE_VAAPI)
  target_disable_feature(knurbs-vaapi "VA-API Hardware Encoder")
  return()
endif()

find_package(FFmpeg 6.1 REQUIRED avcodec avfilter avformat avutil)
find_package(Libva REQUIRED)
find_package(Libpci REQUIRED)
find_package(Libdrm REQUIRED)

if(NOT TARGET OBS::ffmpeg-formats)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/ffmpeg-formats" "${CMAKE_BINARY_DIR}/shared/ffmpeg-formats")
endif()

if(NOT TARGET OBS::opts-parser)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/opts-parser" "${CMAKE_BINARY_DIR}/shared/opts-parser")
endif()

add_library(knurbs-vaapi MODULE)
add_library(OBS::knurbs-vaapi ALIAS knurbs-vaapi)

target_sources(
  knurbs-vaapi
  PRIVATE knurbs-vaapi.c vaapi-encoder.c vaapi-ids.h vaapi-utils.c vaapi-utils.h vaapi-vendor.c vaapi-vendor.h
)

target_link_libraries(
  knurbs-vaapi
  PRIVATE
    OBS::libobs
    OBS::ffmpeg-formats
    OBS::opts-parser
    FFmpeg::avcodec
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avutil
    Libva::va
    Libva::drm
    Libpci::pci
    Libdrm::Libdrm
)

set_target_properties_obs(knurbs-vaapi PROPERTIES FOLDER plugins/knurbs-vaapi PREFIX "")
