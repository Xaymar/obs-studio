cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(obs-studio VERSION ${OBS_VERSION_CANONICAL})

if(CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/architecture.cmake")
  if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
    return()
  endif()
endif()

include(compilerconfig)
include(defaults)
include(helpers)

option(ENABLE_FRONTEND "Enable building with UI (requires Qt)" ON)
option(ENABLE_SCRIPTING "Enable scripting support" ON)
option(ENABLE_HEVC "Enable HEVC encoders" ON)
option(ENABLE_VULKAN "Enable the Vulkan graphics backend (requires the Vulkan SDK)" OFF)

enable_testing()

add_subdirectory(libobs)
if(OS_WINDOWS)
  add_subdirectory(libobs-d3d11)
  add_subdirectory(libobs-winrt)
endif()
add_subdirectory(libobs-opengl)
if(OS_MACOS)
  add_subdirectory(libobs-metal)
endif()
if(ENABLE_VULKAN)
  add_subdirectory(deps/vma)
  add_subdirectory(libobs-vulkan)
endif()
add_subdirectory(plugins)

add_subdirectory(test/test-input)
add_subdirectory(test/test-obs-mem)
add_subdirectory(test/install-layout)

if(ENABLE_UNIT_TESTS)
  enable_testing()
  add_subdirectory(test/null-graphics)
  add_subdirectory(test/cmocka)
endif()

add_subdirectory(frontend)

message_configuration()
