// The GPU renderer: a GLX context on a full screen overlay window, plus the
// XComposite -> texture_from_pixmap plumbing that lets us composite *foreign*
// X11 windows without ever copying their pixels through the CPU.
//
// Why GLX and not EGL: we are an X11 client talking to an X11 server, and
// GLX_EXT_texture_from_pixmap gives a zero copy view of an XComposite pixmap.
// Presentation goes through glXSwapBuffers with a swap interval of 1, which is
// what delivers real vsync.
//
// Everything is drawn in *screen pixel space*: the vertex shader takes a
// screen-space rect and each fragment knows its own pixel position, so rounded
// corners, borders, shadows and window captions are computed analytically from
// signed distances. That keeps the Windows 11 look resolution independent and
// cheap.
#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <epoxy/gl.h>
#include <epoxy/glx.h>

#include <string>
#include <utility>
#include <vector>

#include "text.h"
#include "theme.h"
#include "util.h"

namespace wm {

// A client's contents as a GL texture; w/h are the *content* pixel size, so the
// frame's inner rect maps to the texture exactly.
struct WindowTex {
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    bool alpha = false;
    bool valid() const { return tex != 0 && w > 0 && h > 0; }
};

// Everything the Fluent window shader needs to draw one managed window.
struct WindowSprite {
    WindowTex tex;
    Rect frame;              // outer frame in screen pixels
    int captionH = metrics::kCaptionH;
    float radius = float(metrics::kRadius);
    bool focused = false;
    bool maximized = false;
    // Client requested a blurred backdrop (_KDE_NET_WM_BLUR_BEHIND_REGION): draw
    // its body as Mica (blurred wallpaper) so translucent content reads as glass.
    bool glass = false;
    float opacity = 1.0f;
    float attention = 0.0f;  // 0..1 flash for _NET_WM_STATE_DEMANDS_ATTENTION
    // Caption button state, in layout order from the right edge. The hovers are
    // 0..1 amounts (the shader mixes by them), so a hover can fade in and out.
    float closeHover = 0.0f, closePress = 0.0f;
    float maxHover = 0.0f, maxPress = 0.0f;
    float minHover = 0.0f, minPress = 0.0f;
    // Magic lamp: when `genie` is > 0 the window's content is forward-warped
    // into `genieIcon` (its taskbar button) by the genie mesh, instead of being
    // drawn as an ordinary frame.
    float genie = 0.0f;
    Rect genieIcon;
};

class Compositor {
public:
    Compositor() = default;
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Creates the overlay window and the GL context. `width`/`height` are the
    // screen size; `wantVsync` requests a swap interval of 1. `wallpaperPath` is
    // the background PNG (may be empty: the procedural backdrop is used then).
    bool init(Display* dpy, int screen, int width, int height, bool wantVsync,
              const std::string& wallpaperPath, std::string* error);
    void shutdown();

    // Re-bakes the wallpaper in place with a new source image (or empty to
    // restore the procedural backdrop). Safe any time after init: the bake is
    // re-run exactly as at start-up, after freeing the previous texture chain,
    // so the session wallpaper can be swapped without restarting the shell.
    bool setWallpaper(const std::string& path, std::string* error);

