cmake_minimum_required(VERSION 3.28...3.30)

add_executable(knurbs-amf-test)
add_executable(OBS::knurbs-amf-test ALIAS knurbs-amf-test)

target_sources(knurbs-amf-test PRIVATE knurbs-amf-test.cpp)
target_link_libraries(
  knurbs-amf-test
  PRIVATE OBS::COMutils AMF::AMF d3d11 dxgi dxguid
)

set_target_properties_obs(knurbs-amf-test PROPERTIES FOLDER plugins/knurbs-amf)
