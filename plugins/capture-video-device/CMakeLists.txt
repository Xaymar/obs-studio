cmake_minimum_required(VERSION 3.28...3.30)

add_library(capture-video-device MODULE)
add_library(OBS::capture-video-device ALIAS capture-video-device)

target_sources(
  capture-video-device
  PRIVATE backends.c migrate.c migrate.h plugin-main.c video-device.c video-device.h
)

target_link_libraries(capture-video-device PRIVATE OBS::libobs)

set_target_properties_obs(capture-video-device PROPERTIES FOLDER plugins PREFIX "")
