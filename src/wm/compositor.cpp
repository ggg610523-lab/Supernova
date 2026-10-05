#include "compositor.h"

#include "png.h"

#include <X11/extensions/Xcomposite.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace wm {
namespace {

// ---------------------------------------------------------------- unit quad
// Every primitive is drawn as this one quad, placed by uRect in screen pixels.
constexpr float kQuad[8] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f};

// Magic-lamp mesh resolution: rows carry the vertical funnel, columns the
// horizontal convergence. A handful of either is plenty -- the silhouette comes
// from the per-row warp, not from the tessellation.
constexpr int kGenieCols = 8;
constexpr int kGenieRows = 48;

const char* const kVertexSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat4 uProj;
uniform vec4 uRect;      // x, y, w, h in screen pixels
uniform vec2 uPivot;     // rotation pivot in screen pixels
uniform vec2 uRot;       // (cos, sin) of the rotation; (1, 0) = none
out vec2 vQ;             // 0..1 across the quad
void main() {
    vQ = aPos;
    vec2 p = uRect.xy + aPos * uRect.zw;
    vec2 d = p - uPivot;
    vec2 r = vec2(d.x * uRot.x - d.y * uRot.y, d.x * uRot.y + d.y * uRot.x);
    gl_Position = uProj * vec4(uPivot + r, 0.0, 1.0);
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
uniform float uSaturate;  // backdrop-filter saturate() amount (mode 1)
uniform float uClipTop;   // mode 0: discard fragments above this screen y
uniform vec2  uArc;       // annular arc: start/end angle (radians, 0 = 12 o'clock)
uniform float uThick;     // annular arc stroke width
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
        // A vertical clip lets a caller draw only the part of a rounded shape
        // below some y, which is how a slider fill follows the pill's corners
        // without overlapping two translucent rects.
        if (p.y < uClipTop) {
            fragColor = vec4(0.0);
            return;
        }
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        float a = cover(d) * uOpacity * uColor.a;
        fragColor = vec4(uColor.rgb * a, a);
        return;
    }

    if (uMode == 1) {                    // acrylic surface
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        vec2 uv = clamp(p / uScreen, vec2(0.0), vec2(1.0));
        vec3 col = texture(uBlur, uv).rgb;
        // Acrylic is a saturated wash: push the blurred backdrop away from grey so
        // its colour survives, then take the tint over it. uSaturate is the
        // backdrop-filter saturate() amount -- the taskbar runs it high (3), the
        // default surfaces keep a gentler 1.45.
        float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(l), col, uSaturate);
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

    if (uMode == 4) {                    // vertical gradient rounded fill
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        vec4 col = mix(uColor, uColor2, clamp(vQ.y, 0.0, 1.0));
        float a = cover(d) * uOpacity * col.a;
        fragColor = vec4(col.rgb * a, a);
        return;
    }

    if (uMode == 5) {                    // annular arc / ring
        vec2 v = p - c;
        float rr = length(v);
        float band = abs(rr - uRadius) - uThick * 0.5;  // 0 on the ring's centre line
        float ang = atan(v.x, -v.y);                    // 0 at 12 o'clock, clockwise
        if (ang < 0.0) ang += 6.28318530718;
        float span = uArc.y - uArc.x;
        if (span < 0.0) span += 6.28318530718;
        float d = band;
        if (span < 6.28318530718 - 0.0001) {            // partial: antialias the two ends
            float da = ang - uArc.x;
            da -= 6.28318530718 * floor(da / 6.28318530718);  // wrap into [0, TAU)
            float over = da - span;                     // > 0 means outside the wedge
            float edge = over > 0.0 ? over : -min(da, span - da);
            d = max(band, edge * max(rr, 1.0));
        }
        float a = cover(d) * uOpacity * uColor.a;
        fragColor = vec4(uColor.rgb * a, a);
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

// The magic-lamp minimise shaders: the window content is drawn over a grid mesh
// whose vertices are warped into a genie funnel that pours into the taskbar
// icon. It is a forward warp (each vertex carries its own source uv), so no
// coordinate inversion is needed and the sides stay smooth at every value.
const char* const kGenieVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aGrid;   // 0..1 across the window content
uniform mat4 uProj;
uniform vec4 uRect;      // content rect: x, y, w, h (screen px)
uniform vec4 uIcon;      // taskbar icon rect: x, y, w, h (screen px)
uniform float uProgress; // 0 = untouched, 1 = fully poured into the icon
out vec2 vUV;
void main() {
    float t = clamp(uProgress, 0.0, 1.0);
    vec2 src = uRect.xy + aGrid * uRect.zw;
    // Per-row progress: rows nearer the icon pour in first (the "+ aGrid.y"
    // term), so at t = 1 every row has reached the icon and the window has
    // become the icon itself.
    float s = clamp(t * (1.0 + aGrid.y), 0.0, 1.0);
    s = s * s * (3.0 - 2.0 * s);               // smoothstep: a softer waist
    vec2 iconCentre = uIcon.xy + uIcon.zw * 0.5;
    float dstX = iconCentre.x + (aGrid.x - 0.5) * uIcon.z;  // converge onto icon
    float dstY = uIcon.y + aGrid.y * uIcon.w;               // slide onto the icon
    vec2 pos = vec2(mix(src.x, dstX, s), mix(src.y, dstY, s));
    vUV = aGrid;
    gl_Position = uProj * vec4(pos, 0.0, 1.0);
}
)GLSL";

