project(obs-cmocka)

find_package(CMocka CONFIG REQUIRED)

# obs.dll and the obs-deps runtime libraries are not next to the test executables on Windows
set(_obs_runtime_env "")
if(OS_WINDOWS)
  list(APPEND _obs_runtime_env "PATH=path_list_prepend:${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}")
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/bin")
      list(APPEND _obs_runtime_env "PATH=path_list_prepend:${prefix}/bin")
    endif()
  endforeach()
endif()

# Every unit test and what it needs, so run-coverage.ps1 can build them in one go
add_custom_target(obs-unit-tests)

# obs_add_unit_test(<name>): <name>.c linked with libobs and cmocka, run in its own scratch directory
function(obs_add_unit_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(${name} PRIVATE OBS::libobs cmocka::cmocka $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
  set_target_properties(${name} PROPERTIES FOLDER "Tests and Examples")
  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/work/${name}")
  file(MAKE_DIRECTORY "${work_dir}")
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${work_dir}")
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
  add_dependencies(obs-unit-tests ${name})
endfunction()

# obs_add_graphics_test(<name>): a unit test that runs gs_* on the libobs-null module (see gs-test.h)
function(obs_add_graphics_test name)
  obs_add_unit_test(${name})
  target_compile_definitions(
    ${name}
    PRIVATE
      NULL_GRAPHICS_MODULE="$<TARGET_FILE:libobs-null>"
      OBS_DATA_DIR="${CMAKE_SOURCE_DIR}/libobs/data"
      TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  )
  add_dependencies(${name} libobs-null)
endfunction()

# obs_add_core_test(<name>): a graphics test that starts libobs (see obs-core-test.h). libobs finds its effects
# in ../../data/libobs relative to the working directory on Windows and in share/obs/libobs on Linux, so the test
# runs in work/<name>/bin/64bit next to a copy of the data.
function(obs_add_core_test name)
  obs_add_graphics_test(${name})
  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/work/${name}/bin/64bit")
  file(MAKE_DIRECTORY "${work_dir}")
  set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${work_dir}")
  add_custom_command(
    TARGET ${name}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/libobs/data"
      "${CMAKE_CURRENT_BINARY_DIR}/work/${name}/data/libobs"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/libobs/data" "${work_dir}/share/obs/libobs"
    VERBATIM
  )
endfunction()

# Serializer test
add_executable(test_serializer test_serializer.c)
target_include_directories(test_serializer PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_serializer PRIVATE OBS::libobs cmocka::cmocka)
add_test(NAME test_serializer COMMAND test_serializer)

# darray test
add_executable(test_darray test_darray.c)
target_include_directories(test_darray PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_darray PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_darray COMMAND test_darray)

# bitstream test
add_executable(test_bitstream test_bitstream.c)
target_include_directories(test_bitstream PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_bitstream PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_bitstream COMMAND test_bitstream)

# Source ID aliases and settings migration
add_executable(test_source_alias test_source_alias.c)
target_include_directories(test_source_alias PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_source_alias PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_source_alias COMMAND test_source_alias)
set_tests_properties(test_source_alias PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")

# Video Device Capture: migrations and backend selection, with fake backends
add_executable(
  test_video_device
  test_video_device.c
  ${CMAKE_SOURCE_DIR}/plugins/knurbs-video-device-capture/migrate.c
  ${CMAKE_SOURCE_DIR}/plugins/knurbs-video-device-capture/video-device.c
)
target_include_directories(
  test_video_device
  PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/plugins/knurbs-video-device-capture
)
target_link_libraries(test_video_device PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_video_device COMMAND test_video_device)
set_tests_properties(test_video_device PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")

# PipeWire async frame plane layout
if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  find_package(PipeWire 0.3.33 QUIET)
  find_package(Libdrm QUIET)
  if(PipeWire_FOUND AND Libdrm_FOUND)
    get_target_property(_libdrm_includes Libdrm::Libdrm INTERFACE_INCLUDE_DIRECTORIES)
    add_executable(test_pipewire_formats test_pipewire_formats.c ${CMAKE_SOURCE_DIR}/shared/pipewire/formats.c)
    target_include_directories(
      test_pipewire_formats
      PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/shared/pipewire ${_libdrm_includes}
    )
    target_link_libraries(test_pipewire_formats PRIVATE OBS::libobs PipeWire::PipeWire cmocka::cmocka)
    add_test(NAME test_pipewire_formats COMMAND test_pipewire_formats)
    set_tests_properties(test_pipewire_formats PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
  endif()
endif()

add_executable(test_no_video test_no_video.c)
target_include_directories(test_no_video PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_no_video PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_no_video test_no_video)

add_executable(test_type_alias test_type_alias.c)
target_include_directories(test_type_alias PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_type_alias PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_type_alias test_type_alias)

# Settings migration of the NVENC plugin; plain libobs code, so it builds
# without the NVENC SDK.
add_executable(test_nvenc_migrate test_nvenc_migrate.c "${CMAKE_SOURCE_DIR}/plugins/knurbs-nvenc/nvenc-migrate.c")
target_include_directories(test_nvenc_migrate PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/knurbs-nvenc")
target_link_libraries(test_nvenc_migrate PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_nvenc_migrate test_nvenc_migrate)

# Vulkan backend export test
if(ENABLE_VULKAN)
  find_package(Vulkan REQUIRED)
  add_executable(test_vulkan_exports test_vulkan_exports.cpp)
  target_include_directories(test_vulkan_exports PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(
    test_vulkan_exports
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_exports libobs-vulkan)
  target_link_libraries(test_vulkan_exports PRIVATE OBS::libobs cmocka::cmocka)
  add_test(test_vulkan_exports test_vulkan_exports)

  # Needs a Vulkan device; each test skips when device_create reports GS_ERROR_NOT_SUPPORTED.
  add_executable(test_vulkan_device test_vulkan_device.cpp)
  target_include_directories(test_vulkan_device PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_device
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_device libobs-vulkan)
  target_link_libraries(test_vulkan_device PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_device test_vulkan_device)

  # Queue global priority: default, opt-in realtime, refusal fallback; skips without a device.
  add_executable(test_vulkan_priority test_vulkan_priority.cpp)
  target_include_directories(test_vulkan_priority PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_priority
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_priority libobs-vulkan)
  target_link_libraries(test_vulkan_priority PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_priority test_vulkan_priority)

  # Textures, clear and stage-surface readback; skips without a device.
  add_executable(test_vulkan_readback test_vulkan_readback.cpp)
  target_include_directories(test_vulkan_readback PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_readback
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_readback libobs-vulkan)
  target_link_libraries(test_vulkan_readback PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_readback test_vulkan_readback)

  # Texture upload, mipmaps, dynamic map and sRGB views; skips without a device.
  add_executable(test_vulkan_upload test_vulkan_upload.cpp)
  target_include_directories(test_vulkan_upload PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_upload
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_upload libobs-vulkan)
  target_link_libraries(test_vulkan_upload PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_upload test_vulkan_upload)

  # Swapchain and present on hidden Win32 windows; skips without a device or windowing.
  add_executable(test_vulkan_swapchain test_vulkan_swapchain.cpp)
  target_include_directories(test_vulkan_swapchain PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_swapchain
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_swapchain libobs-vulkan)
  target_link_libraries(test_vulkan_swapchain PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_swapchain test_vulkan_swapchain)

  # Shader translation and params; the effect parser is compiled in (ep_parse is not exported). No device needed
  # except for the glslang refcount test, which skips without one.
  add_executable(
    test_vulkan_shader
    test_vulkan_shader.cpp
    test_vulkan_shader_effect.c
    test_vulkan_shader_effect.h
    ${CMAKE_SOURCE_DIR}/libobs/graphics/effect-parser.c
  )
  target_include_directories(test_vulkan_shader PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_shader
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_shader PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_shader libobs-vulkan)
  target_link_libraries(test_vulkan_shader PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_shader test_vulkan_shader)

  # Draw path; needs a device (skips without one). The effect parser is compiled in, as for test_vulkan_shader.
  add_executable(
    test_vulkan_draw
    test_vulkan_draw.cpp
    test_vulkan_shader_effect.c
    test_vulkan_shader_effect.h
    ${CMAKE_SOURCE_DIR}/libobs/graphics/effect-parser.c
  )
  target_include_directories(test_vulkan_draw PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_draw
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_draw PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_draw libobs-vulkan)
  target_link_libraries(test_vulkan_draw PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_draw test_vulkan_draw)

  # Depth/stencil buffers and depth-tested drawing; needs a device (skips without one).
  add_executable(test_vulkan_depth test_vulkan_depth.cpp)
  target_include_directories(test_vulkan_depth PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_depth
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_depth PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_depth libobs-vulkan)
  target_link_libraries(test_vulkan_depth PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_depth test_vulkan_depth)

  # Every .effect in the tree and texrender through the generic gs_* API (gs_create); skips without a device.
  # The test walks the same directories at run time and must find exactly this many.
  file(
    GLOB_RECURSE _effects
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/libobs/data/*.effect"
    "${CMAKE_SOURCE_DIR}/frontend/data/*.effect"
    "${CMAKE_SOURCE_DIR}/test/test-input/data/*.effect"
  )
  file(GLOB _plugin_dirs LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/plugins/*")
  foreach(_dir IN LISTS _plugin_dirs)
    file(GLOB_RECURSE _plugin_effects CONFIGURE_DEPENDS "${_dir}/data/*.effect")
    list(APPEND _effects ${_plugin_effects})
  endforeach()
  list(LENGTH _effects _effect_count)
  add_executable(test_vulkan_effects test_vulkan_effects.cpp)
  target_include_directories(test_vulkan_effects PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(
    test_vulkan_effects
    PRIVATE
      OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
      OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
      OBS_EFFECT_COUNT=${_effect_count}
  )
  target_compile_features(test_vulkan_effects PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_effects libobs-vulkan)
  target_link_libraries(test_vulkan_effects PRIVATE OBS::libobs cmocka::cmocka)
  add_test(test_vulkan_effects test_vulkan_effects)

  # End-to-end OBS video pipeline on a module given as argument; rendered for Vulkan and (Windows) D3D11 in separate
  # processes, then compared. Only a Vulkan render without a device may skip (reported Skipped); it writes nothing,
  # and compare then fails on the missing file (or is not run), so the comparison never passes without comparing.
  add_executable(test_vulkan_video test_vulkan_video.cpp)
  target_include_directories(test_vulkan_video PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(test_vulkan_video PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_compile_features(test_vulkan_video PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_video libobs-vulkan)
  target_link_libraries(test_vulkan_video PRIVATE OBS::libobs cmocka::cmocka $<$<PLATFORM_ID:Windows>:gdi32>)
  set(_video_vulkan "${CMAKE_CURRENT_BINARY_DIR}/video_vulkan.bin")
  set(_video_d3d11 "${CMAKE_CURRENT_BINARY_DIR}/video_d3d11.bin")
  add_test(
    NAME test_vulkan_video_vulkan
    COMMAND test_vulkan_video render "$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan" vulkan "${_video_vulkan}"
  )
  # cmocka's skip() exits 0: SKIPPED in the output makes ctest report Skipped, never Passed.
  set_tests_properties(
    test_vulkan_video_vulkan
    PROPERTIES FIXTURES_SETUP video_vulkan SKIP_REGULAR_EXPRESSION "SKIPPED"
  )
  if(TARGET libobs-d3d11)
    add_dependencies(test_vulkan_video libobs-d3d11)
    add_test(
      NAME test_vulkan_video_d3d11
      COMMAND test_vulkan_video render "$<TARGET_FILE_DIR:libobs-d3d11>/libobs-d3d11" d3d11 "${_video_d3d11}"
    )
    set_tests_properties(
      test_vulkan_video_d3d11
      PROPERTIES FIXTURES_SETUP video_d3d11 SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
    add_test(NAME test_vulkan_video_compare COMMAND test_vulkan_video compare "${_video_vulkan}" "${_video_d3d11}")
    set_tests_properties(
      test_vulkan_video_compare
      PROPERTIES FIXTURES_REQUIRED "video_vulkan;video_d3d11" SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
  endif()

  # Queue priority measurement only (not a test, not installed): the synthetic D3D11 foreground load. The OBS side
  # load is `test_vulkan_video load`. See docs/superpowers/specs/2026-11-17-libobs-vulkan-priority-measurement.md.
  if(WIN32)
    add_executable(vulkan_priority_fg_load vulkan_priority_fg_load.cpp)
    target_link_libraries(vulkan_priority_fg_load PRIVATE d3d11 dxgi d3dcompiler)
  endif()

  # Pure logic: compiles the tracker directly, needs no device or loader.
  add_executable(test_vulkan_tracker test_vulkan_tracker.cpp ${CMAKE_SOURCE_DIR}/libobs-vulkan/vulkan-tracker.cpp)
  target_include_directories(test_vulkan_tracker PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_features(test_vulkan_tracker PRIVATE cxx_std_17)
  target_link_libraries(test_vulkan_tracker PRIVATE Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_tracker test_vulkan_tracker)

  # Every test that creates a device runs under the validation layer with synchronization validation, so the hazard
  # checks behind the tracker run in every ctest invocation. Set OBS_VULKAN_REQUIRE_VALIDATION=1 to make a missing
  # validation layer fail the test. cmocka's skip() exits 0, so SKIPPED (no Vulkan 1.3 device) reports Skipped.
  set_tests_properties(
    test_vulkan_device
    test_vulkan_priority
    test_vulkan_readback
    test_vulkan_upload
    test_vulkan_swapchain
    test_vulkan_shader
    test_vulkan_draw
    test_vulkan_depth
    test_vulkan_effects
    test_vulkan_video_vulkan
    PROPERTIES
      ENVIRONMENT "OBS_VULKAN_VALIDATION=1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
      SKIP_REGULAR_EXPRESSION "SKIPPED"
  )
endif()

# OS path test
add_executable(test_os_path test_os_path.c)
target_include_directories(test_os_path PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_os_path PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_os_path COMMAND test_os_path)

set_tests_properties(
  test_serializer
  test_darray
  test_bitstream
  test_os_path
  PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}"
)

# Tests that start libobs: run from the rundir so libobs finds its data files (../../data/libobs)
if(TARGET libobs-null)
  # Canvas weak reference regression test
  add_executable(test_obs_canvas_weak test_obs_canvas_weak.c)
  target_include_directories(test_obs_canvas_weak PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_obs_canvas_weak PRIVATE OBS::libobs cmocka::cmocka)
  add_test(
    NAME test_obs_canvas_weak
    COMMAND test_obs_canvas_weak
    WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  )

  # Video thread on the null graphics module
  add_executable(test_obs_video_null test_obs_video_null.c)
  target_include_directories(test_obs_video_null PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_obs_video_null PRIVATE OBS::libobs cmocka::cmocka)
  target_compile_definitions(test_obs_video_null PRIVATE NULL_GRAPHICS_MODULE="$<TARGET_FILE:libobs-null>")
  add_dependencies(test_obs_video_null libobs-null)
  add_test(
    NAME test_obs_video_null
    COMMAND test_obs_video_null
    WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  )

  set_tests_properties(test_obs_canvas_weak test_obs_video_null PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
endif()

foreach(test IN ITEMS test_serializer test_darray test_bitstream test_os_path test_obs_canvas_weak test_obs_video_null)
  if(TARGET ${test})
    add_dependencies(obs-unit-tests ${test})
  endif()
endforeach()
if(TARGET test-obs-mem)
  add_dependencies(obs-unit-tests test-obs-mem)
endif()

# util/
foreach(
  test
  IN
  ITEMS
    test_util_base
    test_util_bmem
    test_util_crc32
    test_util_darray
    test_util_deque
    test_util_dstr
    test_util_lexer
    test_util_cf_lexer
    test_util_cf_parser
    test_util_config_file
    test_util_text_lookup
    test_util_array_serializer
    test_util_file_serializer
    test_util_buffered_file_serializer
    test_util_platform
    test_util_pipe
    test_util_profiler
    test_util_threading
    test_util_task
    test_util_spsc_fifo
)
  obs_add_unit_test(${test})
endforeach()
if(OS_WINDOWS)
  obs_add_unit_test(test_util_windows)
endif()

# media-io/
foreach(
  test
  IN
  ITEMS
    test_media_format_conversion
    test_media_video_frame
    test_media_video_formats
    test_media_audio_resampler
    test_media_video_scaler
    test_media_video_io
    test_media_audio_io
    test_media_remux
)
  obs_add_unit_test(${test})
endforeach()

# graphics/ math
foreach(
  test
  IN
  ITEMS
    test_graphics_vec
    test_graphics_matrix
    test_graphics_geometry
    test_graphics_shader_parser
)
  obs_add_unit_test(${test})
endforeach()

# graphics/
foreach(
  test
  IN
  ITEMS
    test_graphics_graphics
    test_graphics_failures
    test_graphics_effect
    test_graphics_image_file
    test_graphics_texture_render
)
  obs_add_graphics_test(${test})
endforeach()

# core/
foreach(
  test
  IN
  ITEMS
    test_obs_data
    test_obs_properties
    test_callback
    test_obs_codec_helpers
)
  obs_add_graphics_test(${test})
endforeach()

# core/ libobs
foreach(
  test
  IN
  ITEMS
    test_obs_source
    test_obs_source_av
    test_obs_source_profiler
    test_obs_register
    test_obs_video_formats
    test_obs_video_failures
    test_obs_scene
    test_obs_output
    test_obs_encoder_slow
    test_obs_audio
    test_obs_hotkey
    test_obs_canvas
)
  obs_add_core_test(${test})
endforeach()

# fake module for test_obs_module
add_library(fake-module MODULE fake-module/fake-module.c)
target_link_libraries(fake-module PRIVATE OBS::libobs)
set_target_properties(fake-module PROPERTIES FOLDER "Tests and Examples" PREFIX "")
obs_add_core_test(test_obs_module)
target_compile_definitions(
  test_obs_module
  PRIVATE FAKE_MODULE_PATH="$<TARGET_FILE:fake-module>" FAKE_MODULE_DATA="${CMAKE_CURRENT_SOURCE_DIR}/fake-module/data"
)
add_dependencies(test_obs_module fake-module)

# Every test program reports failures on stderr instead of opening a dialog (see noninteractive.c), and no
# test may hang the run.
get_property(_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _test_targets)
  get_target_property(_type ${_target} TYPE)
  if(_type STREQUAL "EXECUTABLE")
    target_sources(${_target} PRIVATE "${CMAKE_SOURCE_DIR}/test/common/noninteractive.c")
  endif()
endforeach()
get_property(_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_tests} PROPERTIES TIMEOUT 300)
