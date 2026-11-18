cmake_minimum_required(VERSION 3.28...3.30)

add_executable(test-obs-mem test-obs-mem.c)
target_link_libraries(test-obs-mem PRIVATE OBS::libobs)
add_test(NAME test-obs-mem COMMAND test-obs-mem)
