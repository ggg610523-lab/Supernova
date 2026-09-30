// Shared X11 property readers used by the manager and the window bookkeeping.
//
// Header-only and inline so both translation units see exactly the same
// implementation without a link-time dependency on anything.
#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>

namespace wm {

// Reads a UTF-8 (or, for broken clients, 32 bit packed) text property.
inline bool atomName(Display* dpy, Window w, Atom prop, std::string* out) {
    out->clear();
    Atom type = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, w, prop, 0, 4096, False, AnyPropertyType, &type, &format,
                           &items, &after, &data) != Success) {
        return false;
    }
    if (data && format == 8) {
        out->assign(reinterpret_cast<char*>(data), items);
    } else if (data && format == 32) {
        const unsigned char* p = data;
        for (unsigned long i = 0; i < items; ++i) {
            const unsigned long v = *reinterpret_cast<const unsigned long*>(p);
            if (v) out->push_back(static_cast<char>(v & 0xFF));
            p += sizeof(long);
        }
    }
    if (data) XFree(data);
    return !out->empty();
}

// Reads a single 32 bit cardinal property (e.g. _NET_WM_PID, _NET_WM_OPACITY).
inline bool cardProperty(Display* dpy, Window w, Atom prop, unsigned long* out) {
    Atom type = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_CARDINAL, &type, &format, &items,
                           &after, &data) != Success) {
        return false;
    }
    bool ok = false;
    if (data && items >= 1 && format == 32) {
        *out = *reinterpret_cast<unsigned long*>(data);
        ok = true;
    }
    if (data) XFree(data);
    return ok;
}

// True when the atom list property `prop` contains `value` (e.g. is
// _NET_WM_WINDOW_TYPE_DESKTOP in _NET_WM_WINDOW_TYPE, or WM_DELETE_WINDOW in
// WM_PROTOCOLS).
inline bool windowHasAtom(Display* dpy, Window w, Atom prop, Atom value) {
    Atom type = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, w, prop, 0, 64, False, XA_ATOM, &type, &format, &items, &after,
                           &data) != Success) {
        return false;
    }
    bool found = false;
    if (data && format == 32) {
        const Atom* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < items && !found; ++i) found = (atoms[i] == value);
    }
    if (data) XFree(data);
    return found;
}

}  // namespace wm
