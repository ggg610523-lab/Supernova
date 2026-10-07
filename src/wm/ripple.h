// HarmonyOS-style app launch ripple animation
//
// A spatial ripple that radiates from an origin point and expands until it reaches
// its target frame. Used for app launches to give the impression of expanding from
// the home screen icon into a full-screen window. The ripple has:
//
//   1. Wave front that expands radially (origin -> target)
//   2. Scale morphing (icon size -> app frame size)
//   3. Opacity transitions (icon visible -> ripple peak -> content visible)
//   4. Status bar parallax (stays fixed while content moves beneath)
//
// The animation is frame-rate independent and uses spring dynamics from motion.h
// to ensure smooth deceleration.
#pragma once

#include <cmath>
#include "motion.h"

namespace wm {

// ============================================================================
// Ripple launch state: animates from icon rect to app frame
// ============================================================================
struct RippleState {
    // Ripple expansion: 0 = at icon, 1 = at target frame
    double progress = 0.0;

    // Visual parameters
    double scale = 1.0;           // geometric scale (1.0 at icon, ~1.15 at peak)
    double opacity = 1.0;         // overall alpha (0.8 -> 1.0 -> 0.0 -> 1.0)
    double rippleRadius = 0.0;    // visual ripple wave radius (px)
    double rippleOpacity = 1.0;   // ripple wave opacity (fades as it expands)

    // Content state
    double contentOpacity = 0.0;  // window content fades in after ripple passes
    double cornerRadius = 18.0;   // morphs from icon radius to app corner radius

    // Parallax offset for status bar effect
    double statusBarY = 0.0;      // status bar stays anchored, content shifts under

    // Internal: spring integrators for smooth motion
    motion::Spring progressSpring;
    motion::Spring scaleSpring;
    motion::Spring opacitySpring;
};

// ============================================================================
// Ripple animator: drives the launch sequence
// ============================================================================
class RippleAnimator {
public:
    RippleAnimator() {
        // Configure springs for a playful HarmonyOS feel
        // Phase 1 (0-0.4): Icon scale up + ripple out
        // Phase 2 (0.4-0.8): Ripple peak, content fade in
        // Phase 3 (0.8-1.0): Content settles, ripple fades
        state.progressSpring.configure(3.2, 0.75);    // Playful overshoot
        state.scaleSpring.configure(3.5, 0.80);        // Slightly springy
        state.opacitySpring.configure(2.8, 0.75);      // Smooth fade
    }

    // Advance animation by dt seconds
    void step(double dtSec) {
        // Drive progress spring toward completion (1.0)
        state.progressSpring.step(1.0, dtSec);
        const double p = state.progressSpring.value;

        // Phase-based animation curve
        if (p < 0.3) {
            // Phase 1: Icon bounces & scales up
            // Slight undershoot then overshoot (playful feel)
            const double phase1 = p / 0.3;
            state.scale = 1.0 + 0.12 * motion::smoothstep(phase1);
            state.rippleOpacity = 1.0;
            state.contentOpacity = 0.0;
        } else if (p < 0.65) {
            // Phase 2: Ripple expands, content starts fading in
            const double phase2 = (p - 0.3) / 0.35;
            state.scale = 1.12 * (1.0 - phase2 * 0.08);  // Scale back down
            state.rippleOpacity = 1.0 * (1.0 - phase2 * 1.2);  // Ripple fades
            state.contentOpacity = motion::smoothstep(phase2) * 0.7;  // Content rises
        } else {
            // Phase 3: Content fully visible, ripple gone
            state.scale = 1.04;
            state.rippleOpacity = 0.0;
            state.contentOpacity = 1.0 + (p - 0.65) * 0.3;  // Final boost to 1.0
        }

        // Morphing corner radius: icon (18px) -> content (rounded, ~12px for app)
        state.cornerRadius = 18.0 - p * 6.0;

        // Ripple radius expands with progress
        // Peak at ~140% of diagonal, then freezes
        const double ripplePeak = p > 0.65 ? 1.4 : (0.8 + 0.6 * p / 0.65);
        state.rippleRadius = ripplePeak;  // normalized 0-1.4
    }

    // Returns true if animation has settled
    bool settled() const {
        return state.progressSpring.settled(1.0, 0.01);
    }

    const RippleState& getState() const { return state; }
    RippleState& getMutableState() { return state; }

    // Reset for a new launch
    void reset() {
        state.progressSpring.adopt(0.0, true);
        state.scaleSpring.adopt(1.0, true);
        state.opacitySpring.adopt(1.0, true);
        state.progress = 0.0;
        state.scale = 1.0;
        state.opacity = 1.0;
        state.rippleRadius = 0.0;
        state.rippleOpacity = 1.0;
        state.contentOpacity = 0.0;
        state.cornerRadius = 18.0;
        state.statusBarY = 0.0;
    }

private:
    RippleState state;
};

}  // namespace wm
