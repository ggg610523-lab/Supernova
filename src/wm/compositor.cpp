#include "compositor.h"

#include <X11/extensions/Xcomposite.h>

#include <cstring>
#include <vector>

namespace wm {
namespace {

// ---------------------------------------------------------------- unit quad
// Every primitive is drawn as this one quad, placed by uRect in screen pixels.
constexpr float kQuad[8] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};

const char* const kVertexSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat4 uProj;
uniform vec4 uRect;      // x, y, w, h in screen pixels
out vec2 vQ;             // 0..1 across the quad
void main() {
    vQ = aPos;
    gl_Position = uProj * vec4(uRect.xy + aPos * uRect.zw, 0.0, 1.0);
}
)GLSL";

// The whole Windows 11 shell is one shader. Modes:
//   0 = rounded solid fill        (buttons, pills, dividers)
//   1 = acrylic surface           (taskbar, Start, flyouts)
//   2 = window                    (shadow + Mica caption + border + content)
//   3 = textured quad             (text, icons, thumbnails)
const char* const kFragmentSrc = R"GLSL(
#version 330 core
in vec2 vQ;
out vec4 fragColor;

uniform int   uMode;
uniform vec4  uRect;
uniform vec4  uColor;     // primary fill / caption tint
uniform vec4  uColor2;    // caption glyph colour
uniform vec4  uBorder;    // hairline border colour (a = strength)
uniform vec4  uShadow;    // drop shadow colour (a = strength)
uniform vec4  uClose;     // close button wash
uniform vec4  uFrame;     // outer window frame
uniform vec4  uContent;   // client texture rect in screen pixels
uniform float uRadius;
uniform float uOpacity;
uniform float uCaptionH;
uniform float uBtnW;
uniform vec3  uHover;     // 0..1 per caption button, ordered min/max/close
uniform vec3  uPress;
uniform float uMaximized;
uniform vec2  uScreen;
uniform float uShadowPad;
uniform float uTexMix;    // 1 = take rgb from the texture
uniform float uKeepAlpha; // 1 = take alpha from the texture
uniform float uTintAmount;
uniform sampler2D uTex;
uniform sampler2D uBlur;

// Signed distance to a rounded box, the basis of every Fluent shape here.
float sdRound(vec2 p, vec2 b, float r) {
    r = max(0.0, min(r, min(b.x, b.y)));
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// 1px analytic antialiasing: d is in pixels, 0 on the edge.
float cover(float d) { return clamp(0.5 - d, 0.0, 1.0); }

float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

float boxRing(vec2 p, vec2 halfSize, float thick) {
    return abs(sdRound(p, halfSize, 0.0)) - thick * 0.5;
}

// The three Windows 11 caption glyphs, drawn from distances so the diagonals
// of the close button antialias properly at any DPI.
float glyphCoverage(vec2 p, int idx, float maximized) {
    float d;
    if (idx == 0) {
        d = segDist(p, vec2(-5.0, 0.0), vec2(5.0, 0.0)) - 0.5;            // minimise
    } else if (idx == 2) {
        d = min(segDist(p, vec2(-4.5, -4.5), vec2(4.5, 4.5)),
                segDist(p, vec2(4.5, -4.5), vec2(-4.5, 4.5))) - 0.5;       // close
    } else if (maximized > 0.5) {
        d = min(boxRing(p + vec2(-1.5, -1.5), vec2(3.5, 3.5), 1.0),       // restore
                boxRing(p + vec2(1.5, 1.5), vec2(3.5, 3.5), 1.0));
    } else {
        d = boxRing(p, vec2(4.5, 4.5), 1.0);                             // maximise
    }
    return cover(d);
}

// Mica: the depth effect Windows 11 uses for title bars. We tint a blurred
// sample of the wallpaper that sits *behind* the window (like the real thing)
// and crush its luminance so text stays readable.
vec3 micaTint(vec2 p, vec3 tint, float amount) {
    vec2 uv = clamp(p / uScreen, vec2(0.0), vec2(1.0));
    vec3 c = texture(uBlur, uv).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(l) * 0.30, tint, amount);
}

