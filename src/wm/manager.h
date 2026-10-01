// The window manager itself: X11 policy (map/configure/state/focus/stacking),
// input handling (drag, resize, snapping, shortcuts) and the Windows 11 shell
// (taskbar, Start, Alt-Tab, Task View, flyouts).
//
// Design: we own the root window (SubstructureRedirect) but we never reparent a
// client. Every managed top level stays a direct child of the root and is
// redirected with XComposite so the GPU can composite it. Title bars and
// borders are drawn by us *around* the client's rect, in the overlay, which is
// the bottom-most window -- so clicks on a client reach the client, and clicks
// on our chrome reach us. Nothing about the client's environment is disturbed,
// which is why every X11 program keeps working.
#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <memory>
#include <string>
#include <vector>

#include "apps.h"
#include "compositor.h"
#include "icons.h"
#include "text.h"
#include "theme.h"
#include "util.h"

namespace wm {

// Set by the global X error handler so that requests whose *reply* decides
// whether they worked (grab, redirect, NameWindowPixmap) can be checked.
extern int g_lastError;

// Snap targets, as Windows 11 offers them while a window is dragged to an edge.
enum SnapZone {
    kSnapNone = 0,
    kSnapLeft, kSnapRight, kSnapTop, kSnapBottom,
    kSnapTopLeft, kSnapTopRight, kSnapBottomLeft, kSnapBottomRight,
};

// Every atom we touch, interned once at start up.
struct Atoms {
#define WM_ATOM_LIST(X) \
    X(wmProtocols, "WM_PROTOCOLS")               X(wmDelete, "WM_DELETE_WINDOW") \
    X(wmTakeFocus, "WM_TAKE_FOCUS")              X(wmState, "WM_STATE") \
    X(wmChangeState, "WM_CHANGE_STATE")          X(wmNormalHints, "WM_NORMAL_HINTS") \
    X(wmTransientFor, "WM_TRANSIENT_FOR")        X(wmClass, "WM_CLASS") \
    X(wmName, "WM_NAME")                         X(wmIconName, "WM_ICON_NAME") \
    X(netSupported, "_NET_SUPPORTED")            X(netSupportingWmCheck, "_NET_SUPPORTING_WM_CHECK") \
    X(netClientList, "_NET_CLIENT_LIST")         X(netClientListStacking, "_NET_CLIENT_LIST_STACKING") \
    X(netNumberOfDesktops, "_NET_NUMBER_OF_DESKTOPS") X(netCurrentDesktop, "_NET_CURRENT_DESKTOP") \
    X(netDesktopNames, "_NET_DESKTOP_NAMES")     X(netDesktopGeometry, "_NET_DESKTOP_GEOMETRY") \
    X(netWorkarea, "_NET_WORKAREA")              X(netActiveWindow, "_NET_ACTIVE_WINDOW") \
    X(netDesktopViewport, "_NET_DESKTOP_VIEWPORT") X(netMoveresizeWindow, "_NET_MOVERESIZE_WINDOW") \
    X(netCloseWindow, "_NET_CLOSE_WINDOW")       X(netWmMoveresize, "_NET_WM_MOVERESIZE") \
    X(netWmName, "_NET_WM_NAME")                 X(netWmIcon, "_NET_WM_ICON") \
    X(netWmPid, "_NET_WM_PID")                   X(netWmWindowType, "_NET_WM_WINDOW_TYPE") \
    X(netWmState, "_NET_WM_STATE")               X(netWmAllowedActions, "_NET_WM_ALLOWED_ACTIONS") \
    X(netWmStrut, "_NET_WM_STRUT")               X(netWmStrutPartial, "_NET_WM_STRUT_PARTIAL") \
    X(netFrameExtents, "_NET_FRAME_EXTENTS")     X(netWmDesktop, "_NET_WM_DESKTOP") \
    X(netWmUserTime, "_NET_WM_USER_TIME")        X(netRestackWindow, "_NET_RESTACK_WINDOW") \
    X(netRequestFrameExtents, "_NET_REQUEST_FRAME_EXTENTS") X(netShowingDesktop, "_NET_SHOWING_DESKTOP") \
    X(netWmOpacity, "_NET_WM_OPACITY")           X(netWmBypassCompositor, "_NET_WM_BYPASS_COMPOSITOR") \
    X(motifHints, "_MOTIF_WM_HINTS") \
    X(netWmOpaqueRegion, "_NET_WM_OPAQUE_REGION") X(netWmPing, "_NET_WM_PING") \
    X(netStartupId, "_NET_STARTUP_ID")           X(utf8String, "UTF8_STRING") \
    X(gtkFrameExtents, "_GTK_FRAME_EXTENTS")     X(gtkShowWindowMenu, "_GTK_SHOW_WINDOW_MENU") \
    X(typeNormal, "_NET_WM_WINDOW_TYPE_NORMAL")  X(typeDesktop, "_NET_WM_WINDOW_TYPE_DESKTOP") \
    X(typeDock, "_NET_WM_WINDOW_TYPE_DOCK")      X(typeToolbar, "_NET_WM_WINDOW_TYPE_TOOLBAR") \
    X(typeMenu, "_NET_WM_WINDOW_TYPE_MENU")      X(typeUtility, "_NET_WM_WINDOW_TYPE_UTILITY") \
    X(typeSplash, "_NET_WM_WINDOW_TYPE_SPLASH")  X(typeDialog, "_NET_WM_WINDOW_TYPE_DIALOG") \
    X(typeDropdownMenu, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU") X(typePopupMenu, "_NET_WM_WINDOW_TYPE_POPUP_MENU") \
    X(typeTooltip, "_NET_WM_WINDOW_TYPE_TOOLTIP") X(typeNotification, "_NET_WM_WINDOW_TYPE_NOTIFICATION") \
    X(stateModal, "_NET_WM_STATE_MODAL")         X(stateSticky, "_NET_WM_STATE_STICKY") \
    X(stateMaximizedVert, "_NET_WM_STATE_MAXIMIZED_VERT") X(stateMaximizedHorz, "_NET_WM_STATE_MAXIMIZED_HORZ") \
    X(stateShaded, "_NET_WM_STATE_SHADED")       X(stateSkipTaskbar, "_NET_WM_STATE_SKIP_TASKBAR") \
    X(stateSkipPager, "_NET_WM_STATE_SKIP_PAGER") X(stateHidden, "_NET_WM_STATE_HIDDEN") \
    X(stateFullscreen, "_NET_WM_STATE_FULLSCREEN") X(stateAbove, "_NET_WM_STATE_ABOVE") \
    X(stateBelow, "_NET_WM_STATE_BELOW")         X(stateDemandsAttention, "_NET_WM_STATE_DEMANDS_ATTENTION") \
    X(stateFocused, "_NET_WM_STATE_FOCUSED")     X(actionMove, "_NET_WM_ACTION_MOVE") \
    X(actionResize, "_NET_WM_ACTION_RESIZE")     X(actionMinimize, "_NET_WM_ACTION_MINIMIZE") \
    X(actionMaximizeHorz, "_NET_WM_ACTION_MAXIMIZE_HORZ") X(actionMaximizeVert, "_NET_WM_ACTION_MAXIMIZE_VERT") \
    X(actionFullscreen, "_NET_WM_ACTION_FULLSCREEN") X(actionClose, "_NET_WM_ACTION_CLOSE")

#define WM_ATOM_DECL(name, str) Atom name{};
    WM_ATOM_LIST(WM_ATOM_DECL)
#undef WM_ATOM_DECL
    // _NET_WM_CM_S<n> is not a property: the compositing manager *claims* it as
    // a selection, which is how toolkits discover that a compositor is running.
    Atom netWmCm{};
    void init(Display* dpy) {
#define WM_ATOM_INIT(name, str) name = XInternAtom(dpy, str, False);
        WM_ATOM_LIST(WM_ATOM_INIT)
#undef WM_ATOM_INIT
    }
#undef WM_ATOM_LIST
};

// A managed top level window. `frame` is the chrome we draw around it; the
// client itself lives at clientRect() and is never reparented.
struct Client {
    Window id = 0;
    Rect frame;             // outer frame, screen coordinates
    Rect restore;           // geometry to return to from maximised / snapped
    int captionH = metrics::kCaptionH;
    bool frameless = false;  // client asked for no decorations (_MOTIF_WM_HINTS)
    bool managed = false;   // false for override-redirect / dock / desktop windows
    bool alive = true;
    bool mapped = false;
    bool redirected = false;
    bool unredirected = false;   // fast path: fullscreen window drawn by the server
    bool focused = false;
    bool minimized = false;
    bool fullscreen = false;
    bool maximizedH = false, maximizedV = false;
    bool skipTaskbar = false, skipPager = false;
    bool isDock = false, isDesktop = false, isDialog = false, isModal = false;
    bool isSplash = false, isToolbar = false, isMenu = false;
    bool hasAlpha = false;
    bool inputHint = true;       // WM_HINTS input: false means "focus by protocol"
    bool takeFocus = false;      // WM_TAKE_FOCUS is in WM_PROTOCOLS
    bool userPosition = false;   // the client asked for a specific position
    bool urgent = false;         // _NET_WM_STATE_DEMANDS_ATTENTION
    int snapZone = kSnapNone;
    long strut[4] = {0, 0, 0, 0};  // left, right, top, bottom (dock reservations)

