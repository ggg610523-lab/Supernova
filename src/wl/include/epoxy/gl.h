// GLES2 shim for the Wayland build.
//
// The shared shell units (text.cpp, icons.cpp, icons.h) were written against
// <epoxy/gl.h>, which declares the desktop GL and GLES entry points at once.
// wlroots' public headers pull in <GLES2/gl2.h> directly, and the two disagree
// about the KHRONOS boolean enum when they meet in one translation unit.
//
// The Wayland renderer only ever needs the GLES2 entry points, so this header
// stands in for epoxy and includes the real thing. It is reachable only through
// the win11wm-wl target's include path; the X11 target still uses libepoxy.
#pragma once

#include <GLES2/gl2.h>

// GLES2 has no sized RGBA8 internal format; the unsized one is the same storage
// for these 8-bit uploads (text glyphs, icons, wallpaper).
#ifndef GL_RGBA8
#define GL_RGBA8 GL_RGBA
#endif
