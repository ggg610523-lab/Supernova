cmake_minimum_required(VERSION 3.16)
project(win11wm LANGUAGES CXX)

# C++17 across the board; the X11/GLX helpers use POSIX (clock_gettime, fork,
# sigaction) so we keep the GNU extensions available.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# win11wm -- a GPU accelerated, vsynced X11 compositing window manager with a
# Windows 11 ("Fluent") shell. See README.md for the design notes.
#
# The window manager never reparents a client: every managed top level keeps
# being a direct child of the root window and is composited out of the X server
# with XComposite + GLX_EXT_texture_from_pixmap. That is what makes *any* X11
# program work, and it is what keeps dragging smooth.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11       REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCOMPOSITE REQUIRED IMPORTED_TARGET xcomposite)
pkg_check_modules(XDAMAGE   REQUIRED IMPORTED_TARGET xdamage)
pkg_check_modules(XFIXES    REQUIRED IMPORTED_TARGET xfixes)
pkg_check_modules(XRENDER   REQUIRED IMPORTED_TARGET xrender)
# epoxy provides the whole GL/GLX entry point surface, including the extension
# calls we need (glXBindTexImageEXT, glXSwapIntervalEXT, ...). Link *only*
# epoxy: it deliberately loads libGLX/libGL at runtime.
pkg_check_modules(EPOXY     REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(FT2       REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FC        REQUIRED IMPORTED_TARGET fontconfig)
# zlib only inflates the icon PNGs the asset script produced (src/wm/png.cpp).
pkg_check_modules(ZLIB      REQUIRED IMPORTED_TARGET zlib)

add_executable(win11wm
  src/main.cpp
  src/wm/manager.cpp
  src/wm/windows.cpp
  src/wm/input.cpp
  src/wm/draw.cpp
  src/wm/compositor.cpp
  src/wm/text.cpp
  src/wm/apps.cpp
  src/wm/icons.cpp
  src/wm/png.cpp
)
target_include_directories(win11wm PRIVATE src)
target_compile_options(win11wm PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
target_link_libraries(win11wm PRIVATE
  PkgConfig::X11
  PkgConfig::XCOMPOSITE
  PkgConfig::XDAMAGE
  PkgConfig::XFIXES
  PkgConfig::XRENDER
  PkgConfig::EPOXY
  PkgConfig::FT2
  PkgConfig::FC
  PkgConfig::ZLIB
)

