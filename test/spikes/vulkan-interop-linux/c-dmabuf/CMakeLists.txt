add_executable(spike-c-dmabuf main.cpp)
target_link_libraries(spike-c-dmabuf PRIVATE spike_common PkgConfig::GBM PkgConfig::LIBDRM)
