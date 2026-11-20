cmake_minimum_required(VERSION 3.28...3.30)

add_library(win-dshow MODULE)
add_library(OBS::dshow ALIAS win-dshow)

if(NOT TARGET OBS::libdshowcapture)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/libdshowcapture" libdshowcapture)
endif()

if(NOT TARGET OBS::winhandle)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" "${CMAKE_BINARY_DIR}/libobs")
endif()

# The video capture source is the DirectShow backend of
# knurbs-video-device-capture; only the encoders are left here.
target_sources(win-dshow PRIVATE dshow-plugin.cpp win-dshow-encoder.cpp)

configure_file(cmake/windows/obs-module.rc.in win-dshow.rc)
target_sources(win-dshow PRIVATE win-dshow.rc)

target_link_libraries(
  win-dshow
  PRIVATE
    OBS::libobs
    OBS::w32-pthreads
    OBS::libdshowcapture
    OBS::winhandle
    strmiids
    winmm
)

set_target_properties_obs(win-dshow PROPERTIES FOLDER plugins/win-dshow PREFIX "")
