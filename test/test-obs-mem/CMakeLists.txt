cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_TEST_OBS_MEM "Build the obs_mem refcounting test (not installed by default builds)" OFF)

if(NOT ENABLE_TEST_OBS_MEM)
  target_disable(test-obs-mem)
  return()
endif()

add_executable(test-obs-mem test-obs-mem.c)
target_link_libraries(test-obs-mem PRIVATE OBS::libobs)
set_target_properties_obs(test-obs-mem PROPERTIES FOLDER "Tests and Examples")
if(OS_WINDOWS)
  # Run the copy staged next to obs.dll and w32-pthreads.dll
  add_test(NAME test-obs-mem COMMAND "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}/test-obs-mem.exe")

  # obs.dll needs the obs-deps runtime libraries (ffmpeg, zlib)
  set(deps_path "")
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/bin")
      list(APPEND deps_path "PATH=path_list_prepend:${prefix}/bin")
    endif()
  endforeach()
  set_tests_properties(test-obs-mem PROPERTIES ENVIRONMENT_MODIFICATION "${deps_path}")
else()
  add_test(NAME test-obs-mem COMMAND test-obs-mem)
endif()

# report failures on stderr instead of opening a dialog, and never hang the run
target_sources(test-obs-mem PRIVATE "${CMAKE_SOURCE_DIR}/test/common/noninteractive.c")
set_tests_properties(test-obs-mem PROPERTIES TIMEOUT 300)