void main() {
    vec2 p = uRect.xy + vQ * uRect.zw;   // this fragment's screen pixel
    vec2 c = uRect.xy + uRect.zw * 0.5;

    if (uMode == 0) {                    // rounded solid fill
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        float a = cover(d) * uOpacity * uColor.a;
        fragColor = vec4(uColor.rgb * a, a);
        return;
    }

    if (uMode == 1) {                    // acrylic surface
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        vec2 uv = clamp(p / uScreen, vec2(0.0), vec2(1.0));
        vec3 col = texture(uBlur, uv).rgb;
        // Acrylic is a saturated, slightly luminous wash: push the colour away
        // from grey, lift it with a luminosity layer, then take the tint.
        float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(l), col, 1.45);
        col = mix(col, uColor.rgb, uTintAmount);
        // Grain is luminance only; coloured speckle reads as dirt, not acrylic.
        col += vec3(hash21(floor(p) + 0.5) - 0.5) * 0.016;
        col = mix(col, uBorder.rgb, cover(abs(d + 0.5) - 0.5) * uBorder.a);
        float a = cover(d) * uOpacity * uColor.a;
        fragColor = vec4(col * a, a);
        return;
    }

    if (uMode == 2) {                    // a whole Fluent window
        vec2 halfSize = uFrame.zw * 0.5;
        vec2 fc = uFrame.xy + halfSize;
        float r = mix(uRadius, 0.0, uMaximized);
        float d = sdRound(p - fc, halfSize, r);
        float shape = cover(d);

        // Drop shadow: the same rounded box, pushed down and grown a little.
        vec2 sc = fc + vec2(0.0, 7.0);
        float ds = sdRound(p - sc, halfSize + vec2(3.0, 3.0), r + 3.0);
        float sh = clamp(1.0 - ds / max(uShadowPad, 1.0), 0.0, 1.0);
        sh = sh * sh * uShadow.a;

        vec3 body = vec3(0.12, 0.12, 0.13);
        float alphaMul = 1.0;
        if (d < 0.0) {
            if (p.y - uFrame.y < uCaptionH) {
                body = micaTint(p, uColor.rgb, uTintAmount);
                float dRight = (uFrame.x + uFrame.z) - p.x;   // .z is the width
                int cell = int(floor(max(dRight, 0.0) / max(uBtnW, 1.0)));
                if (cell < 3) {
                    int idx = 2 - cell;      // 0 minimise, 1 maximise, 2 close
                    float hov = idx == 2 ? uHover.z : (idx == 1 ? uHover.y : uHover.x);
                    float prs = idx == 2 ? uPress.z : (idx == 1 ? uPress.y : uPress.x);
                    float amt = idx == 2 ? clamp(hov * uClose.a + prs * 0.14, 0.0, 1.0)
                                         : clamp(hov * 0.06 + prs * 0.10, 0.0, 1.0);
                    body = mix(body, idx == 2 ? uClose.rgb : vec3(1.0), amt);
                    vec2 bc = vec2(uFrame.x + uFrame.z - (float(cell) + 0.5) * uBtnW,
                                   uFrame.y + uCaptionH * 0.5);
                    body = mix(body, uColor2.rgb,
                               glyphCoverage(p - bc, idx, uMaximized) * uColor2.a);
                }
            } else {
                vec2 uv = (p - uContent.xy) / max(uContent.zw, vec2(1.0));
                if (uTexMix > 0.5 && uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
                    vec4 t = texture(uTex, uv);
                    body = t.rgb;
                    alphaMul = mix(1.0, t.a, uKeepAlpha);
                }
            }
            body = mix(body, uBorder.rgb, cover(abs(d + 0.5) - 0.5) * uBorder.a);
        }
        float bodyA = shape * uOpacity * alphaMul;
        float a = bodyA + sh * (1.0 - bodyA);
        fragColor = vec4(body * bodyA + uShadow.rgb * sh * (1.0 - bodyA), a);
        return;
    }

    // Mode 3: textured quad (text, icons, thumbnails, the wallpaper itself).
    float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
    vec4 t = texture(uTex, vQ);
    vec3 rgb = mix(uColor.rgb, t.rgb * uColor.rgb, uTexMix);
    float a = cover(d) * uOpacity * mix(uColor.a, t.a, uKeepAlpha);
    fragColor = vec4(rgb * a, a);
}
)GLSL";

// Separable Gaussian blur, used to build the acrylic / Mica source texture from
// the wallpaper. It runs at half resolution, so nine taps go a long way.
const char* const kBlurSrc = R"GLSL(
#version 330 core
in vec2 vQ;
out vec4 fragColor;
uniform vec4 uRect;
uniform vec2 uBlurDir;
uniform sampler2D uTex;
void main() {
    vec4 sum = texture(uTex, vQ) * 0.227027;
    sum += (texture(uTex, vQ + uBlurDir) + texture(uTex, vQ - uBlurDir)) * 0.1945946;
    sum += (texture(uTex, vQ + uBlurDir * 2.0) + texture(uTex, vQ - uBlurDir * 2.0)) * 0.1216216;
    sum += (texture(uTex, vQ + uBlurDir * 3.0) + texture(uTex, vQ - uBlurDir * 3.0)) * 0.0540540;
    sum += (texture(uTex, vQ + uBlurDir * 4.0) + texture(uTex, vQ - uBlurDir * 4.0)) * 0.0162162;
    fragColor = sum;
}
)GLSL";