    std::string title = "Window";
    std::string appName;   // lower-case WM_CLASS instance, used to match launchers
    pid_t pid = 0;

    // WM_NORMAL_HINTS, applied to the *client* size while resizing.
    int minW = 1, minH = 1, maxW = 0, maxH = 0;

    // ---- compositing
    Pixmap namedPixmap = 0;
    Pixmap copyPixmap = 0;      // fallback for windows whose visual differs
    Picture srcPic = 0, copyDstPic = 0;
    bool usingCopy = false;
    Visual* visual = nullptr;
    int depth = 24;
    WindowTex tex;
    Damage damage = 0;
    bool damagePending = false;
    bool needsRepaint = true;
    int pixW = 0, pixH = 0;     // size the current pixmap texture was bound at

    GLuint iconTex = 0;
    int iconW = 0, iconH = 0;

    // caption button interaction: 0 minimise, 1 maximise, 2 close, -1 none
    int hoverBtn = -1;
    int pressBtn = -1;

    // ---- animation
    double appear = 1.0;      // 0 -> 1 open animation
    double vanish = 0.0;      // 0 -> 1 close animation
    double minFade = 0.0;     // 0 -> 1 minimise animation
    double zoom = 1.0;        // 1 = final geometry, 0 = animation just started
    Rect drawFrame;           // interpolated frame actually painted
    Rect animFrom;            // frame at the start of a geometry animation
    double animStart = 0.0;
    int animMs = metrics::kAnimMs;
    double attentionPulse = 0.0;
    bool closing = false;
    double closeDeadline = 0.0;
};

// Command line options.
struct Options {
    std::string display;
    bool vsync = true;
    bool stats = false;
    bool unredirect = true;
    int frames = 0;  // > 0: render this many frames and exit (self test)
    // Optional: launch these commands once the WM owns the display. This
    // replaces the old SDL prototype's "--embed prog" flag: instead of
    // forcing one foreign client *inside* another compositor window, the
    // single GPU compositor just starts the apps and manages them normally,
    // with DAMAGE-driven repaint and vsynced presentation.
    std::vector<std::string> launch;
};


// The window manager + shell.
class Manager {
public:
    Manager() = default;
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    int run(const Options& options);
    // Signal safe: the handlers in main() only set this flag and poll() returns
    // with EINTR, so the loop unwinds through its normal shutdown path.
    void stop() { running = false; }

private:
    // ---- start up / tear down
    bool openDisplay(const std::string& name, std::string* error);
    bool claimWm(std::string* error);
    void setupRootProperties();
    bool grabKeys(std::string* error);
    void scanExistingWindows();
    void shutdown();

