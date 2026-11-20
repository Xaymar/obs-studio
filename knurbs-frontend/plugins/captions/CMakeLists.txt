cmake_minimum_required(VERSION 3.28...3.30)

# knurbs-captions: passes captions from sources and caption plugins on to the
# stream and the recording. Replaces decklink-captions for the Knurbs
# frontend. include/knurbs-captions-api.h is the API for caption plugins.

find_package(Qt6 REQUIRED Core Widgets)

# The API header, for caption provider plugins. Header only.
add_library(knurbs-captions-api INTERFACE)
add_library(Knurbs::captions-api ALIAS knurbs-captions-api)
target_include_directories(knurbs-captions-api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(knurbs-captions-api INTERFACE OBS::libobs)

# Logic without Qt, shared with the tests.
add_library(knurbs-captions-core STATIC)
target_sources(
  knurbs-captions-core
  PRIVATE
    src/caption-router.cpp
    src/caption-router.hpp
    src/captions-config.cpp
    src/captions-config.hpp
    src/captions.cpp
    src/captions.hpp
)
target_include_directories(knurbs-captions-core PUBLIC src)
target_link_libraries(knurbs-captions-core PUBLIC OBS::libobs OBS::frontend-api knurbs-captions-api)
set_target_properties(
  knurbs-captions-core
  PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON FOLDER plugins/knurbs
)

add_library(knurbs-captions MODULE)
add_library(Knurbs::captions ALIAS knurbs-captions)

target_sources(knurbs-captions PRIVATE src/captions-dock.cpp src/captions-dock.hpp src/plugin.cpp)

target_link_libraries(
  knurbs-captions
  PRIVATE knurbs-captions-core knurbs-captions-api OBS::libobs OBS::frontend-api Qt6::Core Qt6::Widgets
)

set_target_properties(knurbs-captions PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

if(COMMAND set_target_properties_obs)
  set_target_properties_obs(knurbs-captions PROPERTIES FOLDER plugins/knurbs PREFIX "" AUTOMOC ON)
else()
  set_target_properties(knurbs-captions PROPERTIES PREFIX "" AUTOMOC ON)
endif()

if(ENABLE_UNIT_TESTS)
  add_subdirectory(tests)
endif()