// The wallpaper: a procedural take on the Windows 11 "Bloom" dark wallpaper, so
// the shell needs no image assets and no wallpaper reader. Baked once into a
// texture at start up.
const char* const kWallpaperSrc = R"GLSL(
#version 330 core
in vec2 vQ;
out vec4 fragColor;
uniform vec2 uWallRes;
float hash21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1.0, 0.0)), f.x),
               mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), f.x), f.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s;
}
void main() {
    float aspect = uWallRes.x / max(uWallRes.y, 1.0);
    vec2 p = (vQ - 0.5) * vec2(aspect, 1.0);
    vec3 col = mix(vec3(0.012, 0.024, 0.050), vec3(0.043, 0.086, 0.160),
                   pow(1.0 - vQ.y, 1.2));
    vec3 glow = vec3(0.0);
    glow += vec3(0.10, 0.34, 0.78) * exp(-dot(p - vec2(-0.62, 0.16), p - vec2(-0.62, 0.16)) * 4.0);
    glow += vec3(0.04, 0.52, 0.62) * exp(-dot(p - vec2( 0.55, 0.30), p - vec2( 0.55, 0.30)) * 5.0);
    glow += vec3(0.34, 0.12, 0.72) * exp(-dot(p - vec2( 0.15,-0.46), p - vec2( 0.15,-0.46)) * 3.6);
    col += glow * 0.60;
    float w = p.x + 0.42 * sin(p.y * 2.35 + 0.65);
    col += exp(-pow(abs(w - 0.02) * 2.6, 2.0)) * vec3(0.05, 0.20, 0.42) * 0.85;
    col += (fbm(p * 2.2 + 3.1) - 0.5) * 0.022;
    col *= 1.0 - 0.32 * dot(p, p);
    fragColor = vec4(col, 1.0);
}
)GLSL";

// ------------------------------------------------------------------- helpers
bool compileShader(GLenum type, const char* src, GLuint* out, std::string* error) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        if (error) *error = "glCreateShader failed";
        return false;
    }
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string text(static_cast<size_t>(len > 1 ? len : 1), '\0');
        glGetShaderInfoLog(shader, len, nullptr, text.data());
        if (error) *error = text;
        glDeleteShader(shader);
        return false;
    }
    *out = shader;
    return true;
}

bool linkProgram(const char* vsSrc, const char* fsSrc, GLuint* out, std::string* error) {
    GLuint vs = 0, fs = 0;
    if (!compileShader(GL_VERTEX_SHADER, vsSrc, &vs, error)) return false;
    if (!compileShader(GL_FRAGMENT_SHADER, fsSrc, &fs, error)) {
        glDeleteShader(vs);
        return false;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string text(static_cast<size_t>(len > 1 ? len : 1), '\0');
        glGetProgramInfoLog(prog, len, nullptr, text.data());
        if (error) *error = text;
        glDeleteProgram(prog);
        return false;
    }
    *out = prog;
    return true;
}

// Column major ortho with y pointing *down*, so screen pixels and GL pixels
// agree (0,0 == top left, exactly like X11).
void orthoMatrix(float* m, int w, int h) {
    const float fw = w > 0 ? float(w) : 1.0f;
    const float fh = h > 0 ? float(h) : 1.0f;
    m[0] = 2.0f / fw; m[1] = 0;        m[2] = 0;  m[3] = 0;
    m[4] = 0;         m[5] = -2.0f / fh; m[6] = 0; m[7] = 0;
    m[8] = 0;         m[9] = 0;        m[10] = -1; m[11] = 0;
    m[12] = -1;       m[13] = 1;       m[14] = 0;  m[15] = 1;
}

}  // namespace

Compositor::~Compositor() { shutdown(); }

void Compositor::shutdown() {
    if (!dpy_) return;

    if (ctx_) {
        glXMakeCurrent(dpy_, None, nullptr);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (prog_) glDeleteProgram(prog_);
        if (blurProg_) glDeleteProgram(blurProg_);
        if (wallProg_) glDeleteProgram(wallProg_);
        prog_ = blurProg_ = wallProg_ = 0;
        vao_ = vbo_ = 0;
        const auto delTex = [](GLuint& t) {
            if (t) glDeleteTextures(1, &t);
            t = 0;
        };
        const auto delFbo = [](GLuint& f) {
            if (f) glDeleteFramebuffers(1, &f);
            f = 0;
        };
        delTex(wallTex_);
        delTex(halfA_);
        delTex(halfB_);
        delFbo(wallFbo_);
        delFbo(halfFboA_);
        delFbo(halfFboB_);
        for (auto& kv : bound_) {
            if (kv.second.glxPix) glXDestroyPixmap(dpy_, kv.second.glxPix);
        }
        bound_.clear();
        fbcCache_.clear();
        glXDestroyContext(dpy_, ctx_);
        ctx_ = nullptr;
    }
    if (glxWin_) {
        glXDestroyWindow(dpy_, glxWin_);
        glxWin_ = 0;
    }
    if (overlay_) {
        XDestroyWindow(dpy_, overlay_);
        overlay_ = 0;
    }
    if (cmap_) {
        XFreeColormap(dpy_, cmap_);
        cmap_ = 0;
    }
    XSync(dpy_, False);
    ready_ = false;
    dpy_ = nullptr;
}