    bool ready() const { return ready_; }
    Window overlay() const { return overlay_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool vsyncActive() const { return vsyncActive_; }
    int swapInterval() const { return swapInterval_; }
    bool hasTextureFromPixmap() const { return tfi_; }
    const std::string& rendererName() const { return rendererName_; }
    const std::string& vendorName() const { return vendorName_; }
    const std::string& glVersion() const { return glVersion_; }

    // --------------------------------------------------------------- textures
    // Wraps an XComposite named pixmap in a texture (zero copy). The pixmap's
    // visual/depth must be supplied so we can pick a matching GLX FBConfig.
    // Returns 0 when the pixmap cannot be bound, in which case the caller
    // should uploadTexture() a CPU copy instead.
    GLuint bindPixmap(Pixmap pixmap, VisualID visualId, int depth, int w, int h, bool alpha);
    void releasePixmap(GLuint tex);
    // Uploads/updates an ordinary RGBA texture (icons, fallback copies).
    void uploadTexture(GLuint* tex, const unsigned char* rgba, int w, int h);
    void destroyTexture(GLuint tex);

    // ------------------------------------------------------------------ frame
    void beginFrame();
    void drawWallpaper();
    void drawWindow(const WindowSprite& s);
    // `clipTop`, when set, discards pixels above that screen y before the shape
    // is rasterised (used for the Control Centre slider fill).
    static constexpr int kNoClip = -2000000000;
    void drawRect(const Rect& r, float radius, const Color& c, float opacity = 1.0f,
                  int clipTop = kNoClip);
    // A solid rounded rect rotated by `angle` radians about (cx, cy). The rect
    // is `w` x `h` *before* rotation, centred on (cx, cy). Used for the clock
    // hands and ticks, which the axis-aligned drawRect cannot express.
    void drawRectRotated(int cx, int cy, int w, int h, float angle, float radius,
                         const Color& c, float opacity = 1.0f);
    // A vertical gradient rounded fill: `top` at the top edge, `bottom` at the
    // bottom edge. Used by the desktop widgets for their iOS 18 colour panels.
    void drawGradient(const Rect& r, float radius, const Color& top, const Color& bottom,
                      float opacity = 1.0f);
    // An annular arc centred on (cx, cy): `radius` is the ring's centre line and
    // `thick` its stroke width, running clockwise from `a0` to `a1` (0 = 12
    // o'clock). Analytic, so a ring stays smooth at any size.
    void drawArc(int cx, int cy, float radius, float thick, float a0, float a1,
                 const Color& c, float opacity = 1.0f);
    // `saturation` is the backdrop-filter saturate() amount: 1.0 leaves the blur
    // alone, higher values make its colour pop (the Windows 11 web clone uses 3).
    void drawAcrylic(const Rect& r, float radius, const Color& tint, float tintAmount,
                     const Color& border, float opacity = 1.0f, float saturation = 1.45f);
    void drawTex(GLuint tex, const Rect& dst, float radius, const Color& tint,
                 float opacity, bool textureColor, bool textureAlpha);
    void drawText(const TextTex& t, const Rect& dst, const Color& c, float opacity = 1.0f);
    void present();

    // Frame pacing readout for the --stats HUD.
    double lastFrameSeconds() const { return lastFrameSec_; }
    int sampledFps() const { return fps_; }

    // ------------------------------------------------------------- profiling
    // Wall-clock split of the last frame: CPU time spent issuing GL calls vs the
    // GPU time the driver reports for the same span. Timer queries are optional
    // (GL_ARB_timer_query), so gpuMs stays 0 when the driver has none.
    double cpuFrameMs() const { return cpuFrameMs_; }
    double gpuFrameMs() const { return gpuFrameMs_; }
    long drawCalls() const { return drawCalls_; }
    long drawCallsLastFrame() const { return drawsThisFrame_; }
    // Draw calls this frame split by shader mode, so a profile can show whether
    // the cost is in solid fills (0), acrylic (1), windows (2) or textures (3).
    const long* drawsByMode() const { return drawsByMode_; }
    // Set once at init; false when the platform has no timer queries.
    bool hasTimerQuery() const { return timerQuery_ != 0; }
    // Enables the machine-readable "PERF ..." line on stderr, once per 500ms
    // sampling window. Used by scripts/bench.sh; off by default so a normal
    // session is quiet.
    void setVerbosePerf(bool on) { verbose_ = on; }

private:
    bool createContext(std::string* error);
    // An FBConfig whose visual matches `visualId`, needed by glXCreatePixmap
    // for texture_from_pixmap binding (cached, one round trip per visual).
    GLXFBConfig fbcForVisual(VisualID visualId, int depth);
    bool buildShaders(std::string* error);
    bool buildWallpaper(std::string* error);
    void destroyGl();
    void drawQuad(GLuint tex, int mode);
    // Binds the main program + quad VAO once per frame; every primitive goes
    // through this instead of repeating glUseProgram/glBindVertexArray.
    void useMainProgram();
    // Binds/releases an XComposite pixmap texture around a draw (texture_from
    // _pixmap semantics: the server may not write the pixmap while it is bound).
    bool tfiBind(GLuint tex);
    void tfiRelease(GLuint tex);

    // ---- magic lamp (minimise / restore) ----------------------------------
    // A window is forward-warped into (or out of) its taskbar button by drawing
    // it over a fine grid mesh whose vertices follow a genie funnel. Kept apart
    // from the main shader so the ordinary window path stays untouched.
    bool buildGenieMesh();
    bool drawGenie(const WindowSprite& s);
    GLuint genieProg_ = 0;
    GLuint meshVao_ = 0, meshVbo_ = 0, meshIbo_ = 0;
    GLint meshIndexCount_ = 0;
    GLint gProj_ = -1, gRect_ = -1, gIcon_ = -1, gProgress_ = -1;
    GLint gOpacity_ = -1, gKeepAlpha_ = -1, gTex_ = -1;

    Display* dpy_ = nullptr;
    int screen_ = 0;
    int width_ = 0, height_ = 0;
    Window overlay_ = 0;
    Colormap cmap_ = 0;
    Visual* visual_ = nullptr;
    int depth_ = 24;

    GLXContext ctx_ = nullptr;
    GLXFBConfig fbc_ = nullptr;
    GLXWindow glxWin_ = 0;
    bool ready_ = false;
    bool tfi_ = false;
    bool vsyncWanted_ = true;
    bool vsyncActive_ = false;
    int swapInterval_ = 0;

    GLuint prog_ = 0;      // the main Fluent shader
    GLuint blurProg_ = 0;  // separable blur used for acrylic / Mica
    GLuint wallProg_ = 0;  // procedural Windows 11 wallpaper
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    // Uniform locations, resolved once at link time.
    GLint uProj_ = -1;
    GLint uRect_ = -1, uMode_ = -1, uColor_ = -1, uColor2_ = -1, uBorder_ = -1;
    GLint uShadow_ = -1, uClose_ = -1, uFrame_ = -1, uContent_ = -1, uRadius_ = -1;
    GLint uOpacity_ = -1, uCaptionH_ = -1, uBtnW_ = -1, uHover_ = -1, uPress_ = -1;
    GLint uMaximized_ = -1, uScreen_ = -1, uShadowPad_ = -1, uTexMix_ = -1;
    GLint uKeepAlpha_ = -1, uGlass_ = -1, uTintAmount_ = -1, uSaturate_ = -1, uClipTop_ = -1,
          uTex_ = -1, uBlur_ = -1;
    GLint uArc_ = -1, uThick_ = -1;
    GLint uBlurDir_ = -1, uWallRes_ = -1;
    GLint uPivot_ = -1, uRot_ = -1;
    float proj_[16] = {};

    // Wallpaper (a decoded photo when one is available, otherwise the
    // procedural bake) plus its blurred half-resolution acrylic source.
    std::string wallpaperPath_;
    GLuint wallTex_ = 0, wallFbo_ = 0;
    GLuint halfA_ = 0, halfB_ = 0;
    GLuint halfFboA_ = 0, halfFboB_ = 0;
    int halfW_ = 0, halfH_ = 0;

    // GLX pixmap <-> texture bookkeeping, so releasePixmap() can destroy both.
    struct Bound {
        GLXPixmap glxPix = 0;
        int w = 0, h = 0;
    };
    std::vector<std::pair<GLuint, Bound>> bound_;
    std::vector<std::pair<VisualID, GLXFBConfig>> fbcCache_;

    std::string rendererName_, vendorName_, glVersion_;
    double lastFrameSec_ = 0.0;
    double lastPresentMs_ = 0.0;
    int fps_ = 0;
    double fpsWindowStart_ = 0.0;
    int fpsFrames_ = 0;

    // Profiling state: one query in flight, resolved on a later frame, so the
    // frame cost is reported a frame late rather than stalling on a fence.
    GLuint timerQuery_ = 0;
    GLuint timerQueryPending_ = 0;
    long drawCalls_ = 0;
    long drawsThisFrame_ = 0;
    long drawsByMode_[8] = {0};
    double cpuFrameMs_ = 0.0;
    double gpuFrameMs_ = 0.0;
    double cpuFrameStart_ = 0.0;
    bool mainProgramBound_ = false;
    bool verbose_ = false;
    long drawsWindow_ = 0;
    int drawsFrames_ = 0;
};

}  // namespace wm
