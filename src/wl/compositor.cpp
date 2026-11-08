// The Wayland/GLES2 implementation of the Fluent renderer.
//
// The shaders below are the X11 build's, ported to GLSL ES 1.00: wlroots creates
// its EGL context as OpenGL ES 2, so attributes/varyings and texture2D() replace
// the desktop GL 3.3 in/out and texture() spellings. The maths, and therefore
// the Windows 11 look, is unchanged.
#include "compositor.h"

#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <unistd.h>

#include "png.h"
#include "theme.h"

namespace wm {
namespace {

constexpr int kGenieCols = 8;
constexpr int kGenieRows = 48;

// Interleaved batch vertex: pos(2), rect(4), color(4), color2(4), border(4),
// shape params p0/p1/p2 (4 each). Six vertices per quad (two triangles).
constexpr int kBatchFloatsPerVertex = 30;
constexpr int kBatchVertsPerQuad = 6;

const char* const kVertexSrc = R"GLSL(
#version 100
attribute vec2 aPos;
uniform mat4 uProj;
uniform vec4 uRect;
uniform vec2 uPivot;
uniform vec2 uRot;
varying vec2 vQ;
void main() {
    vQ = aPos;
    vec2 p = uRect.xy + aPos * uRect.zw;
    vec2 d = p - uPivot;
    vec2 r = vec2(d.x * uRot.x - d.y * uRot.y, d.x * uRot.y + d.y * uRot.x);
    gl_Position = uProj * vec4(uPivot + r, 0.0, 1.0);
}
)GLSL";

// The batched vertex stream. GLSL ES 100 has no layout qualifiers, so the
// attribute slots are bound before linking (see linkProgram).
const char* const kBatchVertexSrc = R"GLSL(
#version 100
attribute vec2 aPos;
attribute vec4 aRect;
attribute vec4 aColor;
attribute vec4 aColor2;
attribute vec4 aBorder;
attribute vec4 aP0;
attribute vec4 aP1;
attribute vec4 aP2;
uniform mat4 uProj;
uniform vec2 uPivot;
uniform vec2 uRot;
varying vec2 vQ;
varying vec4 vRect;
varying vec4 vColor;
varying vec4 vColor2;
varying vec4 vBorder;
varying vec4 vP0;
varying vec4 vP1;
varying vec4 vP2;
void main() {
    vQ = aPos;
    vRect = aRect;
    vColor = aColor;
    vColor2 = aColor2;
    vBorder = aBorder;
    vP0 = aP0;
    vP1 = aP1;
    vP2 = aP2;
    vec2 p = aRect.xy + aPos * aRect.zw;
    vec2 d = p - uPivot;
    vec2 r = vec2(d.x * uRot.x - d.y * uRot.y, d.x * uRot.y + d.y * uRot.x);
    gl_Position = uProj * vec4(uPivot + r, 0.0, 1.0);
}
)GLSL";

// The uniform program: windows, child surfaces and the wallpaper bake.
const char* const kFragUniformPrologue = R"GLSL(
#version 100
precision highp float;
varying vec2 vQ;

uniform int   uMode;
uniform vec4  uRect;
uniform vec4  uColor;
uniform vec4  uColor2;
uniform vec4  uBorder;
uniform vec4  uShadow;
uniform vec4  uClose;
uniform vec4  uFrame;
uniform vec4  uContent;
uniform float uRadius;
uniform float uOpacity;
uniform float uCaptionH;
uniform float uBtnW;
uniform vec3  uHover;
uniform vec3  uPress;
uniform float uMaximized;
uniform vec2  uScreen;
uniform float uShadowPad;
uniform float uTexMix;
uniform float uKeepAlpha;
uniform float uGlass;
uniform float uTintAmount;
uniform float uSaturate;
uniform float uClipTop;
uniform vec4  uTexRect;
uniform vec2  uArc;
uniform float uThick;
uniform sampler2D uTex;
uniform sampler2D uBlur;
)GLSL";

// The batched program. Per-element state arrives as vertex attributes; the
// window/child-surface branches stay uniform-driven but the shared body below
// is compiled into both programs. Eight varyings and eight attributes is the
// GLES2 floor, which is what this renderer must stay within.
const char* const kFragBatchPrologue = R"GLSL(
#version 100
precision highp float;
varying vec2 vQ;
varying vec4 vRect;
varying vec4 vColor;
varying vec4 vColor2;
varying vec4 vBorder;
varying vec4 vP0;
varying vec4 vP1;
varying vec4 vP2;

uniform vec4  uShadow;
uniform vec4  uClose;
uniform vec4  uFrame;
uniform vec4  uContent;
uniform float uCaptionH;
uniform float uBtnW;
uniform vec3  uHover;
uniform vec3  uPress;
uniform float uMaximized;
uniform vec2  uScreen;
uniform float uShadowPad;
uniform float uGlass;
uniform vec4  uTexRect;
uniform sampler2D uTex;
uniform sampler2D uBlur;

#define uMode       int(vP0.x + 0.5)
#define uRect       vRect
#define uColor      vColor
#define uColor2     vColor2
#define uBorder     vBorder
#define uRadius     vP0.y
#define uOpacity    vP0.z
#define uTintAmount vP0.w
#define uSaturate   vP1.x
#define uThick      vP1.y
#define uArc        vP1.zw
#define uClipTop    vP2.x
#define uTexMix     vP2.y
#define uKeepAlpha  vP2.z
)GLSL";

// One body for both programs, so the batched and uniform paths cannot drift.
const char* const kFragHelpers = R"GLSL(
float sdRound(vec2 p, vec2 b, float r) {
    r = max(0.0, min(r, min(b.x, b.y)));
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

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

float glyphCoverage(vec2 p, int idx, float maximized) {
    float d;
    if (idx == 0) {
        d = segDist(p, vec2(-5.0, 0.0), vec2(5.0, 0.0)) - 0.5;
    } else if (idx == 2) {
        d = min(segDist(p, vec2(-4.5, -4.5), vec2(4.5, 4.5)),
                segDist(p, vec2(4.5, -4.5), vec2(-4.5, 4.5))) - 0.5;
    } else if (maximized > 0.5) {
        d = min(boxRing(p + vec2(-1.5, -1.5), vec2(3.5, 3.5), 1.0),
                boxRing(p + vec2(1.5, 1.5), vec2(3.5, 3.5), 1.0));
    } else {
        d = boxRing(p, vec2(4.5, 4.5), 1.0);
    }
    return cover(d);
}

vec3 micaTint(vec2 p, vec3 tint, float amount) {
    vec2 uv = clamp(p / uScreen, vec2(0.0), vec2(1.0));
    vec3 c = texture2D(uBlur, uv).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return mix(vec3(l) * 0.30, tint, amount);
}
)GLSL";

