cmake_minimum_required(VERSION 3.28...3.30)

# Conversions between libobs and FFmpeg formats. Header only; consumers link
# libobs and the FFmpeg libraries themselves.
add_library(ffmpeg-formats INTERFACE)
add_library(OBS::ffmpeg-formats ALIAS ffmpeg-formats)

target_sources(ffmpeg-formats INTERFACE ffmpeg-formats.h)

target_include_directories(ffmpeg-formats INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
