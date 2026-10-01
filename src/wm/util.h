// Small shared helpers: geometry, monotonic timing, Fluent easing, logging.
// Kept header-only so every translation unit of the WM agrees on the same
// trivial definitions without a link-time dependency on a fat "common" lib.
#pragma once

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace wm {

// ------------------------------------------------------------------ geometry
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(int X, int Y, int W, int H) : x(X), y(Y), w(W), h(H) {}
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
    bool intersects(const Rect& o) const {
        return !(o.x >= right() || o.right() <= x || o.y >= bottom() || o.bottom() <= y);
    }
    Rect inflated(int d) const { return Rect{x - d, y - d, w + 2 * d, h + 2 * d}; }
    Rect moved(int dx, int dy) const { return Rect{x + dx, y + dy, w, h}; }
    bool operator==(const Rect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

struct Point {
    int x = 0, y = 0;
};

inline Rect lerpRect(const Rect& a, const Rect& b, double t) {
    const auto l = [t](int p, int q) { return int(std::lround(p + (q - p) * t)); };
    return Rect{l(a.x, b.x), l(a.y, b.y), l(a.w, b.w), l(a.h, b.h)};
}

// Clamp `r` inside `bounds`, keeping the frame's caption reachable.
inline Rect clampRect(Rect r, const Rect& bounds) {
    if (r.w > bounds.w) r.w = bounds.w;
    if (r.h > bounds.h) r.h = bounds.h;
    r.x = r.x < bounds.x ? bounds.x : (r.x + r.w > bounds.right() ? bounds.right() - r.w : r.x);
    r.y = r.y < bounds.y ? bounds.y : (r.y + r.h > bounds.bottom() ? bounds.bottom() - r.h : r.y);
    return r;
}

// ------------------------------------------------------------------- timing
// Monotonic milliseconds: the only clock the compositor paces itself on.
inline double nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
}

inline double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

// ----------------------------------------------------------------- motion
// Move `v` a fixed fraction of the way to `target`, paced by the frame's delta
// time. This is the one stepper the whole shell uses -- hover washes, flyouts,
// minimise, the snap preview -- so every transition shares a single feel and can
// be reversed mid-flight. Returns true while `v` still has somewhere to go, so
// the caller knows to keep repainting.
inline bool approach(double& v, double target, double dtMs, double ms) {
    if (v == target) return false;
    const double d = ms > 0.0 ? dtMs / ms : 1.0;
    if (target > v) v = (v + d > target) ? target : v + d;
    else v = (v - d < target) ? target : v - d;
    return true;
}

// ------------------------------------------------------------------- easing
// Cubic bezier with p0=(0,0) and p3=(1,1). Windows 11 ("Fluent") motion uses
// cubic-bezier(0.1, 0.9, 0.2, 1.0); we solve x(t)=x by bisection, which is
// plenty accurate for a couple of dozen windows per frame.
inline double bezierAxis(double t, double p1, double p2) {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
}

inline double fluentEase(double x, double x1 = 0.1, double y1 = 0.9,
                         double x2 = 0.2, double y2 = 1.0) {
    const double target = clamp01(x);
    if (target <= 0.0) return 0.0;
    if (target >= 1.0) return 1.0;
    double lo = 0.0, hi = 1.0, mid = target;
    for (int i = 0; i < 20; ++i) {
        mid = 0.5 * (lo + hi);
        if (bezierAxis(mid, x1, x2) < target) lo = mid;
        else hi = mid;
    }
    return clamp01(bezierAxis(mid, y1, y2));
}

// The "decelerate" curve Fluent uses for things that appear on screen.
inline double easeOutCubic(double t) {
    t = clamp01(t);
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

inline double easeOutQuad(double t) {
    t = clamp01(t);
    return 1.0 - (1.0 - t) * (1.0 - t);
}

// ------------------------------------------------------------------ logging
inline void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[win11wm] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

// Directory the running binary lives in, so bundled assets can be found
// regardless of the current working directory.
inline std::string executableDir() {
    char buf[4096];
    const long n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string path(buf);
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline bool isDirectory(const std::string& p) {
    struct stat st;
    return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Where the bundled assets live. $WIN11WM_ASSETS wins; otherwise walk up from
// the executable, because the binary is built into build/ while the assets stay
// at the top of the checkout. A relocated install (bin/ next to assets/) still
// resolves on the first try.
inline std::string defaultAssetDir() {
    if (const char* env = std::getenv("WIN11WM_ASSETS")) {
        if (env[0] != '\0') return env;
    }
    std::string dir = executableDir();
    for (int depth = 0; depth < 4 && !dir.empty() && dir != "/"; ++depth) {
        const std::string candidate = dir + "/assets";
        if (isDirectory(candidate)) return candidate;
        const size_t slash = dir.rfind('/');
        dir = slash == std::string::npos ? std::string() : dir.substr(0, slash);
    }
    const std::string exe = executableDir();
    return exe.empty() ? "assets" : exe + "/assets";
}

}  // namespace wm
