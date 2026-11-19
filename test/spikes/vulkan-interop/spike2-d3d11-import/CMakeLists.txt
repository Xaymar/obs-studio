add_executable(spike2-d3d11-import main.cpp)
target_link_libraries(spike2-d3d11-import PRIVATE spike_common d3d11 dxgi)
