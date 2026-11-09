#pragma once

struct wlr_output;

// Ask the host compositor to fullscreen the nested output's toplevel. This is
// only meaningful for wlroots' Wayland backend; other backends return false.
bool wm_wl_output_set_fullscreen(struct wlr_output* output);

// Ask the host compositor to size the nested output's toplevel to w x h. Only
// meaningful for wlroots' Wayland backend; other backends return false.
bool wm_wl_output_set_size(struct wlr_output* output, int w, int h);
