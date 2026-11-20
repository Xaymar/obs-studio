cmake_minimum_required(VERSION 3.28...3.30)

# Knurbs frontend: QApplication-first, Core/Shell split, Vulkan-only libobs.
# Lives beside frontend/ (which stays untouched) until it reaches parity.

option(ENABLE_KNURBS_FRONTEND "Build the new Knurbs frontend" OFF)
if(NOT ENABLE_KNURBS_FRONTEND)
  return()
endif()

if(NOT ENABLE_VULKAN)
  message(FATAL_ERROR "The Knurbs frontend renders with libobs-vulkan only; configure with -DENABLE_VULKAN=ON")
endif()

# Always track the latest Qt 6 release. QRhiWidget (preview) needs 6.7 or newer.
set(KNURBS_QT_MINIMUM_VERSION 6.8 CACHE STRING "Minimum Qt 6 version for the Knurbs frontend")
mark_as_advanced(KNURBS_QT_MINIMUM_VERSION)

find_package(Qt6 ${KNURBS_QT_MINIMUM_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network)

include(cmake/qtads.cmake)

if(NOT TARGET OBS::libobs)
  add_subdirectory("${CMAKE_SOURCE_DIR}/libobs" libobs)
endif()
if(NOT TARGET OBS::frontend-api)
  add_subdirectory("${CMAKE_SOURCE_DIR}/frontend/api" obs-frontend-api)
endif()

add_executable(knurbs)

set_target_properties(knurbs PROPERTIES AUTOMOC ON CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# libobs loads the renderer at runtime, so nothing links it; build it with the frontend anyway.
add_dependencies(knurbs libobs-vulkan)

target_sources(
  knurbs
  PRIVATE
    src/main.cpp
    src/app/Application.cpp
    src/app/Application.hpp
    src/app/Bootstrap.cpp
    src/app/Bootstrap.hpp
    src/app/CommandLine.cpp
    src/app/CommandLine.hpp
    src/app/Log.cpp
    src/app/Log.hpp
    src/app/Paths.cpp
    src/app/Paths.hpp
    src/app/SingleInstance.cpp
    src/app/SingleInstance.hpp
    src/app/SplashScreen.cpp
    src/app/SplashScreen.hpp
    src/core/Core.cpp
    src/core/Core.hpp
    src/core/ModuleLoader.cpp
    src/core/ModuleLoader.hpp
    src/shell/MainWindow.cpp
    src/shell/MainWindow.hpp
    src/api/FrontendApi.cpp
    src/api/FrontendApi.hpp
)

target_include_directories(knurbs PRIVATE src "${CMAKE_SOURCE_DIR}/frontend/api")

target_link_libraries(
  knurbs
  PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network OBS::libobs OBS::frontend-api Knurbs::qtads
)

if(OS_WINDOWS)
  set_target_properties(knurbs PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(COMMAND set_target_properties_obs)
  set_target_properties_obs(knurbs PROPERTIES FOLDER frontend)
endif()
