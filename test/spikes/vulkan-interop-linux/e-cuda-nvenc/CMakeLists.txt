add_executable(spike-e-cuda-nvenc main.cpp)
# ffnvcodec headers only: libcuda and libnvidia-encode are dlopen'ed (no CUDA toolkit needed)
target_link_libraries(spike-e-cuda-nvenc PRIVATE spike_common PkgConfig::FFNVCODEC)