bool Compositor::init(Display* dpy, int screen, int width, int height, bool wantVsync,
                      std::string* error) {
    dpy_ = dpy;
    screen_ = screen;
    width_ = width > 1 ? width : 1;
    height_ = height > 1 ? height : 1;
    vsyncWanted_ = wantVsync;

    int evBase = 0, errBase = 0;
    if (!XCompositeQueryExtension(dpy_, &evBase, &errBase)) {
        if (error) *error = "the X server lacks the Composite extension";
        return false;
    }
    tfi_ = epoxy_has_glx_extension(dpy_, screen_, "GLX_EXT_texture_from_pixmap");
    if (!tfi_) log("GLX_EXT_texture_from_pixmap unavailable: window textures will not bind");

    if (!createContext(error)) return false;
    if (!buildShaders(error)) return false;
    if (!buildWallpaper(error)) return false;
    ready_ = true;
    return true;
}

GLXFBConfig Compositor::fbcForVisual(VisualID visualId, int depth) {
    for (const auto& kv : fbcCache_) {
        if (kv.first == visualId) return kv.second;
    }
    int n = 0;
    GLXFBConfig* cfgs = glXGetFBConfigs(dpy_, screen_, &n);
    GLXFBConfig found = nullptr;
    for (int i = 0; cfgs && i < n && !found; ++i) {
        int vid = 0, bufferSize = 0, bindRgb = 0, bindRgba = 0;
        if (glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_VISUAL_ID, &vid) != 0) continue;
        if (VisualID(vid) != visualId) continue;
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BUFFER_SIZE, &bufferSize);
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &bindRgb);
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &bindRgba);
        if (!bindRgb && !bindRgba) continue;
        if (depth > 0 && bufferSize != depth) continue;
        found = cfgs[i];
    }
    if (cfgs) XFree(cfgs);
    fbcCache_.emplace_back(visualId, found);
    return found;
}

