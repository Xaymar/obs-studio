cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_TEST_INSTALL_LAYOUT "Test the installed header layout and the CMake/pkg-config consumer setup" OFF)

if(NOT ENABLE_TEST_INSTALL_LAYOUT)
  return()
endif()

# Installs the Runtime and Development components into a scratch prefix, checks that the public headers end up under
# OBS_INCLUDE_DESTINATION (include/knurbs) and nowhere else, then builds a small consumer against the installed package
# with find_package() and, on Linux, with pkg-config.
add_test(
  NAME install-layout
  COMMAND
    "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/prefix"
    "-DCONFIG=$<CONFIG>" "-DINCLUDE_DESTINATION=${OBS_INCLUDE_DESTINATION}"
    "-DLIBRARY_DESTINATION=${OBS_LIBRARY_DESTINATION}" "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
    "-DCONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer" "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}" "-DC_COMPILER=${CMAKE_C_COMPILER}"
    "-DDEPS_PREFIX_PATH=${CMAKE_PREFIX_PATH}" "-DCHECK_FRONTEND_API=$<TARGET_EXISTS:obs-frontend-api>"
    "-DCHECK_W32_PTHREADS=$<BOOL:${OS_WINDOWS}>" "-DCHECK_PKG_CONFIG=$<BOOL:${OS_LINUX}>" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/check-install-layout.cmake"
)
