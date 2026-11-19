find_package(CMocka CONFIG REQUIRED)

add_executable(test-knurbs-device-output test-device-output.cpp)
target_link_libraries(test-knurbs-device-output PRIVATE knurbs-device-output-core cmocka::cmocka)
set_target_properties(test-knurbs-device-output PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER tests)
add_test(NAME test-knurbs-device-output COMMAND test-knurbs-device-output)