bool Compositor::createContext(std::string* error) {
    Visual* rootVisual = DefaultVisual(dpy_, screen_);
    const int rootDepth = DefaultDepth(dpy_, screen_);
    const VisualID rootVid = XVisualIDFromVisual(rootVisual);

    int n = 0;
    GLXFBConfig* cfgs = glXGetFBConfigs(dpy_, screen_, &n);
    if (!cfgs || n <= 0) {
        if (error) *error = "glXGetFBConfigs returned no framebuffer configurations";
        if (cfgs) XFree(cfgs);
        return false;
    }

    // Prefer the root visual: the overlay then needs no special treatment and
    // glXMakeCurrent() on the overlay window is always legal.
    GLXFBConfig chosen = nullptr;
    int bestScore = -1;
    for (int i = 0; i < n; ++i) {
        int vid = 0, drawableType = 0, dblBuf = 0;
        if (glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_VISUAL_ID, &vid) != 0) continue;
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_DRAWABLE_TYPE, &drawableType);
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_DOUBLEBUFFER, &dblBuf);
        if (!(drawableType & GLX_WINDOW_BIT) || !dblBuf) continue;
        int bindRgb = 0, bindRgba = 0;
        if (tfi_) {
            glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &bindRgb);
            glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &bindRgba);
            if (!bindRgb && !bindRgba) continue;
        }
        int score = 0;
        if (VisualID(vid) == rootVid) score += 100;
        if (bindRgba) score += 10;
        else if (bindRgb) score += 5;
        if (score > bestScore) {
            bestScore = score;
            chosen = cfgs[i];
        }
    }
    if (!chosen) {
        if (error)
            *error = "no usable GLX framebuffer configuration (a double buffered RGBA "
                     "window config is required)";
        XFree(cfgs);
        return false;
    }
    fbc_ = chosen;

    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy_, fbc_);
    if (!vi) {
        if (error) *error = "glXGetVisualFromFBConfig failed";
        XFree(cfgs);
        return false;
    }
    visual_ = vi->visual;
    depth_ = vi->depth;
    if (depth_ != rootDepth) {
        log("note: overlay depth %d differs from the root depth %d", depth_, rootDepth);
    }
    XFree(cfgs);

    // The overlay: full screen, override redirect (no window manager -- us
    // included -- ever touches it) and lowered beneath every client, because
    // XComposite hides the clients and we paint them ourselves.
    XSetWindowAttributes swa{};
    cmap_ = XCreateColormap(dpy_, RootWindow(dpy_, screen_), visual_, AllocNone);
    swa.colormap = cmap_;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.override_redirect = True;
    swa.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | EnterWindowMask |
                     LeaveWindowMask | KeyPressMask;
    overlay_ = XCreateWindow(dpy_, RootWindow(dpy_, screen_), 0, 0,
                             static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                             0, depth_, InputOutput, visual_,
                             CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect |
                                 CWEventMask,
                             &swa);
    if (!overlay_) {
        if (error) *error = "failed to create the compositor overlay window";
        XFree(vi);
        return false;
    }
    XStoreName(dpy_, overlay_, "win11wm overlay");
    XMapRaised(dpy_, overlay_);
    XLowerWindow(dpy_, overlay_);  // bottom of the stack: clients live above us

    // Context: ask for 3.3 core (VAOs + the GLSL we target), then degrade.
    int attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                     GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
    if (epoxy_has_glx_extension(dpy_, screen_, "GLX_ARB_create_context")) {
        ctx_ = glXCreateContextAttribsARB(dpy_, fbc_, nullptr, True, attribs);
    }
    if (!ctx_) ctx_ = glXCreateNewContext(dpy_, fbc_, GLX_RGBA_TYPE, nullptr, True);
    if (!ctx_) {
        if (error) *error = "failed to create a GLX context";
        XFree(vi);
        return false;
    }

    glxWin_ = glXCreateWindow(dpy_, fbc_, overlay_, nullptr);
    const GLXDrawable draw = glxWin_ ? glxWin_ : overlay_;
    if (!glXMakeContextCurrent(dpy_, draw, draw, ctx_)) {
        if (error) *error = "glXMakeContextCurrent failed";
        XFree(vi);
        return false;
    }
    XFree(vi);

    // Vsync: interval 1 means SwapBuffers blocks until the vertical blank, so
    // the whole frame loop is paced by the display.
    swapInterval_ = 0;
    vsyncActive_ = false;
    if (vsyncWanted_) {
        if (epoxy_has_glx_extension(dpy_, screen_, "GLX_EXT_swap_control")) {
            glXSwapIntervalEXT(dpy_, draw, 1);
            swapInterval_ = 1;
        } else if (epoxy_has_glx_extension(dpy_, screen_, "GLX_MESA_swap_control")) {
            if (glXSwapIntervalMESA(1) == 0) swapInterval_ = 1;
        } else if (epoxy_has_glx_extension(dpy_, screen_, "GLX_SGI_swap_control")) {
            if (glXSwapIntervalSGI(1) == 0) swapInterval_ = 1;
        }
        if (swapInterval_ != 1) log("warning: could not enable vsync on this server");
        vsyncActive_ = swapInterval_ == 1;
    }

    const auto glStr = [](GLenum e) {
        const char* s = reinterpret_cast<const char*>(glGetString(e));
        return std::string(s ? s : "?");
    };
    vendorName_ = glStr(GL_VENDOR);
    rendererName_ = glStr(GL_RENDERER);
    glVersion_ = glStr(GL_VERSION);
    return true;
}

bool Compositor::buildShaders(std::string* error) {
    if (!linkProgram(kVertexSrc, kFragmentSrc, &prog_, error)) return false;
    if (!linkProgram(kVertexSrc, kBlurSrc, &blurProg_, error)) return false;
    if (!linkProgram(kVertexSrc, kWallpaperSrc, &wallProg_, error)) return false;

    const auto loc = [this](const char* n) { return glGetUniformLocation(prog_, n); };
    uProj_ = loc("uProj");
    uRect_ = loc("uRect");
    uMode_ = loc("uMode");
    uColor_ = loc("uColor");
    uColor2_ = loc("uColor2");
    uBorder_ = loc("uBorder");
    uShadow_ = loc("uShadow");
    uClose_ = loc("uClose");
    uFrame_ = loc("uFrame");
    uContent_ = loc("uContent");
    uRadius_ = loc("uRadius");
    uOpacity_ = loc("uOpacity");
    uCaptionH_ = loc("uCaptionH");
    uBtnW_ = loc("uBtnW");
    uHover_ = loc("uHover");
    uPress_ = loc("uPress");
    uMaximized_ = loc("uMaximized");
    uScreen_ = loc("uScreen");
    uShadowPad_ = loc("uShadowPad");
    uTexMix_ = loc("uTexMix");
    uKeepAlpha_ = loc("uKeepAlpha");
    uTintAmount_ = loc("uTintAmount");
    uTex_ = loc("uTex");
    uBlur_ = loc("uBlur");
    uBlurDir_ = glGetUniformLocation(blurProg_, "uBlurDir");
    uWallRes_ = glGetUniformLocation(wallProg_, "uWallRes");

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    orthoMatrix(proj_, width_, height_);
    return true;
}

