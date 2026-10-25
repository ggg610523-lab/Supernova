// Wayland build of the GPU renderer: the same Fluent shader pipeline as the
// X11 compositor, drawn into wlroots' output framebuffer through EGL/GLES2.
//
// wlroots owns the EGL context and the output's render pass. `beginFrame()` is
// called right after `wlr_output_begin_render_pass()`, which leaves the output
// FBO bound and the context current, so every primitive here renders straight
// into the scanout buffer without a copy.
//
// Client windows are `wlr_texture`s imported from their wl_buffers (dmabuf or
// shm, whichever the client sends) and sampled by the same window shader that
// gives the X11 build its Mica captions and rounded corners.
#pragma once

#include <GLES2/gl2.h>

#include <string>
#include <vector>

#include "text.h"
#include "theme.h"
#include "util.h"

struct wlr_renderer;
struct wlr_texture;
struct wlr_buffer;

namespace wm {

// A client's contents as a GL texture. `wlr` owns the GL texture; WindowTex is
// a non-owning view copied into WindowSprite for the draw call.
struct WindowTex {
    GLuint tex = 0;
    int w = 0;
    int h = 0;
    bool alpha = false;
    struct wlr_texture* wlr = nullptr;
    struct wlr_buffer* buffer = nullptr;  // locked while we hold the texture
    bool owned = false;  // false when the texture belongs to a wlr_client_buffer
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
    bool glass = false;
    float opacity = 1.0f;
    float attention = 0.0f;
    float closeHover = 0.0f, closePress = 0.0f;
    float maxHover = 0.0f, maxPress = 0.0f;
    float minHover = 0.0f, minPress = 0.0f;
    float genie = 0.0f;
    Rect genieIcon;
    bool captionTint = false;
    Color captionColor{};
    float tintAmount = 0.88f;
};

class Compositor {
public:
    Compositor() = default;
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // `renderer` is wlroots' GLES2 renderer. GL objects are created lazily on
    // the first frame, when the EGL context is guaranteed to be current.
    bool init(struct wlr_renderer* renderer, int width, int height,
              const std::string& wallpaperPath, std::string* error);
    void shutdown();

    // Queues a re-bake of the wallpaper; performed inside the next frame, where
    // the GL context and framebuffer state are valid.
    bool setWallpaper(const std::string& path, std::string* error);

