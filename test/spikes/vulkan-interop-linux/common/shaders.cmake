# spike_shader(<target> <file> <var>): compiles a GLSL file to a SPIR-V C array header <var>.h
find_program(GLSLANG glslangValidator REQUIRED)
function(spike_shader target file var)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/${var}.h")
  add_custom_command(
    OUTPUT "${out}"
    COMMAND "${GLSLANG}" -V --target-env vulkan1.3 --vn ${var} -o "${out}" "${file}"
    DEPENDS "${file}"
    VERBATIM
  )
  target_sources(${target} PRIVATE "${out}")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()