    // ---- event loop
    void loop();
    void handleEvent(XEvent& ev);
    void tickAnimations(double now);
    void updateHoverStates(int px, int py);

    // ---- client bookkeeping (windows.cpp)
    Client* find(Window w);
    Client* add(Window w, bool existing);
    void removeClient(Client* c);
    void readTitle(Client* c);
    void readClassAndPid(Client* c);
    void readWindowType(Client* c);
    void readDecorations(Client* c);
    void readNormalHints(Client* c);
    void readIcon(Client* c);
    void readStruts(Client* c);
    void readAlpha(Client* c);
    void readOpacity(Client* c);
    void updateStateAtoms(Client* c);
    void updateClientList();
    void updateWorkArea();
    void ensurePixmap(Client* c);
    void destroyPixmap(Client* c);
    // XRender fallback for windows whose visual has no GLX FBConfig.
    bool refreshCopy(Client* c);
    void syncClientGeometry(Client* c);
    void applyFrame(Client* c, bool animate);
    Rect clientRect(const Client* c) const;

    // ---- policy
    void mapClient(Client* c);
    void unmapClient(Client* c);
    void raiseClient(Client* c);
    void restack();
    void focusClient(Client* c, bool raise);
    void focusNext(bool forward);
    Client* activeClient() const;
    void closeClient(Client* c);
    void minimizeClient(Client* c, bool animate);
    void restoreClient(Client* c);
    void toggleMaximize(Client* c);
    void setMaximized(Client* c, bool horizontal, bool vertical);
    void setFullscreen(Client* c, bool on);
    void toggleFullscreen(Client* c);
    void snapClient(Client* c, int zone);
    Rect snapGeometry(int zone) const;
    Rect workArea() const;
    void placeNewClient(Client* c);
    void toggleShowDesktop();
    void updateFullscreenRedirection();
    void activateTaskbarItem(Client* c);
    void cycleTaskbar(bool backward);
    int  openWindows() const;

