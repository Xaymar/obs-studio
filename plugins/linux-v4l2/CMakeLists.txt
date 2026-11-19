cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_V4L2 "Build OBS with v4l2 support" ON)

if(NOT ENABLE_V4L2)
  return()
endif()

if(OS_FREEBSD OR OS_OPENBSD)
  set(CMAKE_REQUIRED_INCLUDES "/usr/local/include")
endif()
include(CheckCSourceCompiles)
set(V4L_TEST_SOURCE "#include<linux/videodev2.h>\nint main(){return 0;}")
check_c_source_compiles("${V4L_TEST_SOURCE}" HAVE_VIDEODEV2_HEADER)

if(NOT HAVE_VIDEODEV2_HEADER)
  message(FATAL_ERROR "Required system header <linux/videodev2.h> not found.")
endif()

add_library(linux-v4l2 MODULE)
add_library(OBS::v4l2 ALIAS linux-v4l2)

# Capture lives in capture-video-device now; this module is the virtual camera.
target_sources(linux-v4l2 PRIVATE linux-v4l2.c v4l2-output.c)

target_link_libraries(linux-v4l2 PRIVATE OBS::libobs)

set_target_properties_obs(linux-v4l2 PROPERTIES FOLDER plugins PREFIX "")
