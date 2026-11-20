cmake_minimum_required(VERSION 3.28...3.30)

# knurbs-device-output: one dock for DeckLink, AJA and ASIO outputs. Replaces
# decklink-output-ui, aja-output-ui and asio-output-ui for the Knurbs
# frontend. Links no device SDK: it drives the outputs the device plugins
# register, through their properties.

find_package(Qt6 REQUIRED Core Widgets)

if(NOT TARGET OBS::properties-view)
  add_subdirectory("${CMAKE_SOURCE_DIR}/shared/properties-view" "${CMAKE_BINARY_DIR}/shared/properties-view")
endif()

# Logic without Qt, shared with the tests.
add_library(knurbs-device-output-core STATIC)
target_sources(
  knurbs-device-output-core
  PRIVATE src/frame-copy.hpp src/output-types.cpp src/output-types.hpp src/settings.cpp src/settings.hpp
)
target_include_directories(knurbs-device-output-core PUBLIC src)
target_link_libraries(knurbs-device-output-core PUBLIC OBS::libobs)
set_target_properties(
  knurbs-device-output-core
  PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON FOLDER plugins/knurbs
)

add_library(knurbs-device-output MODULE)
add_library(Knurbs::device-output ALIAS knurbs-device-output)

target_sources(
  knurbs-device-output
  PRIVATE
    src/device-output.cpp
    src/device-output.hpp
    src/output-dock.cpp
    src/output-dock.hpp
    src/plugin.cpp
)

target_link_libraries(
  knurbs-device-output
  PRIVATE knurbs-device-output-core OBS::libobs OBS::frontend-api OBS::properties-view Qt6::Core Qt6::Widgets
)

set_target_properties(knurbs-device-output PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

if(COMMAND set_target_properties_obs)
  set_target_properties_obs(knurbs-device-output PROPERTIES FOLDER plugins/knurbs PREFIX "" AUTOMOC ON)
else()
  set_target_properties(knurbs-device-output PROPERTIES PREFIX "" AUTOMOC ON)
endif()

if(ENABLE_UNIT_TESTS)
  add_subdirectory(tests)
endif()