    // ---- input (input.cpp)
    void onMapRequest(XMapRequestEvent& ev);
    void onConfigureRequest(XConfigureRequestEvent& ev);
    void onConfigureNotify(XConfigureEvent& ev);
    void onPropertyNotify(XPropertyEvent& ev);
    void onClientMessage(XClientMessageEvent& ev);
    void onDamage(Damage damage, Window drawable);
    void onUnmapNotify(XUnmapEvent& ev);
    void onCrossing(XCrossingEvent& ev);
    void onButtonPress(XButtonEvent& ev);
    void onButtonRelease(XButtonEvent& ev);
    void onMotion(XMotionEvent& ev);
    void onKeyPress(XKeyEvent& ev);
    void runShortcut(KeySym sym, unsigned mods);
    void handleOverlayPress(int x, int y, unsigned button, Time time);
    void handleTaskbarPress(int x, int y, unsigned button);
    void handleClientPress(Client* c, int x, int y, unsigned button);
    void beginMove(Client* c, int x, int y);
    void beginResize(Client* c, int edge, int x, int y);
    void updateDrag(int x, int y);
    void endDrag(int x, int y);
    // Abort a drag without applying a snap: the window keeps the geometry the
    // drag last pushed, instead of being snapped to whatever (0,0) means.
    void cancelDrag();
    int  hitEdge(int x, int y, Client** out) const;      // 1..8 edge, 0 none
    int  hitCaptionButton(const Client* c, int x, int y) const;
    int  hitTitleBar(Client* c, int x, int y) const;
    // The top-most managed client whose *chrome* (caption / resize band, never
    // the client's own content) covers the point. The overlay is the bottom
    // window, so clicks on chrome that another window overlaps arrive here
    // instead; this is how we get them back.
    Client* chromeAt(int x, int y) const;
    bool handleChromePress(int x, int y, unsigned button, Time time);
    void grabPointer();
    void ungrabPointer();
    void closeOverlays();
    void showContextMenu(Client* c, int x, int y);
    void openStartMenu();
    void toggleTaskView();
    bool overlayOpen() const;
    bool pointInOverlaySurface(int x, int y) const;
    // The top-most managed client whose frame contains the point (chrome areas
    // only: client content is not ours to hit test).
    Client* clientAt(int x, int y) const;
    int  snapZoneFor(int x, int y) const;
    void openAltTab(bool backward);
    void applyContextAction(int index);

    // ---- shell drawing (draw.cpp)
    void render();
    void drawDesktop();
    void drawClientSprite(Client* c);
    void drawTaskbar();
    void drawStartMenu();
    void drawAltTab();
    void drawTaskView();
    void drawSnapPreview();
    void drawContextMenu();
    void drawStats();
    void layoutTaskbar();
    void layoutStartMenu();
    void drawTextAt(const std::string& s, int px, Weight w, const Color& c, int x, int y);
    void drawTextCentered(const std::string& s, int px, Weight w, const Color& c, const Rect& r);
    void drawTextRight(const std::string& s, int px, Weight w, const Color& c, int right, int y);
    void drawStartGlyph(const Rect& box, const Color& c);
    void drawSearchGlyph(const Rect& box, const Color& c);
    void drawCaptionGlyph(int which, const Rect& box, const Color& c);
    // Reversal icon for a launcher: the entry's Icon= name first, then its
    // StartupWMClass. Returns false when nothing resolved, so the caller can
    // fall back to the coloured letter tile.
    bool drawAppIcon(const Rect& r, const std::string& iconName, const std::string& wmClass,
                     float radius, float opacity);
    void drawAppTile(const Rect& r, const std::string& name, float radius, const Color& tint,
                     bool hovered);
    void drawWindowThumb(const WindowTex& tex, const Rect& dst, float radius, bool focused);

