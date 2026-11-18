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
endif()

# OS path test
add_executable(test_os_path test_os_path.c)
target_include_directories(test_os_path PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_os_path PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_os_path test_os_path)