const char* const kFragMain = R"GLSL(
void main() {
    vec2 p = uRect.xy + vQ * uRect.zw;
    vec2 c = uRect.xy + uRect.zw * 0.5;

    if (uMode == 0) {
        if (p.y < uClipTop) {
            gl_FragColor = vec4(0.0);
            return;
        }
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        float a = cover(d) * uOpacity * uColor.a;
        gl_FragColor = vec4(uColor.rgb * a, a);
        return;
    }

    if (uMode == 1) {
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        vec2 uv = clamp(p / uScreen, vec2(0.0), vec2(1.0));
        vec3 col = texture2D(uBlur, uv).rgb;
        float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = mix(vec3(l), col, uSaturate);
        col = mix(col, uColor.rgb, uTintAmount);
        col += vec3(hash21(floor(p) + 0.5) - 0.5) * 0.016;
        col = mix(col, uBorder.rgb, cover(abs(d + 0.5) - 0.5) * uBorder.a);
        float a = cover(d) * uOpacity * uColor.a;
        gl_FragColor = vec4(col * a, a);
        return;
    }

    if (uMode == 2) {
        vec2 halfSize = uFrame.zw * 0.5;
        vec2 fc = uFrame.xy + halfSize;
        float r = mix(uRadius, 0.0, uMaximized);
        float d = sdRound(p - fc, halfSize, r);
        float shape = cover(d);

        vec2 sc = fc + vec2(0.0, 7.0);
        float ds = sdRound(p - sc, halfSize + vec2(3.0, 3.0), r + 3.0);
        float sh = clamp(1.0 - ds / max(uShadowPad, 1.0), 0.0, 1.0);
        sh = sh * sh * uShadow.a;

        vec3 body = vec3(0.12, 0.12, 0.13);
        float alphaMul = 1.0;
        if (d < 0.0) {
            if (p.y - uFrame.y < uCaptionH) {
                body = micaTint(p, uColor.rgb, uTintAmount);
                float dRight = (uFrame.x + uFrame.z) - p.x;
                int cell = int(floor(max(dRight, 0.0) / max(uBtnW, 1.0)));
                if (cell < 3) {
                    int idx = 2 - cell;
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
                vec2 local = (p - uContent.xy) / max(uContent.zw, vec2(1.0));
                vec2 uv = uTexRect.xy + local * uTexRect.zw;
                bool inTex = uTexMix > 0.5 && local.x >= 0.0 && local.y >= 0.0 &&
                             local.x <= 1.0 && local.y <= 1.0;
                if (uGlass > 0.5) {
                    body = micaTint(p, uColor.rgb, uTintAmount);
                    alphaMul = 1.0;
                    if (inTex) {
                        vec4 t = texture2D(uTex, uv);
                        body = mix(body, t.rgb, mix(1.0, t.a, uKeepAlpha));
                    }
                } else if (inTex) {
                    vec4 t = texture2D(uTex, uv);
                    body = t.rgb;
                    alphaMul = mix(1.0, t.a, uKeepAlpha);
                }
            }
            body = mix(body, uBorder.rgb, cover(abs(d + 0.5) - 0.5) * uBorder.a);
        }
        float bodyA = shape * uOpacity * alphaMul;
        float a = bodyA + sh * (1.0 - bodyA);
        gl_FragColor = vec4(body * bodyA + uShadow.rgb * sh * (1.0 - bodyA), a);
        return;
    }

    if (uMode == 4) {
        float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
        vec4 col = mix(uColor, uColor2, clamp(vQ.y, 0.0, 1.0));
        float a = cover(d) * uOpacity * col.a;
        gl_FragColor = vec4(col.rgb * a, a);
        return;
    }

    if (uMode == 5) {
        vec2 v = p - c;
        float rr = length(v);
        float band = abs(rr - uRadius) - uThick * 0.5;
        float ang = atan(v.x, -v.y);
        if (ang < 0.0) ang += 6.28318530718;
        float span = uArc.y - uArc.x;
        if (span < 0.0) span += 6.28318530718;
        float d = band;
        if (span < 6.28318530718 - 0.0001) {
            float da = ang - uArc.x;
            da -= 6.28318530718 * floor(da / 6.28318530718);
            float over = da - span;
            float edge = over > 0.0 ? over : -min(da, span - da);
            d = max(band, edge * max(rr, 1.0));
        }
        float a = cover(d) * uOpacity * uColor.a;
        gl_FragColor = vec4(uColor.rgb * a, a);
        return;
    }

    if (uMode == 6) {
        // A child surface of a window (a subsurface or a popup): a premultiplied
        // blit of its buffer, clipped to the window's content area and rounded
        // frame so it cannot spill over the chrome.
        vec2 halfSize = uFrame.zw * 0.5;
        vec2 fc = uFrame.xy + halfSize;
        float r = mix(uRadius, 0.0, uMaximized);
        float shape = cover(sdRound(p - fc, halfSize, r));
        float inside = step(uContent.x, p.x) * step(uContent.y, p.y) *
                       step(p.x, uContent.x + uContent.z) *
                       step(p.y, uContent.y + uContent.w);
        vec4 t = texture2D(uTex, vQ);
        float a = shape * inside * uOpacity * mix(1.0, t.a, uKeepAlpha);
        gl_FragColor = vec4(t.rgb * a, a);
        return;
    }

    float d = sdRound(p - c, uRect.zw * 0.5, uRadius);
    vec2 uv = uBorder.zw == vec2(0.0) ? vQ : uBorder.xy + vQ * uBorder.zw;
    vec4 t = texture2D(uTex, uv);
    vec3 rgb = mix(uColor.rgb, t.rgb * uColor.rgb, uTexMix);
    float a = cover(d) * uOpacity * mix(uColor.a, t.a, uKeepAlpha);
    gl_FragColor = vec4(rgb * a, a);
}
)GLSL";

