# Checks the installed header layout of libobs and the other exported libraries.
#
# Expects BUILD_DIR, PREFIX, CONFIG, INCLUDE_DESTINATION, LIBRARY_DESTINATION, CONSUMER_SOURCE_DIR, CONSUMER_BINARY_DIR,
# GENERATOR, GENERATOR_PLATFORM, C_COMPILER, DEPS_PREFIX_PATH, CHECK_FRONTEND_API, CHECK_W32_PTHREADS and
# CHECK_PKG_CONFIG.

cmake_minimum_required(VERSION 3.28)

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
  if(NOT result EQUAL 0)
    string(JOIN " " command ${ARGN})
    message(FATAL_ERROR "Command failed (${result}): ${command}\n${output}")
  endif()
endfunction()

if(NOT INCLUDE_DESTINATION MATCHES "(^|/)include/knurbs$")
  message(FATAL_ERROR "OBS_INCLUDE_DESTINATION is '${INCLUDE_DESTINATION}', expected it to end in include/knurbs")
endif()

file(REMOVE_RECURSE "${PREFIX}" "${CONSUMER_BINARY_DIR}")

foreach(component IN ITEMS Runtime Development)
  run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}" --config "${CONFIG}" --component ${component})
endforeach()

set(include_dir "${PREFIX}/${INCLUDE_DESTINATION}")

set(
  expected_headers
  obs-module.h
  obs.h
  obsconfig.h
  util/bmem.h
  util/threading.h
  graphics/graphics.h
)
if(CHECK_FRONTEND_API)
  list(APPEND expected_headers obs-frontend-api.h)
endif()
if(CHECK_W32_PTHREADS)
  list(APPEND expected_headers pthread.h sched.h)
endif()

foreach(header IN LISTS expected_headers)
  if(NOT EXISTS "${include_dir}/${header}")
    message(FATAL_ERROR "Installed header missing: ${include_dir}/${header}")
  endif()
endforeach()

# Nothing may be installed straight into <prefix>/include or the old include/obs directory, where it collides with other
# packages (and with an upstream OBS Studio install).
get_filename_component(include_root "${include_dir}" DIRECTORY)
file(GLOB stray LIST_DIRECTORIES true RELATIVE "${include_root}" "${include_root}/*")
list(REMOVE_ITEM stray knurbs)
if(stray)
  message(FATAL_ERROR "Unexpected entries installed next to include/knurbs in ${include_root}: ${stray}")
endif()

# CMake package consumer: the exported targets' INTERFACE_INCLUDE_DIRECTORIES must point at include/knurbs.
set(prefix_path "${PREFIX}")
list(APPEND prefix_path ${DEPS_PREFIX_PATH})
string(REPLACE ";" "\;" prefix_path "${prefix_path}")

set(
  configure_args
  -S
  "${CONSUMER_SOURCE_DIR}"
  -B
  "${CONSUMER_BINARY_DIR}"
  -G
  "${GENERATOR}"
  "-DCMAKE_PREFIX_PATH=${prefix_path}"
  "-DEXPECTED_INCLUDE_DIR=${include_dir}"
  "-DCHECK_FRONTEND_API=${CHECK_FRONTEND_API}"
)
if(GENERATOR_PLATFORM)
  list(APPEND configure_args -A "${GENERATOR_PLATFORM}")
endif()
if(C_COMPILER)
  list(APPEND configure_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(NOT GENERATOR MATCHES "Visual Studio|Xcode|Multi-Config")
  list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${CONFIG}")
endif()

run("${CMAKE_COMMAND}" ${configure_args})
run("${CMAKE_COMMAND}" --build "${CONSUMER_BINARY_DIR}" --config "${CONFIG}")

# pkg-config consumer (Linux): Cflags must point at include/knurbs as well.
if(CHECK_PKG_CONFIG)
  find_program(PKG_CONFIG_EXECUTABLE NAMES pkg-config pkgconf REQUIRED)
  set(ENV{PKG_CONFIG_PATH} "${PREFIX}/${LIBRARY_DESTINATION}/pkgconfig")

  set(packages libobs)
  if(CHECK_FRONTEND_API)
    list(APPEND packages obs-frontend-api)
  endif()

  foreach(package IN LISTS packages)
    execute_process(
      COMMAND "${PKG_CONFIG_EXECUTABLE}" --cflags-only-I ${package}
      RESULT_VARIABLE result
      OUTPUT_VARIABLE cflags
      ERROR_VARIABLE error
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "pkg-config could not find ${package}: ${error}")
    endif()
    if(NOT cflags STREQUAL "-I${include_dir}")
      message(FATAL_ERROR "pkg-config ${package} include flags are '${cflags}', expected '-I${include_dir}'")
    endif()

    execute_process(
      COMMAND "${PKG_CONFIG_EXECUTABLE}" --libs-only-L ${package}
      OUTPUT_VARIABLE ldflags
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT ldflags STREQUAL "-L${PREFIX}/${LIBRARY_DESTINATION}")
      message(
        FATAL_ERROR
        "pkg-config ${package} library flags are '${ldflags}', expected '-L${PREFIX}/${LIBRARY_DESTINATION}'"
      )
    endif()
  endforeach()

  execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --cflags ${packages}
    OUTPUT_VARIABLE cflags
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  separate_arguments(cflags UNIX_COMMAND "${cflags}")
  # Warnings as errors is the consumer's choice, not something libobs imposes.
  foreach(flag IN LISTS cflags)
    if(flag MATCHES "^-Werror($|=)" AND NOT flag STREQUAL "-Werror=return-type")
      message(FATAL_ERROR "pkg-config ${packages} Cflags force '${flag}' on consumers")
    endif()
  endforeach()
  set(source "${CONSUMER_SOURCE_DIR}/consumer.c")
  if(CHECK_FRONTEND_API)
    set(source "${CONSUMER_SOURCE_DIR}/consumer-frontend.c")
  endif()
  run("${C_COMPILER}" ${cflags} -c "${source}" -o "${CONSUMER_BINARY_DIR}/pkg-config-consumer.o")
endif()

message(STATUS "Install layout OK: headers in ${include_dir}")
