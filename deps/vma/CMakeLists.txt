# Header-only; the single VMA_IMPLEMENTATION translation unit lives in libobs-vulkan.
add_library(vma INTERFACE)
add_library(OBS::vma ALIAS vma)
target_include_directories(vma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