const char* const kGenieFragSrc = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uOpacity;
uniform float uKeepAlpha;
void main() {
    vec4 t = texture(uTex, vUV);
    float a = uOpacity * mix(1.0, t.a, uKeepAlpha);
    fragColor = vec4(t.rgb * a, a);
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

// Centre-crops a decoded image to `outW:outH`'s aspect ratio, so a 16:9 photo
// fills a 16:10 screen the way Windows' "Fill" does rather than stretching. The
// cropped texture is then drawn straight onto the screen, which also keeps the
// blurred acrylic/Mica source in the same geometry as what is displayed.
void coverCrop(std::vector<unsigned char>* rgba, int* w, int* h, int outW, int outH) {
    if (!rgba || !w || !h || *w <= 0 || *h <= 0 || outW <= 0 || outH <= 0) return;
    const double src = double(*w) / double(*h);
    const double dst = double(outW) / double(outH);
    if (std::fabs(src - dst) < 1e-3) return;

    int cw = *w, ch = *h, cx = 0, cy = 0;
    if (src > dst) {  // too wide: keep full height, trim the sides
        cw = int(std::lround(double(*h) * dst));
        if (cw < 1) cw = 1;
        if (cw > *w) cw = *w;
        cx = (*w - cw) / 2;
    } else {          // too tall: keep full width, trim top and bottom
        ch = int(std::lround(double(*w) / dst));
        if (ch < 1) ch = 1;
        if (ch > *h) ch = *h;
        cy = (*h - ch) / 2;
    }

    std::vector<unsigned char> out(size_t(cw) * size_t(ch) * 4u);
    for (int y = 0; y < ch; ++y) {
        const unsigned char* s = rgba->data() + (size_t(y + cy) * size_t(*w) + size_t(cx)) * 4u;
        std::memcpy(out.data() + size_t(y) * size_t(cw) * 4u, s, size_t(cw) * 4u);
    }
    *rgba = std::move(out);
    *w = cw;
    *h = ch;
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
        // Delete every GL object *before* releasing the context. Doing it the
        // other way round -- glXMakeCurrent(None) first -- leaves the deletes
        // with no current context, which trips libepoxy's dispatch assertion
        // and aborts the process during shutdown.
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (prog_) glDeleteProgram(prog_);
        if (blurProg_) glDeleteProgram(blurProg_);
        if (wallProg_) glDeleteProgram(wallProg_);
        if (genieProg_) glDeleteProgram(genieProg_);
        prog_ = blurProg_ = wallProg_ = genieProg_ = 0;
        if (meshVao_) glDeleteVertexArrays(1, &meshVao_);
        if (meshVbo_) glDeleteBuffers(1, &meshVbo_);
        if (meshIbo_) glDeleteBuffers(1, &meshIbo_);
        vao_ = vbo_ = meshVao_ = meshVbo_ = meshIbo_ = 0;
        if (timerQueryPending_) {
            glEndQuery(GL_TIME_ELAPSED);
            glDeleteQueries(1, &timerQueryPending_);
            timerQueryPending_ = 0;
        }
        if (timerQuery_) {
            glDeleteQueries(1, &timerQuery_);
            timerQuery_ = 0;
        }
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
        // Context last: everything above needs it to still be current.
        glXMakeCurrent(dpy_, None, nullptr);
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
                      const std::string& wallpaperPath, std::string* error) {
    dpy_ = dpy;
    screen_ = screen;
    width_ = width > 1 ? width : 1;
    height_ = height > 1 ? height : 1;
    vsyncWanted_ = wantVsync;
    wallpaperPath_ = wallpaperPath;

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
    GLXFBConfig exact = nullptr, loose = nullptr;
    for (int i = 0; cfgs && i < n; ++i) {
        int vid = 0, bufferSize = 0, bindRgb = 0, bindRgba = 0;
        if (glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_VISUAL_ID, &vid) != 0) continue;
        if (VisualID(vid) != visualId) continue;
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &bindRgb);
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &bindRgba);
        if (!bindRgb && !bindRgba) continue;
        // The visual id already pins the visual's depth, so the buffer size is
        // only a preference. It has to be: Mesa exposes the depth 24 root
        // visual (0x4d) exclusively through *32 bit* buffer configs -- the
        // alpha channel rides in the extra byte -- so an exact depth match
        // finds nothing and no window could ever be bound as a texture.
        glXGetFBConfigAttrib(dpy_, cfgs[i], GLX_BUFFER_SIZE, &bufferSize);
        if (!loose) loose = cfgs[i];
        if (bufferSize == depth) {
            exact = cfgs[i];
            break;
        }
    }
    if (cfgs) XFree(cfgs);
    GLXFBConfig found = exact ? exact : loose;
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

    // GL 3.3 has no timer query (it arrived in 4.3 / ARB_timer_query), so this is
    // strictly optional: profiling falls back to CPU-side timing when absent.
    glGenQueries(1, &timerQuery_);
    return true;
}