const char* const kBlurSrc = R"GLSL(
#version 100
precision highp float;
varying vec2 vQ;
uniform vec4 uRect;
uniform vec2 uBlurDir;
uniform sampler2D uTex;
void main() {
    vec4 sum = texture2D(uTex, vQ) * 0.227027;
    sum += (texture2D(uTex, vQ + uBlurDir) + texture2D(uTex, vQ - uBlurDir)) * 0.1945946;
    sum += (texture2D(uTex, vQ + uBlurDir * 2.0) + texture2D(uTex, vQ - uBlurDir * 2.0)) * 0.1216216;
    sum += (texture2D(uTex, vQ + uBlurDir * 3.0) + texture2D(uTex, vQ - uBlurDir * 3.0)) * 0.0540540;
    sum += (texture2D(uTex, vQ + uBlurDir * 4.0) + texture2D(uTex, vQ - uBlurDir * 4.0)) * 0.0162162;
    gl_FragColor = sum;
}
)GLSL";

const char* const kWallpaperSrc = R"GLSL(
#version 100
precision highp float;
varying vec2 vQ;
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
    gl_FragColor = vec4(col, 1.0);
}
)GLSL";

const char* const kGenieVertSrc = R"GLSL(
#version 100
attribute vec2 aPos;
uniform mat4 uProj;
uniform vec4 uRect;
uniform vec4 uIcon;
uniform float uProgress;
varying vec2 vUV;
void main() {
    float t = clamp(uProgress, 0.0, 1.0);
    vec2 src = uRect.xy + aPos * uRect.zw;
    float s = clamp(t * (1.0 + aPos.y), 0.0, 1.0);
    s = s * s * (3.0 - 2.0 * s);
    vec2 iconCentre = uIcon.xy + uIcon.zw * 0.5;
    float dstX = iconCentre.x + (aPos.x - 0.5) * uIcon.z;
    float dstY = uIcon.y + aPos.y * uIcon.w;
    vec2 pos = vec2(mix(src.x, dstX, s), mix(src.y, dstY, s));
    vUV = aPos;
    gl_Position = uProj * vec4(pos, 0.0, 1.0);
}
)GLSL";

const char* const kGenieFragSrc = R"GLSL(
#version 100
precision highp float;
varying vec2 vUV;
uniform sampler2D uTex;
uniform float uOpacity;
uniform float uKeepAlpha;
void main() {
    vec4 t = texture2D(uTex, vUV);
    float a = uOpacity * mix(1.0, t.a, uKeepAlpha);
    gl_FragColor = vec4(t.rgb * a, a);
}
)GLSL";

const float kQuad[] = {
    0.f, 0.f,
    1.f, 0.f,
    0.f, 1.f,
    1.f, 1.f,
};

bool compileShader(GLenum type, const char* src, GLuint* out, std::string* error) {
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        if (error) *error = "glCreateShader failed";
        return false;
    }
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string text(static_cast<size_t>(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(shader, len, nullptr, text.data());
        if (error) *error = "shader compile failed: " + text;
        glDeleteShader(shader);
        return false;
    }
    *out = shader;
    return true;
}

