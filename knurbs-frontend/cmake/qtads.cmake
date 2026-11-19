# Qt Advanced Docking System (LGPL-2.1).
# Prefer an installed package (obs-deps or system); otherwise fetch the pinned release.

find_package(qt6advanceddocking CONFIG QUIET)

if(TARGET ads::qt6advanceddocking)
  add_library(Knurbs::qtads ALIAS ads::qt6advanceddocking)
  return()
endif()

include(FetchContent)

set(KNURBS_QTADS_VERSION 4.3.1 CACHE STRING "Qt Advanced Docking System release to fetch")
mark_as_advanced(KNURBS_QTADS_VERSION)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(QT_VERSION_MAJOR 6 CACHE STRING "" FORCE)
set(ADS_VERSION ${KNURBS_QTADS_VERSION} CACHE STRING "" FORCE)

FetchContent_Declare(
  qtads
  GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
  GIT_TAG ${KNURBS_QTADS_VERSION}
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(qtads)

# Note: on Linux, ADS 4.x links libxcb unconditionally for its floating
# windows, although Knurbs runs on Wayland only. Dropping that needs a patch
# to ADS.
if(TARGET qt6advanceddocking)
  # ADS includes <qpa/qplatformnativeinterface.h>. Since Qt 6.10, private
  # headers are only on the include path through the *Private targets, and
  # ADS links only Core, Gui and Widgets.
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
  target_link_libraries(qt6advanceddocking PRIVATE Qt6::GuiPrivate)
  # Third-party code: don't fail the build on its warnings (deprecations and
  # [[nodiscard]] in newer Qt releases).
  set_target_properties(qt6advanceddocking PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  add_library(Knurbs::qtads ALIAS qt6advanceddocking)
else()
  message(FATAL_ERROR "Qt Advanced Docking System target not found after fetching ${KNURBS_QTADS_VERSION}")
endif()