bool Compositor::buildShaders(std::string* error) {
    if (!linkProgram(kVertexSrc, kFragmentSrc, &prog_, error)) return false;
    if (!linkProgram(kVertexSrc, kBlurSrc, &blurProg_, error)) return false;
    if (!linkProgram(kVertexSrc, kWallpaperSrc, &wallProg_, error)) return false;
    if (!linkProgram(kGenieVertSrc, kGenieFragSrc, &genieProg_, error)) return false;

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
    uSaturate_ = loc("uSaturate");
    uClipTop_ = loc("uClipTop");
    uArc_ = loc("uArc");
    uThick_ = loc("uThick");
    uTex_ = loc("uTex");
    uBlur_ = loc("uBlur");
    uPivot_ = loc("uPivot");
    uRot_ = loc("uRot");
    uBlurDir_ = glGetUniformLocation(blurProg_, "uBlurDir");
    uWallRes_ = glGetUniformLocation(wallProg_, "uWallRes");

    // Every program shares the vertex shader, so each needs the rotation pivot
    // and angle initialised to the identity or its quads collapse to a point.
    glUseProgram(prog_);
    glUniform2f(uPivot_, 0.f, 0.f);
    glUniform2f(uRot_, 1.f, 0.f);
    glUseProgram(blurProg_);
    glUniform2f(glGetUniformLocation(blurProg_, "uPivot"), 0.f, 0.f);
    glUniform2f(glGetUniformLocation(blurProg_, "uRot"), 1.f, 0.f);
    glUseProgram(wallProg_);
    glUniform2f(glGetUniformLocation(wallProg_, "uPivot"), 0.f, 0.f);
    glUniform2f(glGetUniformLocation(wallProg_, "uRot"), 1.f, 0.f);
    glUseProgram(0);

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
    if (!buildGenieMesh()) {
        if (error) *error = "failed to build the magic-lamp mesh";
        return false;
    }
    return true;
}

