cmake_minimum_required(VERSION 3.28...3.30)

# shortcut: exports only the Windows set of graphics-imports.c; add the Linux/macOS exports to run the tests there
if(NOT OS_WINDOWS)
  return()
endif()

add_library(libobs-null MODULE)
add_library(OBS::libobs-null ALIAS libobs-null)

target_sources(libobs-null PRIVATE null-calls.h null-graphics.h null-subsystem.c)
target_include_directories(libobs-null PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(libobs-null PRIVATE OBS::libobs)

set_target_properties(libobs-null PROPERTIES FOLDER "Tests and Examples" PREFIX "")