    // ---- desktop icons (draw.cpp / input.cpp)
    void layoutDesktopIcons();
    void drawDesktopIcons();
    // Returns true when the press landed on an icon (and was consumed).
    bool handleDesktopPress(int x, int y, Time time);
    void openDesktopItem(const DesktopItem& item);

    // ---- cursor helper (declared here to keep the cursor table together)
    void setCursor(int which);
    int cursorShown = 0;
    Cursor cursors[7] = {};

    // ---- X state
    Display* dpy = nullptr;
    std::string assetDir;
    int screen = 0;
    Window root = 0;
    Window wmCheckWin = 0;
    Colormap rootCmap = 0;
    Visual* rootVisual = nullptr;
    int rootDepth = 24;
    int screenW = 0, screenH = 0;
    Atoms A;
    int damageEventBase = 0, damageErrorBase = 0;
    bool compositeOk = false, damageOk = false, xfixesOk = false, renderOk = false;
    bool verbose = false;

    Compositor comp;
    Text text;
    IconStore icons;

    std::vector<std::unique_ptr<Client>> clients;  // bottom .. top
    Client* focused = nullptr;
    bool showingDesktop = false;

    // ---- drag / resize
    Client* dragClient = nullptr;
    bool dragIsMove = false;
    int dragEdge = 0;
    Point dragGrab;         // pointer offset inside the frame when the drag began
    Rect dragFrameStart;
    bool dragWasMaximized = false;
    int snapZonePreview = kSnapNone;
    int hoverEdge = 0;
    Client* hoverResizeClient = nullptr;

    // ---- shell state
    bool startOpen = false;
    bool taskViewOpen = false;
    bool altTabOpen = false;
    int altTabIndex = 0;
    std::vector<Client*> altTabOrder;
    bool contextOpen = false;
    Client* contextClient = nullptr;
    Rect contextRect;
    int contextHover = -1;
    std::vector<std::string> contextItems;

    struct TaskItem {
        Rect rect;
        Client* client = nullptr;
    };
    std::vector<TaskItem> taskItems;
    Rect startButtonRect, showDesktopRect, clockRect;
    int hoverTaskIndex = -1;
    bool hoverStart = false;
    bool hoverShowDesktop = false;
    bool hoverClock = false;

    // ---- desktop icons (live contents of the session's Desktop directory)
    std::vector<DesktopItem> desktopItems;
    std::vector<Rect> desktopIconRects;
    int hoverDesktopIcon = -1;
    int selectedDesktopIcon = -1;
    int lastDesktopClick = -1;
    Time lastDesktopClickTime = 0;

    std::vector<AppEntry> apps;
    std::string searchText;
    Rect startRect, searchRect;
    std::vector<Rect> appRects;
    std::vector<size_t> appFiltered;
    int hoverApp = -1;
    std::vector<Rect> taskViewRects;
    Time lastClickTime = 0;
    Client* lastClickClient = nullptr;
    double startAnim = 0.0;
    double taskViewAnim = 0.0;
    double altTabAnim = 0.0;
    int  taskViewHover = -1;

    // ---- loop bookkeeping
    const Options* opts = nullptr;
    // Queued --exec commands: launched once, right after the first present(),
    // so the GL pipeline (context current, VAO bound, wallpaper baked) exists
    // before fork() can touch process-global GL state.
    std::vector<std::string> pendingLaunches;
    double fpsWindowStart = 0.0;
    int fpsFrames = 0;
    int framesRendered = 0;
    double lastTick = 0.0;
    bool running = true;
    bool dirty = true;
    bool pendingRedrawFromDamage = false;
};
}  // namespace wm