    bool ready() const { return ready_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool vsyncActive() const { return vsyncActive_; }
    int swapInterval() const { return swapInterval_; }
    bool hasTextureFromPixmap() const { return true; }
    const std::string& rendererName() const { return rendererName_; }
    const std::string& vendorName() const { return vendorName_; }
    const std::string& glVersion() const { return glVersion_; }

    // --------------------------------------------------------------- textures
    // Imports a client buffer into a WindowTex (a no-op when the buffer is the
    // one already imported). Fails for formats GLES2 cannot sample.
    bool importBuffer(struct wlr_buffer* buffer, WindowTex* out);
    void releaseWindowTex(WindowTex* t);

    void uploadTexture(GLuint* tex, const unsigned char* rgba, int w, int h);
    void destroyTexture(GLuint tex);

    // ------------------------------------------------------------------ frame
    // Called with wlroots' output render pass already begun: the output FBO is
    // bound and the EGL context is current.
    void beginFrame();
    void drawWallpaper();
    void drawWindow(const WindowSprite& s);
    static constexpr int kNoClip = -2000000000;
    void drawRect(const Rect& r, float radius, const Color& c, float opacity = 1.0f,
                  int clipTop = kNoClip);
    void drawRectRotated(int cx, int cy, int w, int h, float angle, float radius,
                         const Color& c, float opacity = 1.0f);
    void drawGradient(const Rect& r, float radius, const Color& top, const Color& bottom,
                      float opacity = 1.0f);
    void drawArc(int cx, int cy, float radius, float thick, float a0, float a1,
                 const Color& c, float opacity = 1.0f);
    void drawAcrylic(const Rect& r, float radius, const Color& tint, float tintAmount,
                     const Color& border, float opacity = 1.0f, float saturation = 1.45f);
    void drawTex(GLuint tex, const Rect& dst, float radius, const Color& tint,
                 float opacity, bool textureColor, bool textureAlpha);
    void drawText(const TextTex& t, const Rect& dst, const Color& c, float opacity = 1.0f);
    // Flushes our state back to something the render pass can finish with.
    void present();

    double lastFrameSeconds() const { return lastFrameSec_; }
    int sampledFps() const { return fps_; }
    double cpuFrameMs() const { return cpuFrameMs_; }
    double gpuFrameMs() const { return gpuFrameMs_; }
    long drawCalls() const { return drawCalls_; }
    long drawCallsLastFrame() const { return drawsThisFrame_; }
    const long* drawsByMode() const { return drawsByMode_; }
    bool hasTimerQuery() const { return false; }
    void setVerbosePerf(bool on) { verbose_ = on; }

private:
    bool buildGl(std::string* error);
    bool buildShaders(std::string* error);
    bool buildWallpaper(std::string* error);
    void destroyGl();
    void useMainProgram();
    void bindQuad();
    bool drawGenie(const WindowSprite& s);

    struct wlr_renderer* renderer_ = nullptr;
    int width_ = 0, height_ = 0;
    bool ready_ = false;
    bool glReady_ = false;
    bool vsyncWanted_ = true;
    bool vsyncActive_ = false;
    int swapInterval_ = 1;

    GLuint prog_ = 0;
    GLuint blurProg_ = 0;
    GLuint wallProg_ = 0;
    GLuint genieProg_ = 0;
    GLuint vbo_ = 0;
    GLuint meshVbo_ = 0, meshIbo_ = 0;
    GLint meshIndexCount_ = 0;

    GLint uProj_ = -1;
    GLint uRect_ = -1, uMode_ = -1, uColor_ = -1, uColor2_ = -1, uBorder_ = -1;
    GLint uShadow_ = -1, uClose_ = -1, uFrame_ = -1, uContent_ = -1, uRadius_ = -1;
    GLint uOpacity_ = -1, uCaptionH_ = -1, uBtnW_ = -1, uHover_ = -1, uPress_ = -1;
    GLint uMaximized_ = -1, uScreen_ = -1, uShadowPad_ = -1, uTexMix_ = -1;
    GLint uKeepAlpha_ = -1, uGlass_ = -1, uTintAmount_ = -1, uSaturate_ = -1,
          uClipTop_ = -1, uTex_ = -1, uBlur_ = -1;
    GLint uArc_ = -1, uThick_ = -1;
    GLint uBlurDir_ = -1, uWallRes_ = -1;
    GLint uPivot_ = -1, uRot_ = -1;
    GLint gProj_ = -1, gRect_ = -1, gIcon_ = -1, gProgress_ = -1;
    GLint gOpacity_ = -1, gKeepAlpha_ = -1, gTex_ = -1;
    float proj_[16] = {};

    GLuint wallTex_ = 0, wallFbo_ = 0;
    GLuint halfA_ = 0, halfB_ = 0;
    GLuint halfFboA_ = 0, halfFboB_ = 0;
    int halfW_ = 0, halfH_ = 0;
    std::string wallpaperPath_;
    bool wallpaperPending_ = false;

    GLuint fbo_ = 0;
    bool mainProgramBound_ = false;

    std::string rendererName_, vendorName_, glVersion_;
    double lastFrameSec_ = 0.0;
    double lastPresentMs_ = 0.0;
    int fps_ = 0;
    double fpsWindowStart_ = 0.0;
    int fpsFrames_ = 0;
    long drawCalls_ = 0;
    long drawsThisFrame_ = 0;
    long drawsByMode_[8] = {0};
    double cpuFrameMs_ = 0.0;
    double gpuFrameMs_ = 0.0;
    double cpuFrameStart_ = 0.0;
    bool verbose_ = false;
};

}  // namespace wm