// The reusable grid the magic-lamp effect draws a window over. Built once; only
// the vertices' *position* changes per frame (in the vertex shader), so this is
// just the unit square, finely subdivided.
bool Compositor::buildGenieMesh() {
    if (!genieProg_) return false;
    gProj_ = glGetUniformLocation(genieProg_, "uProj");
    gRect_ = glGetUniformLocation(genieProg_, "uRect");
    gIcon_ = glGetUniformLocation(genieProg_, "uIcon");
    gProgress_ = glGetUniformLocation(genieProg_, "uProgress");
    gOpacity_ = glGetUniformLocation(genieProg_, "uOpacity");
    gKeepAlpha_ = glGetUniformLocation(genieProg_, "uKeepAlpha");
    gTex_ = glGetUniformLocation(genieProg_, "uTex");

    std::vector<float> verts;
    verts.reserve(size_t(kGenieCols + 1) * size_t(kGenieRows + 1) * 2u);
    for (int r = 0; r <= kGenieRows; ++r)
        for (int c = 0; c <= kGenieCols; ++c) {
            verts.push_back(float(c) / float(kGenieCols));
            verts.push_back(float(r) / float(kGenieRows));
        }
    std::vector<unsigned int> idx;
    idx.reserve(size_t(kGenieCols) * size_t(kGenieRows) * 6u);
    for (int r = 0; r < kGenieRows; ++r)
        for (int c = 0; c < kGenieCols; ++c) {
            const unsigned int v0 = unsigned(r * (kGenieCols + 1) + c);
            const unsigned int v1 = v0 + 1;
            const unsigned int v2 = v0 + unsigned(kGenieCols + 1);
            const unsigned int v3 = v2 + 1;
            idx.push_back(v0); idx.push_back(v1); idx.push_back(v2);
            idx.push_back(v1); idx.push_back(v3); idx.push_back(v2);
        }
    meshIndexCount_ = int(idx.size());

    glGenVertexArrays(1, &meshVao_);
    glBindVertexArray(meshVao_);
    glGenBuffers(1, &meshVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glGenBuffers(1, &meshIbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(unsigned int)),
                 idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

// Bakes the wallpaper into a texture and derives the half resolution blurred
// copy that every acrylic surface and every Mica caption samples. The photo at
// `wallpaperPath_` (assets/wallpaper/wallpaper.png, transcoded from the bundled
// JPEG by scripts/fetch-assets.sh) wins when it decodes; the procedural shader
// is the fallback, so a checkout without the asset still gets a background.
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

    int status = 0;
    std::vector<unsigned char> rgba;
    int imgW = 0, imgH = 0;
    const bool photo = !wallpaperPath_.empty() &&
                       loadPng(wallpaperPath_, &rgba, &imgW, &imgH) && imgW > 0 && imgH > 0;
    if (photo) {
        // Fill the screen without distortion, then hand the pixels to the GPU.
        coverCrop(&rgba, &imgW, &imgH, width_, height_);
        uploadTexture(&wallTex_, rgba.data(), imgW, imgH);
        if (!wallTex_) {
            if (error) *error = "failed to upload the wallpaper texture";
            return false;
        }
        log("wallpaper: %s (%dx%d)", wallpaperPath_.c_str(), imgW, imgH);
    } else {
        if (!wallpaperPath_.empty())
            log("wallpaper: %s unreadable, using the procedural backdrop",
                wallpaperPath_.c_str());
        makeTexture(&wallTex_, width_, height_);
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
        glUniform4f(glGetUniformLocation(wallProg_, "uRect"), 0.f, 0.f, float(width_),
                    float(height_));
        glUniform2f(uWallRes_, float(width_), float(height_));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

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
    // uMode must be set explicitly: it defaults to -1, and a mode matching no
    // branch writes no fragColor at all, leaving the acrylic source undefined --
    // in practice black, which is why every acrylic surface read as flat dark.
    // The VAO has to be bound too: in a core profile a draw with no VAO is a
    // GL_INVALID_OPERATION and writes nothing, and the photo path (unlike the
    // procedural fallback below) never bound one, so the whole blur came out
    // black whenever the wallpaper image loaded -- which is the normal case.
    glBindVertexArray(vao_);
    glUseProgram(prog_);
    glUniform1i(uMode_, 3);
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
            // glXBindTexImageEXT replaces the contents of *the texture that is
            // currently bound on the active unit*, not of `tex`. Binding first
            // is therefore mandatory: otherwise the client's pixels land in
            // whatever was bound before -- in practice the wallpaper, which
            // was why opening a window painted the app over the backdrop.
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
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

void Compositor::useMainProgram() {
    // The main program and the quad VAO are bound for the whole frame by
    // beginFrame(); re-binding them per draw call is pure driver overhead.
    if (mainProgramBound_) return;
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    mainProgramBound_ = true;
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
    // Same reasoning as tfiBind(): the pixmap attaches to the texture bound on
    // the active unit, so pin both before the bind/release pair that gives the
    // texture its storage.
    glActiveTexture(GL_TEXTURE0);
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
void Compositor::beginFrame() {
    cpuFrameStart_ = nowMs();
    drawsThisFrame_ = 0;

    // Collect the query issued last frame, if the driver has already finished
    // it. Reading a pending query would stall the pipeline, so it is left
    // in flight and picked up on a later frame instead.
    if (timerQueryPending_) {
        GLuint done = 0;
        glGetQueryObjectuiv(timerQueryPending_, GL_QUERY_RESULT_AVAILABLE, &done);
        if (done) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(timerQueryPending_, GL_QUERY_RESULT, &ns);
            gpuFrameMs_ = double(ns) / 1e6;
            glDeleteQueries(1, &timerQueryPending_);
            timerQueryPending_ = 0;
        }
    }

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
    glUniform2f(uPivot_, 0.f, 0.f);
    glUniform2f(uRot_, 1.f, 0.f);
    glBindVertexArray(vao_);
    mainProgramBound_ = true;

    if (timerQuery_ && !timerQueryPending_) {
        glBeginQuery(GL_TIME_ELAPSED, timerQuery_);
        timerQueryPending_ = timerQuery_;
    }
}

void Compositor::drawWallpaper() {
    drawTex(wallTex_, Rect{0, 0, width_, height_}, 0.f, Color{1.f, 1.f, 1.f, 1.f}, 1.f, true,
            false);
}

void Compositor::drawRect(const Rect& r, float radius, const Color& c, float opacity,
                          int clipTop) {
    if (r.empty()) return;
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[0];
    glUniform1i(uMode_, 0);
    glUniform4f(uRect_, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform1f(uClipTop_, clipTop == kNoClip ? -1.0e9f : float(clipTop));
    glUniform4f(uColor_, c.r, c.g, c.b, c.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawRectRotated(int cx, int cy, int w, int h, float angle, float radius,
                                 const Color& c, float opacity) {
    if (w <= 0 || h <= 0) return;
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[0];
    glUniform1i(uMode_, 0);
    glUniform4f(uRect_, float(cx) - w * 0.5f, float(cy) - h * 0.5f, float(w), float(h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform1f(uClipTop_, -1.0e9f);
    glUniform4f(uColor_, c.r, c.g, c.b, c.a);
    glUniform2f(uPivot_, float(cx), float(cy));
    glUniform2f(uRot_, std::cos(angle), std::sin(angle));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUniform2f(uRot_, 1.f, 0.f);  // leave the shared state axis-aligned
}

void Compositor::drawGradient(const Rect& r, float radius, const Color& top, const Color& bottom,
                              float opacity) {
    if (r.empty()) return;
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[4];
    glUniform1i(uMode_, 4);
    glUniform4f(uRect_, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform4f(uColor_, top.r, top.g, top.b, top.a);
    glUniform4f(uColor2_, bottom.r, bottom.g, bottom.b, bottom.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawArc(int cx, int cy, float radius, float thick, float a0, float a1,
                         const Color& c, float opacity) {
    if (thick <= 0.f || radius <= 0.f) return;
    const float reach = radius + thick * 0.5f + 1.f;
    const int side = int(std::ceil(reach * 2.f));
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[5];
    glUniform1i(uMode_, 5);
    glUniform4f(uRect_, float(cx) - side * 0.5f, float(cy) - side * 0.5f, float(side),
                float(side));
    glUniform1f(uRadius_, radius);
    glUniform1f(uThick_, thick);
    glUniform1f(uOpacity_, opacity);
    glUniform4f(uColor_, c.r, c.g, c.b, c.a);
    glUniform2f(uArc_, a0, a1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawAcrylic(const Rect& r, float radius, const Color& tint, float tintAmount,
                             const Color& border, float opacity, float saturation) {
    if (r.empty()) return;
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[1];
    glUniform1i(uMode_, 1);
    glUniform4f(uRect_, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform1f(uTintAmount_, tintAmount);
    glUniform1f(uSaturate_, saturation);
    glUniform4f(uColor_, tint.r, tint.g, tint.b, tint.a);
    glUniform4f(uBorder_, border.r, border.g, border.b, border.a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawTex(GLuint tex, const Rect& dst, float radius, const Color& tint,
                         float opacity, bool textureColor, bool textureAlpha) {
    if (!tex || dst.empty()) return;
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[3];
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
    // While a window is minimising or restoring, pour its content into (or out
    // of) the taskbar icon with the magic-lamp mesh instead of an ordinary frame.
    if (s.genie > 0.001f && s.tex.valid() && drawGenie(s)) return;
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

    ++drawsThisFrame_;
    ++drawsByMode_[2];
    useMainProgram();
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
    glUniform3f(uHover_, s.minHover, s.maxHover, s.closeHover);
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

// Magic lamp: forward-warp the window's content into (or out of) its taskbar
// icon. Only the client texture is warped; the Fluent frame chrome is drawn by
// the main shader and is intentionally absent for the duration of the effect.
// Returns false when the content texture cannot be bound, so the caller can fall
// back to the ordinary sprite (e.g. a window with no XComposite texture yet).
bool Compositor::drawGenie(const WindowSprite& s) {
    if (!genieProg_ || !meshVao_ || meshIndexCount_ <= 0) return false;
    const int capH = s.captionH > 0 ? s.captionH : 0;
    const Rect content{s.frame.x + metrics::kBorder, s.frame.y + capH,
                       s.frame.w - 2 * metrics::kBorder, s.frame.h - capH - metrics::kBorder};
    if (content.w < 2 || content.h < 2) return false;
    if (!tfiBind(s.tex.tex)) return false;  // binds the texture on unit 0

    // This effect borrows the genie program and the subdivided mesh VAO, so the
    // main-program cache is no longer valid. Without this the next primitive
    // that reaches for the cache believes prog_/vao_ are still current, skips
    // the rebind, and rasterises with the genie shader and no VAO -- which is
    // why the taskbar vanished for the frame after a minimize.
    mainProgramBound_ = false;
    glUseProgram(genieProg_);
    glBindVertexArray(meshVao_);
    glUniformMatrix4fv(gProj_, 1, GL_FALSE, proj_);
    glUniform4f(gRect_, float(content.x), float(content.y), float(content.w), float(content.h));
    glUniform4f(gIcon_, float(s.genieIcon.x), float(s.genieIcon.y), float(s.genieIcon.w),
                float(s.genieIcon.h));
    glUniform1f(gProgress_, s.genie);
    glUniform1f(gOpacity_, s.opacity);
    glUniform1f(gKeepAlpha_, s.tex.alpha ? 1.f : 0.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.tex.tex);
    glUniform1i(gTex_, 0);
    glDrawElements(GL_TRIANGLES, meshIndexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    tfiRelease(s.tex.tex);
    return true;
}

void Compositor::present() {
    if (timerQueryPending_) glEndQuery(GL_TIME_ELAPSED);
    cpuFrameMs_ = nowMs() - cpuFrameStart_;
    drawCalls_ += drawsThisFrame_;

    glBindVertexArray(0);
    mainProgramBound_ = false;
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
        // A machine-readable line once per sampling window, so a headless
        // benchmark can read the same numbers the HUD paints.
        if (verbose_) {
            log("PERF fps=%d cpu=%.2f gpu=%.2f draws=%ld drawsAvg=%.1f", fps_, cpuFrameMs_,
                gpuFrameMs_, drawsThisFrame_,
                drawsFrames_ ? double(drawsWindow_) / double(drawsFrames_) : 0.0);
            drawsWindow_ = 0;
            drawsFrames_ = 0;
        }
        drawsWindow_ += drawsThisFrame_;
        ++drawsFrames_;
    }
}


}  // namespace wm
