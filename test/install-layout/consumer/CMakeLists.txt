cmake_minimum_required(VERSION 3.28...3.30)

project(install-layout-consumer LANGUAGES C)

find_package(libobs REQUIRED)

get_target_property(include_dirs OBS::libobs INTERFACE_INCLUDE_DIRECTORIES)
if(NOT EXPECTED_INCLUDE_DIR IN_LIST include_dirs)
  message(FATAL_ERROR "OBS::libobs include directories are '${include_dirs}', expected '${EXPECTED_INCLUDE_DIR}'")
endif()

add_library(consumer MODULE consumer.c)
target_link_libraries(consumer PRIVATE OBS::libobs)

if(CHECK_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  add_library(consumer-frontend MODULE consumer-frontend.c)
  target_link_libraries(consumer-frontend PRIVATE OBS::libobs OBS::obs-frontend-api)
endif()
