cmake_minimum_required(VERSION 3.28...3.30)

add_library(knurbs-video-device-capture MODULE)
add_library(OBS::knurbs-video-device-capture ALIAS knurbs-video-device-capture)

target_sources(
  knurbs-video-device-capture
  PRIVATE backends.c backends/dshow/dshow-formats.h migrate.c migrate.h plugin-main.c video-device.c video-device.h
)

target_include_directories(knurbs-video-device-capture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(knurbs-video-device-capture PRIVATE OBS::libobs)

if(OS_WINDOWS)
  # DirectShow, with FFmpeg decoding MJPEG/H.264/HEVC and encoded audio.
  find_package(FFmpeg 8.0 REQUIRED COMPONENTS avcodec avutil)

  if(NOT TARGET OBS::libdshowcapture)
    add_subdirectory("${CMAKE_SOURCE_DIR}/deps/libdshowcapture" "${CMAKE_BINARY_DIR}/deps/libdshowcapture")
  endif()

  if(NOT TARGET OBS::winhandle)
    add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" "${CMAKE_BINARY_DIR}/libobs")
  endif()

  target_sources(
    knurbs-video-device-capture
    PRIVATE
      backends/dshow/dshow-backend.cpp
      backends/dshow/dshow-formats.h
      backends/dshow/encode-dstr.hpp
      backends/dshow/ffmpeg-decode.c
      backends/dshow/ffmpeg-decode.h
  )
  target_link_libraries(
    knurbs-video-device-capture
    PRIVATE OBS::libdshowcapture OBS::winhandle FFmpeg::avcodec FFmpeg::avutil strmiids winmm
  )
  target_compile_definitions(knurbs-video-device-capture PRIVATE HAVE_DSHOW)
endif()

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
      knurbs-video-device-capture
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
      knurbs-video-device-capture
      PRIVATE Libv4l2::Libv4l2 FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
    )
    target_compile_definitions(knurbs-video-device-capture PRIVATE HAVE_V4L2)

    if(ENABLE_UDEV)
      find_package(Libudev REQUIRED)
      target_sources(knurbs-video-device-capture PRIVATE backends/v4l2/v4l2-udev.c backends/v4l2/v4l2-udev.h)
      target_link_libraries(knurbs-video-device-capture PRIVATE Libudev::Libudev)
      target_compile_definitions(knurbs-video-device-capture PRIVATE HAVE_UDEV)
    endif()
  endif()

  # PipeWire cameras through the XDG camera portal. The portal's camera
  # nodes need PipeWire 0.3.60.
  option(ENABLE_PIPEWIRE "Enable PipeWire support" ON)
  if(ENABLE_PIPEWIRE)
    find_package(PipeWire 0.3.33 REQUIRED)
    if(PipeWire_VERSION VERSION_GREATER_EQUAL 0.3.60)
      if(NOT TARGET OBS::pipewire-common)
        add_subdirectory("${CMAKE_SOURCE_DIR}/shared/pipewire" "${CMAKE_BINARY_DIR}/shared/pipewire")
      endif()
      target_sources(knurbs-video-device-capture PRIVATE backends/pipewire/pipewire-camera.c)
      target_link_libraries(knurbs-video-device-capture PRIVATE OBS::glad OBS::pipewire-common)
      target_compile_definitions(knurbs-video-device-capture PRIVATE HAVE_PIPEWIRE_CAMERA)
      target_enable_feature(knurbs-video-device-capture "PipeWire camera backend")
    else()
      target_disable_feature(knurbs-video-device-capture "PipeWire camera backend (needs PipeWire 0.3.60)")
    endif()
  endif()
endif()

set_target_properties_obs(knurbs-video-device-capture PROPERTIES FOLDER plugins PREFIX "")