// Bakes the procedural wallpaper into a texture and derives the half resolution
// blurred copy that every acrylic surface and every Mica caption samples.
bool Compositor::buildWallpaper(std::string* error) {
    const auto makeTexture = [this](GLuint* tex, int w, int h) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    const auto makeFbo = [](GLuint* fbo, GLuint tex, int* status) {
        glGenFramebuffers(1, fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        *status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    };

    makeTexture(&wallTex_, width_, height_);
    int status = 0;
    makeFbo(&wallFbo_, wallTex_, &status);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) *error = "failed to create the wallpaper framebuffer";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBindVertexArray(vao_);
    glDisable(GL_BLEND);
    glViewport(0, 0, width_, height_);
    glUseProgram(wallProg_);
    glUniformMatrix4fv(glGetUniformLocation(wallProg_, "uProj"), 1, GL_FALSE, proj_);
    glUniform4f(glGetUniformLocation(wallProg_, "uRect"), 0.f, 0.f, float(width_), float(height_));
    glUniform2f(uWallRes_, float(width_), float(height_));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    halfW_ = width_ / 2 > 1 ? width_ / 2 : 1;
    halfH_ = height_ / 2 > 1 ? height_ / 2 : 1;
    makeTexture(&halfA_, halfW_, halfH_);
    makeTexture(&halfB_, halfW_, halfH_);
    makeFbo(&halfFboA_, halfA_, &status);
    makeFbo(&halfFboB_, halfB_, &status);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error) *error = "failed to create the blur framebuffers";
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    float halfProj[16];
    orthoMatrix(halfProj, halfW_, halfH_);

    // Downsample the wallpaper into halfA_ with the main shader in texture mode.
    glUseProgram(prog_);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, halfProj);
    glUniform4f(uRect_, 0.f, 0.f, float(halfW_), float(halfH_));
    glUniform1f(uRadius_, 0.f);
    glUniform1f(uOpacity_, 1.f);
    glUniform1f(uTexMix_, 1.f);
    glUniform1f(uKeepAlpha_, 0.f);
    glUniform4f(uColor_, 1.f, 1.f, 1.f, 1.f);
    glUniform1i(uTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, halfFboA_);
    glViewport(0, 0, halfW_, halfH_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wallTex_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Three separable H+V pairs at growing steps. One 9-tap pass only covers a
    // few pixels; real acrylic needs a backdrop blurred over ~30px, which is
    // what the 1/2/4 texel steps add up to without banding.
    glUseProgram(blurProg_);
    glUniformMatrix4fv(glGetUniformLocation(blurProg_, "uProj"), 1, GL_FALSE, halfProj);
    glUniform4f(glGetUniformLocation(blurProg_, "uRect"), 0.f, 0.f, float(halfW_), float(halfH_));
    glUniform1i(glGetUniformLocation(blurProg_, "uTex"), 0);
    const float steps[3] = {1.0f, 2.0f, 4.0f};
    for (int pass = 0; pass < 3; ++pass) {
        glBindFramebuffer(GL_FRAMEBUFFER, halfFboB_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, halfA_);
        glUniform2f(uBlurDir_, steps[pass] * 2.0f / float(halfW_), 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glBindFramebuffer(GL_FRAMEBUFFER, halfFboA_);
        glBindTexture(GL_TEXTURE_2D, halfB_);
        glUniform2f(uBlurDir_, 0.0f, steps[pass] * 2.0f / float(halfH_));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glUseProgram(prog_);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, proj_);
    glEnable(GL_BLEND);
    return true;
}

// ------------------------------------------------------------------ textures
bool Compositor::tfiBind(GLuint tex) {
    for (const auto& kv : bound_) {
        if (kv.first == tex) {
            glXBindTexImageEXT(dpy_, kv.second.glxPix, GLX_FRONT_EXT, nullptr);
            return true;
        }
    }
    return false;
}

void Compositor::tfiRelease(GLuint tex) {
    for (const auto& kv : bound_) {
        if (kv.first == tex) {
            glXReleaseTexImageEXT(dpy_, kv.second.glxPix, GLX_FRONT_EXT);
            return;
        }
    }
}

GLuint Compositor::bindPixmap(Pixmap pixmap, VisualID visualId, int depth, int w, int h,
                              bool alpha) {
    if (!tfi_ || !pixmap || w <= 0 || h <= 0) return 0;
    GLXFBConfig fbc = fbcForVisual(visualId, depth);
    if (!fbc) return 0;

    int bindRgb = 0, bindRgba = 0;
    glXGetFBConfigAttrib(dpy_, fbc, GLX_BIND_TO_TEXTURE_RGB_EXT, &bindRgb);
    glXGetFBConfigAttrib(dpy_, fbc, GLX_BIND_TO_TEXTURE_RGBA_EXT, &bindRgba);
    int format = alpha ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT;
    if (alpha && !bindRgba && bindRgb) format = GLX_TEXTURE_FORMAT_RGB_EXT;
    if (!alpha && !bindRgb && bindRgba) format = GLX_TEXTURE_FORMAT_RGBA_EXT;
    if ((format == GLX_TEXTURE_FORMAT_RGBA_EXT && !bindRgba) ||
        (format == GLX_TEXTURE_FORMAT_RGB_EXT && !bindRgb)) {
        return 0;
    }

    const int attrs[] = {GLX_TEXTURE_FORMAT_EXT, format,
                         GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT, 0};
    GLXPixmap glxPix = glXCreatePixmap(dpy_, fbc, pixmap, attrs);
    if (!glxPix) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        glXDestroyPixmap(dpy_, glxPix);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // One bind/release pair establishes the texture's storage and size.
    glXBindTexImageEXT(dpy_, glxPix, GLX_FRONT_EXT, nullptr);
    glXReleaseTexImageEXT(dpy_, glxPix, GLX_FRONT_EXT);
    glBindTexture(GL_TEXTURE_2D, 0);

    bound_.push_back({tex, Bound{glxPix, w, h}});
    return tex;
}

void Compositor::releasePixmap(GLuint tex) {
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, 0);
    for (size_t i = 0; i < bound_.size(); ++i) {
        if (bound_[i].first != tex) continue;
        if (bound_[i].second.glxPix) glXDestroyPixmap(dpy_, bound_[i].second.glxPix);
        bound_.erase(bound_.begin() + static_cast<ptrdiff_t>(i));
        break;
    }
    glDeleteTextures(1, &tex);
}

