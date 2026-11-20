cmake_minimum_required(VERSION 3.28...3.30)

add_executable(knurbs-qsv-test)

find_package(VPL 2.9 REQUIRED)

target_sources(knurbs-qsv-test PRIVATE knurbs-qsv-test.cpp)

target_compile_definitions(knurbs-qsv-test PRIVATE "$<$<BOOL:${ENABLE_HEVC}>:ENABLE_HEVC>")

target_link_libraries(
  knurbs-qsv-test
  d3d11
  dxgi
  dxguid
  VPL::VPL
  OBS::COMutils
)

target_link_options(knurbs-qsv-test PRIVATE /IGNORE:4099)

set_target_properties_obs(knurbs-qsv-test PROPERTIES FOLDER plugins/knurbs-qsv PREFIX "")
