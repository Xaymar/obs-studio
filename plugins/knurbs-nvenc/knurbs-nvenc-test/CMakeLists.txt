cmake_minimum_required(VERSION 3.28...3.30)

find_package(FFnvcodec 12 REQUIRED)

add_executable(knurbs-nvenc-test)

target_sources(knurbs-nvenc-test PRIVATE knurbs-nvenc-test.cpp)
target_link_libraries(knurbs-nvenc-test FFnvcodec::FFnvcodec)

set_target_properties_obs(knurbs-nvenc-test PROPERTIES FOLDER plugins/knurbs-nvenc)
