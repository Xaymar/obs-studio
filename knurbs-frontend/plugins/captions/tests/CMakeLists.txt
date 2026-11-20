find_package(CMocka CONFIG REQUIRED)

add_executable(test-knurbs-captions test-captions.cpp)
target_link_libraries(test-knurbs-captions PRIVATE knurbs-captions-core cmocka::cmocka)
set_target_properties(test-knurbs-captions PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER tests)
add_test(NAME test-knurbs-captions COMMAND test-knurbs-captions)
