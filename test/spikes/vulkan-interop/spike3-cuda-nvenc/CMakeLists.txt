add_executable(spike3-cuda-nvenc main.cpp)
target_include_directories(spike3-cuda-nvenc PRIVATE "${SPIKES_NVCODEC_HEADERS_DIR}/include")
target_link_libraries(spike3-cuda-nvenc PRIVATE spike_common)
