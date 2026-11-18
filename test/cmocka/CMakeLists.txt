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

  # Pure logic: compiles the tracker directly, needs no device or loader.
  add_executable(test_vulkan_tracker test_vulkan_tracker.cpp ${CMAKE_SOURCE_DIR}/libobs-vulkan/vulkan-tracker.cpp)
  target_include_directories(test_vulkan_tracker PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_features(test_vulkan_tracker PRIVATE cxx_std_17)
  target_link_libraries(test_vulkan_tracker PRIVATE Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_tracker test_vulkan_tracker)

  # Every test that creates a device runs under the validation layer with synchronization validation, so the hazard
  # checks behind the tracker run in every ctest invocation. CI also sets OBS_VULKAN_REQUIRE_VALIDATION=1 so a
  # missing layer fails these tests instead of skipping the message check.
  set_tests_properties(
    test_vulkan_device
    test_vulkan_readback
    test_vulkan_upload
    test_vulkan_swapchain
    test_vulkan_shader
    test_vulkan_draw
    PROPERTIES ENVIRONMENT "OBS_VULKAN_VALIDATION=1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
  )
endif()

# OS path test
add_executable(test_os_path test_os_path.c)
target_include_directories(test_os_path PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_os_path PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_os_path test_os_path)
