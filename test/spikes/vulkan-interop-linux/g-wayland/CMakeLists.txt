# Headless Wayland: a minimal in-process compositor (wl_compositor + zwp_linux_dmabuf_v1 v4) that imports the
# client's dmabufs into its own VkDevice and checks the pixels; the client presents through VK_KHR_wayland_surface.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WP_DIR wayland-protocols pkgdatadir)
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY "${_gen}")
set(_protos "stable/linux-dmabuf/linux-dmabuf-v1" "staging/linux-drm-syncobj/linux-drm-syncobj-v1")
set(_proto_srcs)
foreach(_p IN LISTS _protos)
  get_filename_component(_n "${_p}" NAME)
  set(_xml "${WP_DIR}/${_p}.xml")
  add_custom_command(
    OUTPUT "${_gen}/${_n}-server.h" "${_gen}/${_n}.c"
    COMMAND "${WAYLAND_SCANNER}" server-header "${_xml}" "${_gen}/${_n}-server.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${_xml}" "${_gen}/${_n}.c"
    DEPENDS "${_xml}"
    VERBATIM
  )
  list(APPEND _proto_srcs "${_gen}/${_n}.c" "${_gen}/${_n}-server.h")
endforeach()
add_executable(spike-g-wayland main.cpp ${_proto_srcs})
target_include_directories(spike-g-wayland PRIVATE "${_gen}")
target_compile_definitions(spike-g-wayland PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
target_link_libraries(spike-g-wayland PRIVATE spike_common PkgConfig::WAYLAND PkgConfig::LIBDRM)
