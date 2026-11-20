# Spike A: builds the existing test/cmocka Vulkan tests on Linux against an already built tree (build-vk), with the
# cmocka shim instead of libcmocka (not in the image). Standalone so the repo's test CMake stays untouched.
#   cmake -S . -B build -G Ninja -DOBS_BUILD_DIR=<knurbs>/build-vk && ninja -C build && ./run-tests.sh build
cmake_minimum_required(VERSION 3.28)
project(spike-a-vulkan-tests C CXX)

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
cmake_path(NORMAL_PATH SRC)
if(NOT OBS_BUILD_DIR)
  message(FATAL_ERROR "Set OBS_BUILD_DIR to the configured and built knurbs tree (ENABLE_VULKAN=ON)")
endif()
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

add_library(cmocka_shim STATIC ../cmocka-shim/cmocka-shim.c)
target_include_directories(cmocka_shim PUBLIC ../cmocka-shim)

add_library(obs_imported INTERFACE)
target_include_directories(
  obs_imported
  INTERFACE "${OBS_BUILD_DIR}/config" "${OBS_BUILD_DIR}/libobs" "${SRC}/libobs" "${SRC}/libobs-vulkan" "${SRC}/deps/vma"
)
target_link_libraries(obs_imported INTERFACE "${OBS_BUILD_DIR}/libobs/libobs.so" Vulkan::Vulkan cmocka_shim Threads::Threads)

set(MOD "${OBS_BUILD_DIR}/libobs-vulkan/liblibobs-vulkan")
set(T "${SRC}/test/cmocka")
file(
  GLOB_RECURSE _effects
  "${SRC}/libobs/data/*.effect"
  "${SRC}/frontend/data/*.effect"
  "${SRC}/test/test-input/data/*.effect"
)
file(GLOB _plugin_dirs LIST_DIRECTORIES true "${SRC}/plugins/*")
foreach(_dir IN LISTS _plugin_dirs)
  file(GLOB_RECURSE _plugin_effects "${_dir}/data/*.effect")
  list(APPEND _effects ${_plugin_effects})
endforeach()
list(LENGTH _effects _effect_count)

foreach(name exports device priority readback upload swapchain shader draw depth effects video)
  set(srcs "${T}/test_vulkan_${name}.cpp")
  if(name STREQUAL "shader" OR name STREQUAL "draw")
    list(APPEND srcs "${T}/test_vulkan_shader_effect.c" "${SRC}/libobs/graphics/effect-parser.c")
  endif()
  add_executable(test_vulkan_${name} ${srcs})
  target_compile_features(test_vulkan_${name} PRIVATE cxx_std_17)
  target_compile_definitions(
    test_vulkan_${name}
    PRIVATE OBS_SOURCE_DIR="${SRC}" OBS_VULKAN_MODULE="${MOD}" OBS_EFFECT_COUNT=${_effect_count}
  )
  target_link_libraries(test_vulkan_${name} PRIVATE obs_imported)
endforeach()

add_executable(test_vulkan_tracker "${T}/test_vulkan_tracker.cpp" "${SRC}/libobs-vulkan/vulkan-tracker.cpp")
target_compile_features(test_vulkan_tracker PRIVATE cxx_std_17)
target_link_libraries(test_vulkan_tracker PRIVATE obs_imported)
