cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_AMF "Build AMD AMF Hardware Encoder Plugin" ON)

if(NOT ENABLE_AMF)
  target_disable_feature(knurbs-amf "AMD AMF Hardware Encoder")
  return()
endif()

find_package(AMF 1.4.29 REQUIRED)

if(NOT TARGET OBS::opts-parser)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/opts-parser" "${CMAKE_BINARY_DIR}/shared/opts-parser")
endif()

add_library(knurbs-amf MODULE)
add_library(OBS::knurbs-amf ALIAS knurbs-amf)

add_subdirectory(knurbs-amf-test)

target_sources(knurbs-amf PRIVATE amf-encoder-opts.hpp amf-encoder.cpp amf-ids.h knurbs-amf.c)

target_link_libraries(knurbs-amf PRIVATE OBS::libobs OBS::opts-parser AMF::AMF)

configure_file(cmake/windows/obs-module.rc.in knurbs-amf.rc)
target_sources(knurbs-amf PRIVATE knurbs-amf.rc)

set_target_properties_obs(knurbs-amf PROPERTIES FOLDER plugins/knurbs-amf PREFIX "")
