cmake_minimum_required(VERSION 3.16)
project(tvshell LANGUAGES CXX)

# A standalone 10-foot TV compositor for the Raspberry Pi 3. It is deliberately
# independent of the desktop shell build: the Wayland/GLES2 renderer, text
# rasteriser and icon store are reused as source files, but the policy and UI in
# this directory are TV-only.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(TV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SUPERNOVA_ROOT "${TV_ROOT}/..")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS  REQUIRED IMPORTED_TARGET wlroots-0.18)
pkg_check_modules(WLSERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(WLCLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(XKB      REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(GL       REQUIRED IMPORTED_TARGET egl glesv2)
pkg_check_modules(FT2      REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FC       REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(ZLIB     REQUIRED IMPORTED_TARGET zlib)

add_executable(tvshell
  main.cpp
  manager.cpp
  ui.cpp
  controller.cpp
  ${SUPERNOVA_ROOT}/src/wl/compositor.cpp
  ${SUPERNOVA_ROOT}/src/wm/text.cpp
  ${SUPERNOVA_ROOT}/src/wm/icons.cpp
  ${SUPERNOVA_ROOT}/src/wm/png.cpp
  ${SUPERNOVA_ROOT}/src/wm/apps.cpp
)

# src/wl/include first: its epoxy/gl.h shim maps the shared icon header onto
# GLES2 instead of the desktop GL entry points.
target_include_directories(tvshell PRIVATE
  ${TV_ROOT}
  ${SUPERNOVA_ROOT}/src/wl/include
  ${SUPERNOVA_ROOT}/src/wl
  ${SUPERNOVA_ROOT}/src/wm
  ${SUPERNOVA_ROOT}/src
)

target_compile_definitions(tvshell PRIVATE WLR_USE_UNSTABLE)
target_compile_options(tvshell PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
target_link_libraries(tvshell PRIVATE
  PkgConfig::WLROOTS
  PkgConfig::WLSERVER
  PkgConfig::WLCLIENT
  PkgConfig::XKB
  PkgConfig::GL
  PkgConfig::FT2
  PkgConfig::FC
  PkgConfig::ZLIB
)

# Optional SVG rendering, same as the desktop build: without librsvg the icon
# store still resolves raster PNGs and installed icon themes.
pkg_check_modules(CAIRO IMPORTED_TARGET cairo)
pkg_check_modules(RSVG  IMPORTED_TARGET librsvg-2.0)
if(CAIRO_FOUND AND RSVG_FOUND)
  target_compile_definitions(tvshell PRIVATE WIN11WM_HAVE_RSVG)
  target_link_libraries(tvshell PRIVATE PkgConfig::CAIRO PkgConfig::RSVG)
endif()
