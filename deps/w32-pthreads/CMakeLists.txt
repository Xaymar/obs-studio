cmake_minimum_required(VERSION 3.28...3.30)

add_library(w32-pthreads SHARED EXCLUDE_FROM_ALL)
add_library(OBS::w32-pthreads ALIAS w32-pthreads)

# Native Win32 implementation of the pthreads-win32 2.9.1 ABI. The public
# headers are unchanged so existing plugins keep loading.
target_sources(
  w32-pthreads
  PRIVATE
    pthread.h
    sched.h
    semaphore.h
    src/attr.c
    src/common.c
    src/cond.c
    src/internal.h
    src/key.c
    src/misc.c
    src/mutex.c
    src/rwlock.c
    src/sync.c
    src/thread.c
    w32-pthreads.rc
)

target_compile_definitions(w32-pthreads PRIVATE __CLEANUP_C PTW32_BUILD _WIN32_WINNT=0x0A00)

target_include_directories(w32-pthreads PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

configure_file(cmake/windows/obs-module.rc.in w32-pthreads.rc)

set_target_properties_obs(w32-pthreads PROPERTIES FOLDER deps)

set_property(TARGET w32-pthreads APPEND PROPERTY PUBLIC_HEADER pthread.h sched.h)

target_export(w32-pthreads)
