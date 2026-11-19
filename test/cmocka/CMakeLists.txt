project(obs-cmocka)

find_package(CMocka CONFIG REQUIRED)

# Serializer test
add_executable(test_serializer test_serializer.c)
target_include_directories(test_serializer PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_serializer PRIVATE OBS::libobs cmocka::cmocka)
add_test(test_serializer test_serializer)

# darray test
add_executable(test_darray test_darray.c)
target_include_directories(test_darray PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_darray PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_darray test_darray)

# bitstream test
add_executable(test_bitstream test_bitstream.c)
target_include_directories(test_bitstream PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_bitstream PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_bitstream test_bitstream)

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

add_test(test_os_path test_os_path)
