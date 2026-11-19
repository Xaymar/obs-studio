add_executable(spike4-amf-vulkan main.cpp)
target_include_directories(spike4-amf-vulkan PRIVATE "${SPIKES_AMF_HEADERS_DIR}")
target_link_libraries(spike4-amf-vulkan PRIVATE spike_common)
