// Fluid motion framework.
//
// The shell does not animate with canned ease-in/ease-out curves. Every moving
// value is a *continuous* physical system, advanced by frame delta-time so the
// felt speed is independent of the frame rate. This header is the single source
// of that maths: the models are standalone (pure functions or small stateful
// integrators), so they compose -- an effect is a spatial field feeding forces
// into a spring, whose output is then read for deformation, blur or stretching.
//
// Architecture:
//
//   input ─▶ spatial fields ─▶ forces ─▶ spring dynamics ─▶ deformation ─▶ render
//
// Notes that apply throughout:
//   * Every integration step is called with a *delta time in seconds* and clamps
//     or sub-steps internally, so a stalled frame cannot blow the system up.
//   * "Settled" tests exist so a caller knows when to stop repainting; a system
//     that never terminates is a battery leak, not an animation.
//   * Nothing here touches X11 or GL. It is pure maths so it can be unit-tested
//     and reused by the compositor, the shell and the tablet UI alike.
#pragma once

#include <algorithm>
#include <cmath>

namespace wm {
namespace motion {

inline constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ scalars
inline double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Piecewise-smooth interpolation used by every morph below. f(0)=0, f(1)=1 and
// f'(0)=f'(1)=0, which is what makes a transition start and end without a visible
// jolt. Cheaper than a cubic bezier solver and closed-form invertible.
inline double smoothstep(double t) {
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// One step of an exponential approach, frame-rate independent. A linear "move a
// fixed fraction each frame" ease changes its wall-clock speed with the frame
// rate; this does not. tau is the time constant (seconds to cover ~63%).
inline double expFraction(double dtSec, double tauSec) {
    if (tauSec <= 0.0) return 1.0;
    return 1.0 - std::exp(-dtSec / tauSec);
}

// Gaussian spatial weight: w(d) = exp(-d^2 / (2 sigma^2)). The influence of a
// point on everything around it. sigma sets the spread: w(sigma) = 0.61,
// w(2 sigma) = 0.14, w(3 sigma) = 0.011. It is the kernel shared by the
// magnification field, the magnetic force and the deformation field.
inline double gaussian(double d, double sigma) {
    if (sigma <= 0.0) return 0.0;
    return std::exp(-(d * d) / (2.0 * sigma * sigma));
}

// Smooth pulse over a finite window, peaking at its middle: used as the temporal
// shape f(...) of a propagating wave. 0 outside [0, window].
inline double pulse(double t, double window) {
    if (window <= 0.0 || t <= 0.0 || t >= window) return 0.0;
    return std::sin(kPi * (t / window));
}

// --------------------------------------------------------------------- Vec2
// Minimal 2D vector so the field models read like the mathematics.
struct Vec2 {
    double x = 0.0, y = 0.0;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, double s) { return Vec2{a.x * s, a.y * s}; }
inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double length(Vec2 a) { return std::sqrt(dot(a, a)); }

// ============================================================ 1. Spring dynamics
//
//   m x'' + c x' + k (x - x*) = 0
//
// The state is (position, velocity). `stiffness` is k, `mass` is m and `zeta` is
// the damping ratio, c / (2 sqrt(k m)):
//   zeta < 1  underdamped  -> overshoots, "springy", playful
//   zeta = 1  critically   -> fastest arrival with no overshoot (UI default)
//   zeta > 1  overdamped   -> slow, heavy, no overshoot
// `stiffness` sets the natural frequency w = sqrt(k/m): higher is snappier. The
// pair (w, zeta) fully describes the feel; mass only matters through them.
struct Spring {
    double value = 0.0;       // x(t)
    double velocity = 0.0;    // x'(t)
    double mass = 1.0;        // m
    double stiffness = 220.0; // k
    double zeta = 1.0;        // damping ratio

    // Set the spring's natural frequency directly (Hz), keeping the ratio.
    void setFrequency(double hz) { stiffness = (2.0 * kPi * hz) * (2.0 * kPi * hz) * mass; }

    // One frame. For the critically damped case the exact closed-form solution is
    // used, which is unconditionally stable at any dt; otherwise a stable
    // sub-stepped semi-implicit (symplectic) Euler is used. Returns the new value.
    double step(double target, double dtSec) {
        const double dt = clamp(dtSec, 0.0, 0.25);  // ignore absurd deltas
        if (dt <= 0.0) return value;
        const double w = std::sqrt(stiffness / std::max(mass, 1e-9));
        if (std::abs(zeta - 1.0) < 1e-3) {
            // Critically damped exact solution of  e'' + 2w e' + w^2 e = 0,
            // e = x - x*, from (e0, v0):
            //   e(t) = (e0 + (v0 + w e0) t) e^(-w t)
            //   v(t) = (v0 - w (v0 + w e0) t) e^(-w t)
            const double e0 = value - target;
            const double c1 = velocity + w * e0;
            const double decay = std::exp(-w * dt);
            value = target + (e0 + c1 * dt) * decay;
            velocity = (velocity - w * c1 * dt) * decay;
            return value;
        }
        const double c = 2.0 * zeta * std::sqrt(stiffness * mass);
        const int steps = clampi(static_cast<int>(std::ceil(dt / 0.004)), 1, 64);
        const double h = dt / steps;
        for (int i = 0; i < steps; ++i) {
            const double a = (stiffness * (target - value) - c * velocity) / mass;
            velocity += a * h;
            value += velocity * h;
        }
        return value;
    }

    bool settled(double target, double eps = 1e-3) const {
        return std::abs(value - target) < eps && std::abs(velocity) < eps;
    }
};

// ======================================================= 2. Magnetic attraction
//
//   F(x) = k (p - x) exp( -|p - x|^2 / (2 sigma^2) )
//
// A soft pull toward the pointer that vanishes with distance (the Gaussian kills
// it: past ~3 sigma there is effectively no force). k is the strength, sigma the
// reach. Because the force is continuous and C-infinity there are no seams.
inline double magneticForce(double x, double p, double k, double sigma) {
    const double d = p - x;
    return k * d * gaussian(d, sigma);
}

inline Vec2 magneticForce(Vec2 x, Vec2 p, double k, double sigma) {
    const Vec2 d = p - x;
    return d * (k * gaussian(length(d), sigma));
}

// ================================================================ 3. Repulsion
//
//   F(x) = A (x - p) / (|x - p| + eps)^n
//
// Pushes x away from p. eps is a soft core that keeps the force finite as
// |x-p| -> 0 (a bare inverse power is singular and would explode on contact).
// n sets how sharply the force falls off: n=2 is gravity-like, larger n is
// tighter and more local.
inline double repulsion(double x, double p, double A, double n, double eps = 1.0) {
    const double d = x - p;
    return A * d / std::pow(std::abs(d) + eps, n);
}

inline Vec2 repulsion(Vec2 x, Vec2 p, double A, double n, double eps = 1.0) {
    const Vec2 d = x - p;
    return d * (A / std::pow(length(d) + eps, n));
}

// ========================================================== 4. Fluid deformation
//
//   x' = x + D(x),   D(x) = A exp( -|x - p|^2 / (2 sigma^2) ) (unit direction)
//
// A displacement *field* rather than a rigid move: points near p shift the most,
// points far away not at all, so a shape bends under the pointer like soft
// rubber instead of translating. A is the peak displacement, sigma its reach.
inline double deform(double x, double p, double A, double sigma) {
    return x + A * gaussian(x - p, sigma);
}

inline Vec2 deform(Vec2 x, Vec2 p, Vec2 dir, double A, double sigma) {
    const double w = gaussian(length(p - x), sigma);
    return x + dir * (A * w);
}

// ================================================================= 5. Parallax
//
//   x_i' = x_i + lambda_i * dp
//
// Depth by differential motion: a background layer (small lambda), a middle
// ground and a foreground (large lambda) move by different amounts for the same
// pointer delta dp. lambda is the layer's depth coefficient.
inline double parallax(double x, double dp, double lambda) { return x + lambda * dp; }

// ======================================================== 6. Inertial scrolling
//
//   v(t) = v0 e^(-k t),   x(t) = x0 + (v0 / k) (1 - e^(-k t))
//
// The closed form of "flick with friction": the whole trajectory for any t, so
// the caller can sample it rather than integrate it. k is the drag (1/time); the
// total glide distance is v0/k, and the motion only stops asymptotically.
inline double inertialVelocity(double v0, double k, double t) {
    return v0 * std::exp(-k * t);
}

inline double inertialPosition(double x0, double v0, double k, double t) {
    if (k <= 1e-9) return x0 + v0 * t;
    return x0 + (v0 / k) * (1.0 - std::exp(-k * t));
}

// The distance a flick of velocity v0 will travel before it is effectively over.
inline double inertialDistance(double v0, double k) { return k > 1e-9 ? v0 / k : 0.0; }

// ====================================================== 7. Rubber-band boundaries
//
//   F = -k sign(x) |x|^n
//
// Beyond the boundary the object still moves, but with growing resistance, so the
// stretch is progressive and it springs back smoothly on release. n>1 is the
// nonlinear version (soft at first, firm at the end); n=1 is a plain spring.
inline double rubberForce(double overshoot, double k, double n) {
    const double s = overshoot >= 0.0 ? 1.0 : -1.0;
    return -k * s * std::pow(std::abs(overshoot), n);
}

// The Apple-style positional map: inside [lo, hi] identity, outside it asymptotes
// toward a finite overshoot so the content can never be dragged arbitrarily far.
// c is the resistance constant (0.55 is the familiar feel).
inline double rubberBand(double x, double lo, double hi, double dim, double c = 0.55) {
    if (x < lo) {
        const double d = lo - x;
        return lo - (1.0 - 1.0 / (d * c / std::max(dim, 1.0) + 1.0)) * dim * c;
    }
    if (x > hi) {
        const double d = x - hi;
        return hi + (1.0 - 1.0 / (d * c / std::max(dim, 1.0) + 1.0)) * dim * c;
    }
    return x;
}

// ==================================================== 8. Velocity-dependent blur
//
//   B = B0 + alpha |v|
//
// Motion blur by speed: a fast-moving surface blurs more, a stationary one sits
// at its base blur B0. alpha is the gain; because it is linear in speed it
// returns to B0 continuously as the motion decays.
inline double velocityBlur(double speed, double base, double alpha) {
    return base + alpha * std::abs(speed);
}

// ============================================== 9. Velocity-based stretching
//
//   S_parallel = 1 + alpha |v|,   S_perp = 1 - beta |v|
//
// Squash-and-stretch along the direction of travel: an object elongates as it
// moves fast and recovers when it stops. Kept clamped so a large velocity cannot
// collapse the perpendicular axis.
inline void velocityStretch(double speed, double alpha, double beta, double* along,
                            double* across) {
    const double v = std::abs(speed);
    if (along) *along = 1.0 + alpha * v;
    if (across) *across = std::max(0.05, 1.0 - beta * v);
}

// ============================================================ 10. Smooth morphing
//
//   P(t) = (1 - f(t)) P_A + f(t) P_B,   f(t) = 3t^2 - 2t^3
//
// State transitions interpolate *every* property through one progress value, so
// position, scale, opacity, colour, radius, blur, shadow and rotation all move
// together and none can pop. f is smoothstep (zero slope at both ends).
inline double morph(double a, double b, double f) { return a + (b - a) * f; }

struct MorphState {
    double x = 0.0, y = 0.0;
    double scale = 1.0;
    double opacity = 1.0;
    double radius = 0.0;
    double blur = 0.0;
    double shadow = 0.0;
    double rotation = 0.0;
};

inline MorphState morph(const MorphState& a, const MorphState& b, double t) {
    const double f = smoothstep(t);
    MorphState r;
    r.x = morph(a.x, b.x, f);
    r.y = morph(a.y, b.y, f);
    r.scale = morph(a.scale, b.scale, f);
    r.opacity = morph(a.opacity, b.opacity, f);
    r.radius = morph(a.radius, b.radius, f);
    r.blur = morph(a.blur, b.blur, f);
    r.shadow = morph(a.shadow, b.shadow, f);
    r.rotation = morph(a.rotation, b.rotation, f);
    return r;
}

// ============================================================= 11. Wave propagation
//
//   A_i(t) = A e^(-lambda |i - i0|) f(t - tau |i - i0|)
//
// An interaction at i0 travels outward: neighbouring elements react later (delay
// tau per step) and weaker (decay lambda per step). The delay is what makes it
// read as a wave rather than a simultaneous pulse. `duration` is the width of f.
inline double wave(double amp, double lambda, double tau, int i, int i0, double t,
                   double duration) {
    const double dist = std::abs(double(i - i0));
    const double local = t - tau * dist;
    if (local <= 0.0 || local >= duration) return 0.0;
    return amp * std::exp(-lambda * dist) * pulse(local, duration);
}

// ===================================================== 12. Spatial magnification
//
//   w(d) = exp(-d^2 / (2 sigma^2)),   S*(d) = S0 + A w(d)
//
// The pointer acts as a smooth field over items; each item's *target* scale is
// this, and a Spring (1) turns that target into motion so nothing changes
// instantly. S0 is the resting scale, A the peak growth, sigma the reach.
inline double magnificationTarget(double d, double s0, double A, double sigma) {
    return s0 + A * gaussian(d, sigma);
}

}  // namespace motion
}  // namespace wm