bool linkProgram(const char* vsSrc, const char* fsSrc, GLuint* out, std::string* error,
                 bool batched = false) {
    GLuint vs = 0, fs = 0;
    if (!compileShader(GL_VERTEX_SHADER, vsSrc, &vs, error)) return false;
    if (!compileShader(GL_FRAGMENT_SHADER, fsSrc, &fs, error)) {
        glDeleteShader(vs);
        return false;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    // GLSL ES 100 has no layout qualifiers, so the slots are fixed here. The
    // batched stream owns 0..7; every other program only ever uses aPos.
    static const char* const kBatchAttribs[] = {"aPos", "aRect",  "aColor", "aColor2",
                                                "aBorder", "aP0", "aP1",  "aP2"};
    const int attrCount = batched ? 8 : 1;
    for (int i = 0; i < attrCount; ++i)
        glBindAttribLocation(prog, GLuint(i), kBatchAttribs[i]);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string text(static_cast<size_t>(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(prog, len, nullptr, text.data());
        if (error) *error = "shader link failed: " + text;
        glDeleteProgram(prog);
        return false;
    }
    *out = prog;
    return true;
}

void coverCrop(std::vector<unsigned char>* rgba, int* w, int* h, int outW, int outH) {
    if (!rgba || !w || !h || *w <= 0 || *h <= 0 || outW <= 0 || outH <= 0) return;
    const double src = double(*w) / double(*h);
    const double dst = double(outW) / double(outH);
    if (std::fabs(src - dst) < 1e-3) return;

    int cw = *w, ch = *h, cx = 0, cy = 0;
    if (src > dst) {
        cw = int(std::lround(double(*h) * dst));
        if (cw < 1) cw = 1;
        if (cw > *w) cw = *w;
        cx = (*w - cw) / 2;
    } else {
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

// Column major ortho for the shell's top-left screen space.
//
// The X11 build rendered into a GLX window, where GL's origin is the bottom
// left of the *window*, so a y-down projection (screen y = 0 at NDC +1) lined
// up with what the user saw. wlroots' output pass hands us the output buffer
// instead: its first row is the top of the screen, which is GL's bottom row.
// Screen y therefore has to grow toward NDC +1, or every frame is presented
// mirrored upside down (taskbar at the top, text unreadable) and the pointer
// hit tests no longer match what is on screen.
void orthoMatrix(float* m, int w, int h) {
    const float fw = w > 0 ? float(w) : 1.0f;
    const float fh = h > 0 ? float(h) : 1.0f;
    m[0] = 2.0f / fw; m[1] = 0;        m[2] = 0;  m[3] = 0;
    m[4] = 0;         m[5] = 2.0f / fh; m[6] = 0; m[7] = 0;
    m[8] = 0;         m[9] = 0;        m[10] = -1; m[11] = 0;
    m[12] = -1;       m[13] = -1;      m[14] = 0; m[15] = 1;
}

}  // namespace

Compositor::~Compositor() { shutdown(); }

bool Compositor::init(struct wlr_renderer* renderer, int width, int height,
                      const std::string& wallpaperPath, std::string* error) {
    renderer_ = renderer;
    width_ = width > 1 ? width : 1;
    height_ = height > 1 ? height : 1;
    wallpaperPath_ = wallpaperPath;
    orthoMatrix(proj_, width_, height_);
    ready_ = true;
    (void)error;
    return true;
}

void Compositor::shutdown() {
    // GL objects are destroyed by destroyGl() while the context is current, at
    // the end of the last frame; wlroots then tears the context down itself.
    glReady_ = false;
    renderer_ = nullptr;
    ready_ = false;
}

void Compositor::resize(int width, int height) {
    if (width <= 1 || height <= 1) return;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    orthoMatrix(proj_, width_, height_);
    // The wallpaper cover-crop and the half-resolution blur chain are sized to
    // the old output; rebuilding them is what keeps a resized nested window (or
    // a new DRM mode) from sampling a stale, wrong-sized swapchain texture.
    wallpaperPending_ = true;
}

bool Compositor::setWallpaper(const std::string& path, std::string* error) {
    wallpaperPath_ = path;
    wallpaperPending_ = true;
    (void)error;
    return true;
}

void Compositor::bindQuad() {
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
}

void Compositor::bindBatchAttribs() {
    glBindBuffer(GL_ARRAY_BUFFER, batchVbo_);
    const GLsizei stride = kBatchFloatsPerVertex * GLsizei(sizeof(float));
    const auto attrib = [stride](GLuint loc, GLint size, int offsetFloats) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, size, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<const void*>(size_t(offsetFloats) *
                                                            sizeof(float)));
    };
    attrib(0, 2, 0);   // aPos
    attrib(1, 4, 2);   // aRect
    attrib(2, 4, 6);   // aColor
    attrib(3, 4, 10);  // aColor2
    attrib(4, 4, 14);  // aBorder
    attrib(5, 4, 18);  // aP0
    attrib(6, 4, 22);  // aP1
    attrib(7, 4, 26);  // aP2
}

void Compositor::useBatchProgram() {
    if (batchProgramBound_) return;
    glUseProgram(batchProg_);
    glUniformMatrix4fv(bProj_, 1, GL_FALSE, proj_);
    glUniform2f(bScreen_, float(width_), float(height_));
    glUniform1f(bShadowPad_, float(metrics::kShadowPad));
    glUniform1f(bBtnW_, float(metrics::kBtnW));
    glUniform1i(bTex_, 0);
    glUniform1i(bBlur_, 1);
    glUniform2f(bPivot_, 0.f, 0.f);
    glUniform2f(bRot_, 1.f, 0.f);
    bindBatchAttribs();
    batchProgramBound_ = true;
    mainProgramBound_ = false;
}

void Compositor::flushBatch() {
    if (batchQuads_ == 0) return;
    useBatchProgram();
    glBindBuffer(GL_ARRAY_BUFFER, batchVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(batchVerts_.size() * sizeof(float)),
                 batchVerts_.data(), GL_STREAM_DRAW);
    // Point the packed attributes at the storage that was just (re)allocated.
    bindBatchAttribs();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batchTex_);
    if (batchTex_) {
        // wlroots client textures have no mip levels; pin the filter or the
        // incomplete texture samples as black.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(batchQuads_ * kBatchVertsPerQuad));
    ++drawsThisFrame_;
    batchQuads_ = 0;
    batchVerts_.clear();
    batchTex_ = 0;
}

void Compositor::pushQuad(float x, float y, float w, float h, int mode, float radius,
                          float opacity, const Color& c, const Color& c2,
                          const Color& border, float tintAmount, float saturate, float thick,
                          float arc0, float arc1, float clipTop, float texMix,
                          float keepAlpha) {
    static const float kCorners[6][2] = {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f},
                                         {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
    batchVerts_.reserve(batchVerts_.size() + size_t(kBatchVertsPerQuad * kBatchFloatsPerVertex));
    for (const auto& corner : kCorners) {
        const float v[kBatchFloatsPerVertex] = {
            corner[0], corner[1],
            x, y, w, h,
            c.r, c.g, c.b, c.a,
            c2.r, c2.g, c2.b, c2.a,
            border.r, border.g, border.b, border.a,
            float(mode), radius, opacity, tintAmount,
            saturate, thick, arc0, arc1,
            clipTop, texMix, keepAlpha, 0.f};
        batchVerts_.insert(batchVerts_.end(), v, v + kBatchFloatsPerVertex);
    }
    ++batchQuads_;
    ++drawsByMode_[mode];
}

void Compositor::useMainProgram() {
    // Everything queued so far belongs below this element.
    flushBatch();
    if (mainProgramBound_) return;
    glUseProgram(prog_);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, proj_);
    glUniform2f(uScreen_, float(width_), float(height_));
    glUniform1f(uShadowPad_, float(metrics::kShadowPad));
    glUniform1f(uBtnW_, float(metrics::kBtnW));
    glUniform1i(uTex_, 0);
    glUniform1i(uBlur_, 1);
    bindQuad();
    mainProgramBound_ = true;
    batchProgramBound_ = false;
}

bool Compositor::buildGl(std::string* error) {
    if (!buildShaders(error)) return false;
    if (!buildWallpaper(error)) return false;
    glReady_ = true;
    return true;
}

bool Compositor::buildShaders(std::string* error) {
    // One fragment body, two prologues: uniforms for the immediate path, vertex
    // attributes for the batched one.
    const std::string fragUniform =
        std::string(kFragUniformPrologue) + kFragHelpers + kFragMain;
    const std::string fragBatch =
        std::string(kFragBatchPrologue) + kFragHelpers + kFragMain;
    if (!linkProgram(kVertexSrc, fragUniform.c_str(), &prog_, error)) return false;
    if (!linkProgram(kBatchVertexSrc, fragBatch.c_str(), &batchProg_, error, true))
        return false;
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
    uGlass_ = loc("uGlass");
    uTintAmount_ = loc("uTintAmount");
    uSaturate_ = loc("uSaturate");
    uClipTop_ = loc("uClipTop");
    uTexRect_ = loc("uTexRect");
    uArc_ = loc("uArc");
    uThick_ = loc("uThick");
    uTex_ = loc("uTex");
    uBlur_ = loc("uBlur");
    uPivot_ = loc("uPivot");
    uRot_ = loc("uRot");
    uBlurDir_ = glGetUniformLocation(blurProg_, "uBlurDir");
    uWallRes_ = glGetUniformLocation(wallProg_, "uWallRes");
    const auto bloc = [this](const char* n) { return glGetUniformLocation(batchProg_, n); };
    bProj_ = bloc("uProj");
    bScreen_ = bloc("uScreen");
    bShadowPad_ = bloc("uShadowPad");
    bBtnW_ = bloc("uBtnW");
    bTex_ = bloc("uTex");
    bBlur_ = bloc("uBlur");
    bPivot_ = bloc("uPivot");
    bRot_ = bloc("uRot");

    glUseProgram(prog_);
    glUniform2f(uPivot_, 0.f, 0.f);
    glUniform2f(uRot_, 1.f, 0.f);
    glUseProgram(blurProg_);
    glUniform2f(glGetUniformLocation(blurProg_, "uPivot"), 0.f, 0.f);
    glUniform2f(glGetUniformLocation(blurProg_, "uRot"), 1.f, 0.f);
    glUseProgram(wallProg_);
    glUniform2f(glGetUniformLocation(wallProg_, "uPivot"), 0.f, 0.f);
    glUniform2f(glGetUniformLocation(wallProg_, "uRot"), 1.f, 0.f);
    glUseProgram(genieProg_);
    gProj_ = glGetUniformLocation(genieProg_, "uProj");
    gRect_ = glGetUniformLocation(genieProg_, "uRect");
    gIcon_ = glGetUniformLocation(genieProg_, "uIcon");
    gProgress_ = glGetUniformLocation(genieProg_, "uProgress");
    gOpacity_ = glGetUniformLocation(genieProg_, "uOpacity");
    gKeepAlpha_ = glGetUniformLocation(genieProg_, "uKeepAlpha");
    gTex_ = glGetUniformLocation(genieProg_, "uTex");
    glUseProgram(0);

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    // The batch stream is respecified at every flush; only the name has to
    // exist here.
    glGenBuffers(1, &batchVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // The magic lamp's subdivided unit square. ES2 core has no uint indices, so
    // the mesh uses 16 bit ones (the grid is far below 65535 vertices).
    std::vector<float> verts;
    verts.reserve(size_t(kGenieCols + 1) * size_t(kGenieRows + 1) * 2u);
    for (int r = 0; r <= kGenieRows; ++r)
        for (int c = 0; c <= kGenieCols; ++c) {
            verts.push_back(float(c) / float(kGenieCols));
            verts.push_back(float(r) / float(kGenieRows));
        }
    std::vector<unsigned short> idx;
    idx.reserve(size_t(kGenieCols) * size_t(kGenieRows) * 6u);
    for (int r = 0; r < kGenieRows; ++r)
        for (int c = 0; c < kGenieCols; ++c) {
            const unsigned short v0 = (unsigned short)(r * (kGenieCols + 1) + c);
            const unsigned short v1 = v0 + 1;
            const unsigned short v2 = (unsigned short)(v0 + (kGenieCols + 1));
            const unsigned short v3 = v2 + 1;
            idx.push_back(v0); idx.push_back(v1); idx.push_back(v2);
            idx.push_back(v1); idx.push_back(v3); idx.push_back(v2);
        }
    meshIndexCount_ = GLint(idx.size());
    glGenBuffers(1, &meshVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(),
                 GL_STATIC_DRAW);
    glGenBuffers(1, &meshIbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 GLsizeiptr(idx.size() * sizeof(unsigned short)), idx.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    const auto glStr = [](GLenum e) {
        const char* s = reinterpret_cast<const char*>(glGetString(e));
        return std::string(s ? s : "?");
    };
    vendorName_ = glStr(GL_VENDOR);
    rendererName_ = glStr(GL_RENDERER);
    glVersion_ = glStr(GL_VERSION);
    return true;
}

bool Compositor::buildWallpaper(std::string* error) {
    const auto makeTexture = [this](GLuint* tex, int w, int h) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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

    int status = 0;
    std::vector<unsigned char> rgba;
    int imgW = 0, imgH = 0;
    const bool photo = !wallpaperPath_.empty() &&
                       loadPng(wallpaperPath_, &rgba, &imgW, &imgH) && imgW > 0 && imgH > 0;
    if (photo) {
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
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            return false;
        }

        glDisable(GL_BLEND);
        glViewport(0, 0, width_, height_);
        glUseProgram(wallProg_);
        glUniformMatrix4fv(glGetUniformLocation(wallProg_, "uProj"), 1, GL_FALSE, proj_);
        glUniform4f(glGetUniformLocation(wallProg_, "uRect"), 0.f, 0.f, float(width_),
                    float(height_));
        glUniform2f(uWallRes_, float(width_), float(height_));
        bindQuad();
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
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        return false;
    }

    float halfProj[16];
    orthoMatrix(halfProj, halfW_, halfH_);

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
    bindQuad();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

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

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glUseProgram(prog_);
    glUniformMatrix4fv(uProj_, 1, GL_FALSE, proj_);
    glEnable(GL_BLEND);
    mainProgramBound_ = false;
    return true;
}

// ------------------------------------------------------------------ textures
bool Compositor::importBuffer(struct wlr_buffer* buffer, WindowTex* out) {
    if (!buffer || !out || !renderer_) return false;
    releaseWindowTex(out);

    // A client's surface buffer arrives wrapped in a wlr_client_buffer, which
    // has already turned it into a texture (and keeps it up to date across
    // commits via wlr_texture_update_from_buffer). Importing buffer->base
    // instead fails for every client, because that wrapper is not a shm or
    // dmabuf buffer as far as the GLES2 renderer is concerned.
    struct wlr_client_buffer* client = wlr_client_buffer_get(buffer);
    struct wlr_texture* tex = client ? client->texture : nullptr;
    if (client) {
        // The client may recycle this buffer as soon as it commits the next
        // frame, which would destroy the texture we are about to sample. Holding
        // a lock keeps both alive until releaseWindowTex unlocks them.
        wlr_buffer_lock(buffer);
        out->buffer = buffer;
    }
    // A texture that came with the client buffer is owned by wlroots; one we
    // import below is ours to destroy.
    bool owned = false;
    if (!tex) {
        // Shell-owned buffers (wallpapers, thumbnails, frame dumps) go through
        // the renderer directly.
        tex = wlr_texture_from_buffer(renderer_, buffer);
        if (!tex) return false;
        owned = true;
    }
    if (!wlr_texture_is_gles2(tex)) {
        if (owned) wlr_texture_destroy(tex);
        return false;
    }
    struct wlr_gles2_texture_attribs attribs = {};
    wlr_gles2_texture_get_attribs(tex, &attribs);
    if (attribs.target != GL_TEXTURE_2D) {
        // External-only imports need a samplerExternalOES shader; skip rather
        // than sampling an incomplete texture.
        if (owned) wlr_texture_destroy(tex);
        return false;
    }
    out->tex = attribs.tex;
    out->w = tex->width;
    out->h = tex->height;
    out->alpha = attribs.has_alpha;
    out->wlr = tex;
    out->owned = owned;
    return out->valid();
}

void Compositor::releaseWindowTex(WindowTex* t) {
    if (!t) return;
    // Client textures belong to wlr_client_buffer: destroying one here would
    // leave the client surface pointing at freed GL state.
    if (t->wlr && t->owned) wlr_texture_destroy(t->wlr);
    t->wlr = nullptr;
    if (t->buffer) {
        wlr_buffer_unlock(t->buffer);
        t->buffer = nullptr;
    }
    t->owned = false;
    t->tex = 0;
    t->w = t->h = 0;
    t->alpha = false;
}

void Compositor::uploadTexture(GLuint* tex, const unsigned char* rgba, int w, int h) {
    if (!tex || !rgba || w <= 0 || h <= 0) return;
    if (!*tex) glGenTextures(1, tex);
    if (!*tex) return;
    glBindTexture(GL_TEXTURE_2D, *tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Compositor::destroyTexture(GLuint tex) {
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
}

// ------------------------------------------------------------------- drawing
void Compositor::beginFrame() {
    cpuFrameStart_ = nowMs();
    drawsThisFrame_ = 0;
    for (long& n : drawsByMode_) n = 0;
    mainProgramBound_ = false;

    GLint bound = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    fbo_ = GLuint(bound);

    if (!glReady_) {
        std::string err;
        if (!buildGl(&err)) {
            log("renderer init failed: %s", err.c_str());
        }
    } else if (wallpaperPending_) {
        std::string err;
        if (!buildWallpaper(&err)) log("wallpaper reload failed: %s", err.c_str());
        wallpaperPending_ = false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    batchQuads_ = 0;
    batchVerts_.clear();
    batchTex_ = 0;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, halfA_);
    glActiveTexture(GL_TEXTURE0);
    batchProgramBound_ = false;
    useBatchProgram();
}

void Compositor::drawWallpaper() {
    drawTex(wallTex_, Rect{0, 0, width_, height_}, 0.f, Color{1.f, 1.f, 1.f, 1.f}, 1.f,
            true, false);
}

void Compositor::drawRect(const Rect& r, float radius, const Color& c, float opacity,
                          int clipTop) {
    if (r.empty()) return;
    pushQuad(float(r.x), float(r.y), float(r.w), float(r.h), 0, radius, opacity, c, Color{},
             Color{}, 0.f, 0.f, 0.f, 0.f, 0.f,
             clipTop == kNoClip ? -1.0e9f : float(clipTop), 0.f, 0.f);
}

void Compositor::drawRectRotated(int cx, int cy, int w, int h, float angle, float radius,
                                 const Color& c, float opacity) {
    if (w <= 0 || h <= 0) return;
    // Rotation is per-draw uniform state, so this element is painted alone.
    useMainProgram();
    glUniform1i(uMode_, 0);
    glUniform4f(uRect_, float(cx) - w * 0.5f, float(cy) - h * 0.5f, float(w), float(h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform1f(uClipTop_, -1.0e9f);
    glUniform4f(uColor_, c.r, c.g, c.b, c.a);
    glUniform2f(uPivot_, float(cx), float(cy));
    glUniform2f(uRot_, std::cos(angle), std::sin(angle));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUniform2f(uRot_, 1.f, 0.f);
    ++drawsThisFrame_;
    ++drawsByMode_[0];
}

void Compositor::drawGradient(const Rect& r, float radius, const Color& top,
                              const Color& bottom, float opacity) {
    if (r.empty()) return;
    pushQuad(float(r.x), float(r.y), float(r.w), float(r.h), 4, radius, opacity, top,
             bottom, Color{}, 0.f, 0.f, 0.f, 0.f, 0.f, -1.0e9f, 0.f, 0.f);
}

void Compositor::drawArc(int cx, int cy, float radius, float thick, float a0, float a1,
                         const Color& c, float opacity) {
    if (thick <= 0.f || radius <= 0.f) return;
    const float reach = radius + thick * 0.5f + 1.f;
    const int side = int(std::ceil(reach * 2.f));
    // Keep the half-pixel centre: the arc shader antialiases from the exact
    // float rect, so rounding this to an int would shift the stroke.
    pushQuad(float(cx) - side * 0.5f, float(cy) - side * 0.5f, float(side), float(side),
             5, radius, opacity, c, Color{}, Color{}, 0.f, 0.f, thick, a0, a1, -1.0e9f,
             0.f, 0.f);
}

void Compositor::drawAcrylic(const Rect& r, float radius, const Color& tint,
                             float tintAmount, const Color& border, float opacity,
                             float saturation) {
    if (r.empty()) return;
    pushQuad(float(r.x), float(r.y), float(r.w), float(r.h), 1, radius, opacity, tint,
             Color{}, border, tintAmount, saturation, 0.f, 0.f, 0.f, -1.0e9f, 0.f, 0.f);
}

void Compositor::drawTex(GLuint tex, const Rect& dst, float radius, const Color& tint,
                         float opacity, bool textureColor, bool textureAlpha) {
    if (!tex || dst.empty()) return;
    if (batchQuads_ > 0 && batchTex_ != tex) flushBatch();
    if (batchQuads_ == 0) batchTex_ = tex;
    pushQuad(float(dst.x), float(dst.y), float(dst.w), float(dst.h), 3, radius, opacity,
             tint, Color{}, Color{0.f, 0.f, 0.f, 0.f}, 0.f, 0.f, 0.f, 0.f, 0.f,
             -1.0e9f, textureColor ? 1.f : 0.f, textureAlpha ? 1.f : 0.f);
}

void Compositor::drawTexUV(GLuint tex, const Rect& dst, float u0, float v0, float u1,
                           float v1, float radius, const Color& tint, float opacity,
                           bool textureColor, bool textureAlpha) {
    if (!tex || dst.empty()) return;
    if (batchQuads_ > 0 && batchTex_ != tex) flushBatch();
    if (batchQuads_ == 0) batchTex_ = tex;
    // Mode 3 does not use uBorder for anything else, so it carries the UV
    // sub-rect. That keeps the per-quad UV state in the existing batch vertex.
    const Color uv{u0, v0, u1 - u0, v1 - v0};
    pushQuad(float(dst.x), float(dst.y), float(dst.w), float(dst.h), 3, radius, opacity,
             tint, Color{}, uv, 0.f, 0.f, 0.f, 0.f, 0.f, -1.0e9f,
             textureColor ? 1.f : 0.f, textureAlpha ? 1.f : 0.f);
}

void Compositor::drawText(const TextTex& t, const Rect& dst, const Color& c, float opacity) {
    if (!t.tex) return;
    drawTex(t.tex, dst, 0.f, c, opacity, false, true);
}

void Compositor::drawWindowContent(const WindowTex& t, const Rect& dst, const Rect& frame,
                                  const Rect& content, float radius, bool maximized,
                                  float opacity) {
    if (!t.valid() || dst.empty() || content.empty()) return;
    if (!dst.intersects(content)) return;
    useMainProgram();
    ++drawsThisFrame_;
    ++drawsByMode_[2];
    glUniform1i(uMode_, 6);
    glUniform4f(uRect_, float(dst.x), float(dst.y), float(dst.w), float(dst.h));
    glUniform4f(uFrame_, float(frame.x), float(frame.y), float(frame.w), float(frame.h));
    glUniform4f(uContent_, float(content.x), float(content.y), float(content.w),
                float(content.h));
    glUniform1f(uRadius_, radius);
    glUniform1f(uOpacity_, opacity);
    glUniform1f(uMaximized_, maximized ? 1.f : 0.f);
    glUniform1f(uKeepAlpha_, t.alpha ? 1.f : 0.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Compositor::drawWindow(const WindowSprite& s) {
    if (s.frame.empty()) return;
    // The window paints over everything queued so far and owns its texture.
    flushBatch();
    if (s.genie > 0.001f && s.tex.valid() && drawGenie(s)) return;
    const Rect quad = s.frame.inflated(metrics::kShadowPad);
    const int capH = s.captionH > 0 ? s.captionH : 0;
    const Rect content{s.frame.x + metrics::kBorder, s.frame.y + capH,
                       s.frame.w - 2 * metrics::kBorder,
                       s.frame.h - capH - metrics::kBorder};

    const Color caption = s.captionTint
                              ? s.captionColor
                              : (s.focused ? theme::kCaption : theme::kCaptionIdle);
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
    glUniform1f(uTintAmount_, s.tintAmount);
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
    glUniform1f(uGlass_, (s.glass && hasTex && s.tex.alpha) ? 1.f : 0.f);
    glUniform4f(uTexRect_, s.uvX, s.uvY, s.uvW, s.uvH);
    glUniform1i(uMode_, 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hasTex ? s.tex.tex : 0);
    if (hasTex) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool Compositor::drawGenie(const WindowSprite& s) {
    if (!genieProg_ || !meshIndexCount_) return false;
    const int capH = s.captionH > 0 ? s.captionH : 0;
    const Rect content{s.frame.x + metrics::kBorder, s.frame.y + capH,
                       s.frame.w - 2 * metrics::kBorder,
                       s.frame.h - capH - metrics::kBorder};
    if (content.w < 2 || content.h < 2) return false;

    mainProgramBound_ = false;
    batchProgramBound_ = false;
    glUseProgram(genieProg_);
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
    glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshIbo_);
    glDrawElements(GL_TRIANGLES, meshIndexCount_, GL_UNSIGNED_SHORT, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray(0);
    return true;
}

void Compositor::present() {
    // Anything still queued is painted now.
    flushBatch();
    cpuFrameMs_ = nowMs() - cpuFrameStart_;
    drawCalls_ += drawsThisFrame_;
    mainProgramBound_ = false;
    batchProgramBound_ = false;
    for (GLuint i = 0; i < 8; ++i) glDisableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    const double now = nowMs();
    lastFrameSec_ = lastPresentMs_ > 0.0 ? (now - lastPresentMs_) / 1000.0 : 0.0;
    lastPresentMs_ = now;
    if (verbose_ && benchFirstMs_ > 0.0) noteBench(lastFrameSec_ * 1000.0);
    if (benchFirstMs_ <= 0.0) benchFirstMs_ = now;
    benchLastMs_ = now;
    ++benchFrames_;
    benchDraws_ += drawsThisFrame_;
    for (int i = 0; i < 6; ++i) benchPrims_[i] += drawsByMode_[i];
    if (fpsWindowStart_ <= 0.0) fpsWindowStart_ = now;
    ++fpsFrames_;
    const double span = now - fpsWindowStart_;
    if (span >= 500.0) {
        fps_ = int(double(fpsFrames_) * 1000.0 / span + 0.5);
        fpsFrames_ = 0;
        fpsWindowStart_ = now;
        if (verbose_) {
            log("PERF fps=%d cpu=%.2f gpu=%.2f draws=%ld", fps_, cpuFrameMs_, gpuFrameMs_,
                drawsThisFrame_);
        }
    }
}

// ---------------------------------------------------------- --perf summary
void Compositor::addBucket(long* buckets, double ms) {
    int i = int(ms * 4.0);
    if (i < 0) i = 0;
    if (i > kBenchBuckets) i = kBenchBuckets;  // 100 ms+ falls in the last bucket
    ++buckets[i];
}

void Compositor::noteBench(double intervalMs) {
    // Start-up is not the steady state: the first frames compile shaders, bake
    // the wallpaper and build the glyph atlas. Those belong in a start-up time,
    // not in a frame-time percentile of a running session.
    constexpr long kWarmupFrames = 30;
    if (benchFrames_ <= kWarmupFrames) return;
    // A shell with nothing moving presents nothing, so the gap either side of an
    // idle stretch is not a dropped frame and must not be counted as one. 100 ms
    // is beyond any refresh interval a display can ask for (10 Hz) and below the
    // gap an idle session leaves. A sample counts only when the frame before it
    // was also part of a run, which drops the one gap either side of an idle
    // stretch as well as the stretch itself.
    constexpr double kIdleGapMs = 100.0;
    const bool inRun = benchPrevMs_ > 0.0 && benchPrevMs_ <= kIdleGapMs &&
                       intervalMs <= kIdleGapMs;
    benchPrevMs_ = intervalMs;
    if (!inRun) return;
    addBucket(benchInterval_, intervalMs);
    addBucket(benchCpu_, cpuFrameMs_);
    ++benchSamples_;
    benchCpuSumMs_ += cpuFrameMs_;
    benchMaxIntervalMs_ = std::max(benchMaxIntervalMs_, intervalMs);
    benchMaxCpuMs_ = std::max(benchMaxCpuMs_, cpuFrameMs_);
}

void Compositor::reportBench() const {
    const double span = (benchLastMs_ - benchFirstMs_) / 1000.0;
    if (!verbose_ || benchFrames_ < 2 || span <= 0.0) return;
    // `benchFrames_` includes the first frame, which has no interval before it.
    const long frames = benchFrames_;

    // p-th percentile of a bucket histogram: the value at which p% of the
    // samples have been seen, reported at the bucket's upper edge.
    const auto pct = [](const long* buckets, double p) {
        long total = 0;
        for (int i = 0; i <= kBenchBuckets; ++i) total += buckets[i];
        if (total <= 0) return 0.0;
        long seen = 0;
        for (int i = 0; i <= kBenchBuckets; ++i) {
            seen += buckets[i];
            if (double(seen) >= p * double(total)) return double(i + 1) / 4.0;
        }
        return double(kBenchBuckets + 1) / 4.0;
    };

    const double p50 = pct(benchInterval_, 0.50);
    const double p99 = pct(benchInterval_, 0.99);
    const double cpuAvg = benchCpuSumMs_ / double(std::max(1L, benchSamples_));
    // The mean frame interval over the samples that are frames at all (idle gaps
    // are excluded above), which is the rate the shell actually ran at.
    long sampled = 0;
    double intervalSum = 0.0;
    for (int i = 0; i <= kBenchBuckets; ++i) {
        sampled += benchInterval_[i];
        intervalSum += double(benchInterval_[i]) * (double(i) + 0.5) / 4.0;
    }
    const double intervalAvg = sampled > 0 ? intervalSum / double(sampled) : 0.0;
    // A frame that took longer than a whole period and a half missed at least
    // one presentation; the refresh rate is unknown here, so the median
    // interval stands in for the display period.
    long dropped = 0;
    for (int i = 0; i <= kBenchBuckets; ++i) {
        if (double(i + 1) / 4.0 > p50 * 1.5) dropped += benchInterval_[i];
    }

    // Resident set size, in the units /proc reports it (pages of 4 KiB).
    double rssMb = 0.0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        long pages = 0, resident = 0;
        if (std::fscanf(f, "%ld %ld", &pages, &resident) == 2)
            rssMb = double(resident) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
        std::fclose(f);
    }

    std::printf("BENCH frames=%ld sampled=%ld span=%.2fs fps=%.1f draws=%.1f rss=%.1fMB\n",
                frames, sampled, span, intervalAvg > 0.0 ? 1000.0 / intervalAvg : 0.0,
                double(benchDraws_) / double(frames), rssMb);
    if (sampled == 0) {
        std::printf("BENCH no steady-state frames: the shell never drew a run of "
                    "frames (idle, or shorter than the warm-up)\n");
        return;
    }
    std::printf("BENCH interval_ms avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f "
                "low1%%=%.1ffps dropped=%ld (%.1f%%)\n",
                intervalAvg, p50, pct(benchInterval_, 0.95), p99, benchMaxIntervalMs_,
                p99 > 0.0 ? 1000.0 / p99 : 0.0, dropped,
                sampled > 0 ? 100.0 * double(dropped) / double(sampled) : 0.0);
    std::printf("BENCH cpu_ms avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f\n", cpuAvg,
                pct(benchCpu_, 0.50), pct(benchCpu_, 0.95), pct(benchCpu_, 0.99),
                benchMaxCpuMs_);
    // Not a number to be trusted at zero: this build has no GL timer query, so
    // GPU time is simply not measured here (see PERF's gpu= field).
    std::printf("BENCH gpu_ms unmeasured (no GL timer query in this context)\n");

    // Primitive submissions per frame, by shader mode. Unlike the HUD line these
    // are averages over the run, not one frame's snapshot.
    const double n = double(frames);
    std::printf("BENCH prims/frame fill=%.0f acrylic=%.0f window=%.0f texture=%.0f "
                "gradient=%.0f arc=%.0f\n",
                double(benchPrims_[0]) / n, double(benchPrims_[1]) / n,
                double(benchPrims_[2]) / n, double(benchPrims_[3]) / n,
                double(benchPrims_[4]) / n, double(benchPrims_[5]) / n);
}

void Compositor::destroyGl() {
    if (!glReady_) return;
    if (prog_) glDeleteProgram(prog_);
    if (batchProg_) glDeleteProgram(batchProg_);
    if (blurProg_) glDeleteProgram(blurProg_);
    if (wallProg_) glDeleteProgram(wallProg_);
    if (genieProg_) glDeleteProgram(genieProg_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (batchVbo_) glDeleteBuffers(1, &batchVbo_);
    if (meshVbo_) glDeleteBuffers(1, &meshVbo_);
    if (meshIbo_) glDeleteBuffers(1, &meshIbo_);
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
    prog_ = batchProg_ = blurProg_ = wallProg_ = genieProg_ = vbo_ = 0;
    batchVbo_ = 0;
    meshVbo_ = meshIbo_ = 0;
    glReady_ = false;
}

}  // namespace wm