void Compositor::uploadTexture(GLuint* tex, const unsigned char* rgba, int w, int h) {
    if (!tex || !rgba || w <= 0 || h <= 0) return;
    if (!*tex) glGenTextures(1, tex);
    if (!*tex) return;
    glBindTexture(GL_TEXTURE_2D, *tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Compositor::destroyTexture(GLuint tex) {
    if (!tex) return;
    for (size_t i = 0; i < bound_.size(); ++i) {
        if (bound_[i].first == tex) {
            releasePixmap(tex);
            return;
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
}

// ------------------------------------------------------------------- drawing
void Compositor::drawQuad(GLuint tex, int mode) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(uMode_, mode);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::beginFrame() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    // Everything we emit is premultiplied, which is what makes stacked
    // translucent surfaces (acrylic + Mica + shadows) blend correctly.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog_);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, proj_);
    glUniform2f(uScreen_, float(width_), float(height_));
    glUniform1f(uShadowPad_, float(metrics::kShadowPad));
    glUniform1f(uBtnW_, float(metrics::kBtnW));
    glUniform1i(uTex_, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, halfA_);  // the blurred wallpaper
    glUniform1i(uBlur_, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
}

void Compositor::drawWallpaper() {
    drawTex(wallTex_, Rect{0, 0, width_, height_}, 0.f, Color{1.f, 1.f, 1.f, 1.f}, 1.f, true,
            false);
}

void Compositor::drawRect(const Rect& r, float radius, const Color& c, float opacity) {
    if (r.empty()) return;
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glUniform1i(uMode_, 0);
    glUniform4f(uRect_, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform4f(uColor_, c.r, c.g, c.b, c.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawAcrylic(const Rect& r, float radius, const Color& tint, float tintAmount,
                             const Color& border, float opacity) {
    if (r.empty()) return;
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glUniform1i(uMode_, 1);
    glUniform4f(uRect_, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform1f(uTintAmount_, tintAmount);
    glUniform4f(uColor_, tint.r, tint.g, tint.b, tint.a);
    glUniform4f(uBorder_, border.r, border.g, border.b, border.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawTex(GLuint tex, const Rect& dst, float radius, const Color& tint,
                         float opacity, bool textureColor, bool textureAlpha) {
    if (!tex || dst.empty()) return;
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glUniform4f(uRect_, float(dst.x), float(dst.y), float(dst.w), float(dst.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform4f(uColor_, tint.r, tint.g, tint.b, tint.a);
    glUniform1f(uTexMix_, textureColor ? 1.f : 0.f);
    glUniform1f(uKeepAlpha_, textureAlpha ? 1.f : 0.f);
    // A texture_from_pixmap texture must be bound around the sampling.
    const bool tfi = tfiBind(tex);
    glUniform1i(uMode_, 3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (tfi) tfiRelease(tex);
}

void Compositor::drawText(const TextTex& t, const Rect& dst, const Color& c, float opacity) {
    if (!t.tex) return;
    drawTex(t.tex, dst, 0.f, c, opacity, false, true);
}

// One draw call per window: shadow, Mica caption, caption buttons with their
// Windows 11 glyphs, the hairline border and the client's own pixels, all
// resolved analytically from a rounded box distance in the fragment shader.
void Compositor::drawWindow(const WindowSprite& s) {
    if (s.frame.empty()) return;
    const Rect quad = s.frame.inflated(metrics::kShadowPad);
    const int capH = s.captionH > 0 ? s.captionH : 0;
    // The client's rect inside the frame: caption on top, 1px border elsewhere.
    const Rect content{s.frame.x + metrics::kBorder, s.frame.y + capH,
                       s.frame.w - 2 * metrics::kBorder, s.frame.h - capH - metrics::kBorder};

    const Color caption = s.focused ? theme::kCaption : theme::kCaptionIdle;
    const Color glyph = s.focused ? theme::kGlyph : theme::kGlyphIdle;
    Color border = s.focused ? theme::kBorderFocus : theme::kBorderIdle;
    const Color shadow = s.focused ? theme::kShadow : theme::kShadowIdle;
    if (s.attention > 0.f) {
        const auto mixc = [&](float a, float b) {
            return static_cast<float>(lerp(double(a), double(b), double(s.attention)));
        };
        border = Color{mixc(border.r, theme::kAccentDeep.r), mixc(border.g, theme::kAccentDeep.g),
                       mixc(border.b, theme::kAccentDeep.b), 1.f};
    }

    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glUniform4f(uRect_, float(quad.x), float(quad.y), float(quad.w), float(quad.h));
    glUniform4f(uFrame_, float(s.frame.x), float(s.frame.y), float(s.frame.w),
                float(s.frame.h));
    glUniform4f(uContent_, float(content.x), float(content.y), float(content.w),
                float(content.h));
    glUniform1f(uCaptionH_, float(capH));
    glUniform1f(uRadius_, s.radius);
    glUniform1f(uOpacity_, s.opacity);
    glUniform1f(uMaximized_, s.maximized ? 1.f : 0.f);
    glUniform1f(uTintAmount_, 0.88f);
    glUniform3f(uHover_, s.minHover ? 1.f : 0.f, s.maxHover ? 1.f : 0.f,
                s.closeHover ? 1.f : 0.f);
    glUniform3f(uPress_, s.minPress ? 1.f : 0.f, s.maxPress ? 1.f : 0.f,
                s.closePress ? 1.f : 0.f);
    glUniform4f(uColor_, caption.r, caption.g, caption.b, caption.a);
    glUniform4f(uColor2_, glyph.r, glyph.g, glyph.b, glyph.a);
    glUniform4f(uBorder_, border.r, border.g, border.b, border.a);
    glUniform4f(uShadow_, shadow.r, shadow.g, shadow.b, shadow.a * s.opacity);
    glUniform4f(uClose_, theme::kCloseHover.r, theme::kCloseHover.g, theme::kCloseHover.b, 1.f);

    const bool hasTex = s.tex.valid();
    glUniform1f(uTexMix_, hasTex ? 1.f : 0.f);
    glUniform1f(uKeepAlpha_, (hasTex && s.tex.alpha) ? 1.f : 0.f);
    const bool tfi = hasTex && tfiBind(s.tex.tex);
    glUniform1i(uMode_, 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hasTex ? s.tex.tex : 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (tfi) tfiRelease(s.tex.tex);
}

void Compositor::present() {
    glBindVertexArray(0);
    glUseProgram(0);
    const double before = nowMs();
    glXSwapBuffers(dpy_, glxWin_ ? glxWin_ : overlay_);
    const double after = nowMs();
    lastFrameSec_ = lastPresentMs_ > 0.0 ? (after - lastPresentMs_) / 1000.0 : 0.0;
    lastPresentMs_ = after;
    if (fpsWindowStart_ <= 0.0) fpsWindowStart_ = before;
    ++fpsFrames_;
    const double span = after - fpsWindowStart_;
    if (span >= 500.0) {
        fps_ = int(double(fpsFrames_) * 1000.0 / span + 0.5);
        fpsFrames_ = 0;
        fpsWindowStart_ = after;
    }
}


}  // namespace wm
