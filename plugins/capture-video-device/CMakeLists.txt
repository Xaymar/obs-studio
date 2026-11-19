cmake_minimum_required(VERSION 3.28...3.30)

add_library(capture-video-device MODULE)
add_library(OBS::capture-video-device ALIAS capture-video-device)

target_sources(
  capture-video-device
  PRIVATE backends.c migrate.c migrate.h plugin-main.c video-device.c video-device.h
)

target_include_directories(capture-video-device PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(capture-video-device PRIVATE OBS::libobs)

if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  option(ENABLE_V4L2 "Build OBS with v4l2 support" ON)
  option(ENABLE_UDEV "Build the V4L2 backend with UDEV support" ON)

  if(ENABLE_V4L2)
    find_package(Libv4l2 REQUIRED)
    find_package(FFmpeg 8.0 REQUIRED COMPONENTS avcodec avutil avformat)

    if(OS_FREEBSD OR OS_OPENBSD)
      set(CMAKE_REQUIRED_INCLUDES "/usr/local/include")
    endif()
    include(CheckCSourceCompiles)
    set(V4L_TEST_SOURCE "#include<linux/videodev2.h>\nint main(){return 0;}")
    check_c_source_compiles("${V4L_TEST_SOURCE}" HAVE_VIDEODEV2_HEADER)
    if(NOT HAVE_VIDEODEV2_HEADER)
      message(FATAL_ERROR "Required system header <linux/videodev2.h> not found.")
    endif()

    target_sources(
      capture-video-device
      PRIVATE
        backends/v4l2/v4l2-backend.c
        backends/v4l2/v4l2-controls.c
        backends/v4l2/v4l2-controls.h
        backends/v4l2/v4l2-decoder.c
        backends/v4l2/v4l2-decoder.h
        backends/v4l2/v4l2-helpers.c
        backends/v4l2/v4l2-helpers.h
    )
    target_link_libraries(
      capture-video-device
      PRIVATE Libv4l2::Libv4l2 FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
    )
    target_compile_definitions(capture-video-device PRIVATE HAVE_V4L2)

    if(ENABLE_UDEV)
      find_package(Libudev REQUIRED)
      target_sources(capture-video-device PRIVATE backends/v4l2/v4l2-udev.c backends/v4l2/v4l2-udev.h)
      target_link_libraries(capture-video-device PRIVATE Libudev::Libudev)
      target_compile_definitions(capture-video-device PRIVATE HAVE_UDEV)
    endif()
  endif()
endif()

set_target_properties_obs(capture-video-device PROPERTIES FOLDER plugins PREFIX "")
