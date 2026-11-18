# Placeholder: vk_mem_alloc.h (MIT, header-only) is not vendored yet; Task 2 adds it.
add_library(vma INTERFACE)
add_library(OBS::vma ALIAS vma)
target_include_directories(vma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
