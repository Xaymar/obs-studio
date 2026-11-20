add_executable(spike-d-sync main.cpp)
target_link_libraries(spike-d-sync PRIVATE spike_common PkgConfig::LIBDRM)
