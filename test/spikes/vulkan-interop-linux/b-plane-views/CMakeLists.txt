add_executable(spike-b-plane-views main.cpp)
target_link_libraries(spike-b-plane-views PRIVATE spike_common)
spike_shader(spike-b-plane-views ${SPIKE_COMMON}/fullscreen.vert fullscreen_vert)
spike_shader(spike-b-plane-views ${SPIKE_COMMON}/pattern.frag pattern_frag)
