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
#include "sysctl.h"
#include "text.h"
#include "theme.h"
#include "util.h"
#include "widgets.h"

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
    double hoverFade[3] = {0.0, 0.0, 0.0};  // eased 0..1 caption-button hover

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
    // Opened from a desktop icon's launch placeholder: the placeholder already
    // supplies the reveal, so the usual zoom-in scale would only fight it and
    // make the window a slightly different size from the tile it replaces.
    bool fromLaunch = false;
    bool closing = false;
    double closeDeadline = 0.0;
    // Hidden by tablet/mobile mode: minimised when the mode was entered so the
    // window is neither painted nor clickable, then restored on the way out.
    bool tabletHidden = false;
    // Opened from the tablet home screen: it fills the area between the status
    // bar and the home indicator, with the decorations stowed while it is up.
    bool tabletApp = false;
    // iOS 26 zoom transition anchor (tablet mode only): the home-screen or dock
    // icon the window grows out of on open and collapses back into on close or
    // minimise. Desktop mode never reads it.
    Rect tabletFrom;
    bool tabletFromValid = false;
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
    // Plain data for one grid cell, declared next to DesktopItem in apps.h.
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
    // Eases every ambient value (hovers, selections, flyout opacity) one frame.
    void tickFluidMotion(double dtMs);
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
    void toggleControlCenter();
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
    void drawControlCenter();
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

    // ---- Control Centre (controlcenter.cpp)
    // The iOS grid is 4 columns x 5 rows; a module occupies one or two cells of
    // it and every interactive part of a module is registered as a CcControl so
    // hit testing is a plain rectangle scan.
    struct CcControl {
        Rect rect;
        int id = 0;          // CcId
        int slot = 0;        // index within a multi-button module
        bool vertical = false;  // slider: value grows upwards
    };
    void layoutControlCenter();
    void updateCcHover(int px, int py);
    void handleControlCenterPress(int x, int y, unsigned button);
    void updateCcDrag(int x, int y);
    void endCcDrag();
    void activateCcControl(int id);
    void launchCcApp(int index);
    // Index into ccControls of a given (id, slot), or -1.
    int ccControlIndex(int id, int slot) const;
    // Scale factor (percent) the whole grid is drawn at, so a short screen
    // shrinks the panel instead of clipping it.
    int ccScale() const;

    // Re-skins the Fluent shell and remembers the choice. The palette itself is
    // theme::applyMode()'s business; this only commits the setting and asks for a
    // repaint, and it is a no-op when the mode is already what was asked for.
    void setLightMode(bool on);

    // ---- desktop icons (draw.cpp / input.cpp)
    void layoutDesktopIcons();
    // Eases every icon's shown cell toward its target cell, so the grid reads
    // like macOS' spring reflow when a widget is placed, removed or dragged.
    void animateDesktopIconReflow(double dtMs);
    void drawDesktopIcons();
    // Returns true when the press landed on an icon (and was consumed).
    bool handleDesktopPress(int x, int y, Time time);
    void openDesktopItem(const DesktopItem& item, const Rect& fromIcon);
    // The icon under a point, or -1. Tests the cells the user is actually looking
    // at, so a press mid-reflow lands on the icon it visibly hit.
    int desktopItemAt(int x, int y) const;

    // ---- desktop folders (the filesystem work is in apps.cpp)
    // Rebuilds the grid around the desktop directory's current contents. Used
    // after the shell itself creates, renames or deletes something; the cells the
    // surviving icons already hold are kept, so the rest of the grid glides to
    // its new place instead of jumping. `selectPath`, when it names an entry that
    // survived, leaves that entry selected -- the grid is sorted by name, so its
    // index has moved and cannot be carried across.
    void refreshDesktop(const std::string& selectPath = {});
    // Renaming a folder: a field over the icon's own label with the keyboard held,
    // so a name can be typed without a second dialog. Return commits, Escape
    // abandons, and either way the grab is handed back.
    void beginDesktopRename(int index);
    void commitDesktopRename();
    void cancelDesktopRename();
    void drawDesktopRename();
    // The field drawn over a folder's label while it is named. Shared by the
    // painter and the press test so what is drawn and what is clickable cannot
    // drift apart. Empty when no rename is up.
    Rect desktopRenameRect() const;
    // Deleting a folder is the one thing here that cannot be undone, so it is only
    // ever reached through this dialog: Escape or Cancel backs out, Return or
    // Delete goes ahead.
    void openConfirmDelete(const std::string& path, const std::string& name);
    void commitConfirmDelete();
    void closeConfirmDelete();
    void drawConfirmDelete();

    // ---- app launch animation (desktop icon -> window, iOS style)
    // Double-clicking a desktop icon grows a placeholder tile out of the icon
    // and into the window the app will open, so the launch feels instant even
    // while the process is still starting. When the real window finally maps it
    // morphs out of the tile instead of popping in.
    struct LaunchAnim {
        Rect from;    // the desktop icon it grew out of
        Rect to;      // the window rect it settles into
        Rect rect;    // current animated rect
        std::string icon;
        std::string name;
        double start = 0.0;
        double ms = 0.0;
        Client* client = nullptr;  // the real window, once it maps
        double claimStart = 0.0;
    };
    std::vector<LaunchAnim> launches;
    void beginLaunchAnim(const DesktopItem& item, const Rect& fromIcon);
    void claimLaunch(Client* c);            // hand a fresh window its launch tile
    void tickLaunches(double now, double dtMs);
    void drawLaunches();

    // ---- desktop widgets (widgets.cpp)
    void initWidgets();                       // default clock + battery
    void drawWidgets();
    void drawClockWidget(const Widget& w);
    void drawBatteryWidget(const Widget& w);
    int  widgetAt(int x, int y) const;        // index, or -1
    bool widgetOnPage(const Widget& w) const;  // is this card on the page shown?
    bool handleWidgetPress(int x, int y, Time time);  // true when consumed
    void beginWidgetDrag(int index, int x, int y, bool resize);
    void updateWidgetDrag(int x, int y);
    void endWidgetDrag();
    void addWidget(WidgetKind kind);
    void removeWidget(int index);
    void suspendWidgets();                    // taken off the desktop for tablet mode
    void restoreWidgets();                    // put back when tablet mode is left
    void openDesktopMenu(int x, int y);       // right click on the desktop
    void refreshBattery(bool force);
    // Corner grip of a widget, for the resize cursor / press test.
    Rect widgetGripRect(const Widget& w) const;

    // ---- tablet / mobile mode (tablet.cpp)
    // An iOS-like home screen: status bar with the date and time, a grid of
    // squircle app icons, a glass dock and the iPhone X home indicator. The
    // desktop windows are hidden while it is up and only the Control Centre stays
    // reachable -- the widget cards come off the desktop entirely. Switching plays
    // a full-screen splash.
    void setTabletMode(bool on);       // starts the splash transition
    void applyTabletMode();            // swap, at the splash midpoint
    void buildTabletEntries();         // the ordered home/dock lists, once
    int tabletDockCapacity() const;   // icons the dock row has room for
    void layoutTabletDockPicker();     // the picker's sheet, tiles and page dots
    void openTabletDockPicker();       // fill it with everything not in the dock
    void closeTabletDockPicker();
    void addTabletDockApps();          // commit the ticked apps to the dock
    void removeTabletDockApp(int i);   // a remove badge was tapped
    void drawTabletDockBadge(const Rect& box, const char* glyph, float a, bool tile);
    void drawTabletDockPickerView();

    void openTabletMenu(int dockIndex, int homeIndex);
    void closeTabletMenu();
    void layoutTabletMenu();
    void drawTabletMenuView();
    bool runTabletMenuAction(int row);
    void tabletMenuItems(std::vector<std::string>& labels, std::vector<char>& danger,
                         std::vector<int>& acts) const;
    // Put a lifted icon back where it came from without running any drop logic: what
    // the quick actions gesture needs, where the long press turns out not to have
    // been a drag after all.
    void cancelTabletIconDrag();
    void drawTabletRunDot(const Rect& box, int jiggle, float a);
    // Whether a home screen entry has a window open. This is the same match that
    // anchors the launch zoom, so the dot lands under the icon the window came from.
    bool tabletEntryRunning(const TabletEntry& e) const;
    bool tabletEntryMatchesClient(const TabletEntry& e, const Client* c) const;
    void saveTabletLayout();           // write the arrangement the user left
    bool loadTabletLayout();           // read it back; false when there is none
    void layoutTabletHome();           // dock/home entries and their rects
    void layoutTabletFolder();         // the open folder's sheet and 3x3 cells
    Rect tabletGridCell(int k) const;  // where icon k of a page sits
    void setTabletHomePage(int page);  // turn to a page, relaying out the grid
    void drawTabletGridPage(int page, int dx, float a, double wiggle);
    void closeTabletFolder();          // fold the open folder back into its icon
    void openTabletFolder(int index);  // zoom a folder's icon out into its sheet
    // iOS names a new folder after what it thinks is in it. There is no category
    // database here, so the honest version is the shared word of its apps, and
    // "Folder" when they share nothing.
    static std::string suggestFolderName(const std::vector<TabletEntry>& apps);
    void drawTabletHome();             // wallpaper overlay: icons and dock
    void drawTabletFolderView();       // the dimmed backdrop and open folder sheet
    void drawTabletRename();           // the rename field, while one is open
    void drawTabletChrome();           // status bar + home indicator, over the app
    void drawTabletSplash();           // the desktop <-> tablet transition
    void drawTabletGlyph(const Rect& box, const Color& c, float opacity);
    bool handleTabletPress(int x, int y, unsigned button, Time time);
    // A tap is acted on when the button comes back up, not when it goes down, so
    // that a press held still is still available to become a drag or a long press.
    void handleTabletRelease(int x, int y, unsigned button, Time time);
    void openTabletEntry(const TabletEntry& e);
    // An app opened from the home screen keeps tablet mode and is inset so the
    // status bar and the home indicator stay tappable above and below it.
    void makeTabletApp(Client* c);
    void endTabletApp(Client* c);
    // The icon rect a tablet window should zoom out of / collapse into: the
    // home-screen entry that matches it, then the dock, then a spot above the
    // home indicator when it has no icon on the grid.
    Rect tabletIconRectFor(const Client* c) const;
    void tabletGoHome();               // minimise every open app, back to the grid
    // The iPhone X home-bar gesture, as Apple documents it: swipe up to go home,
    // swipe up and hold (or overshoot) for the app switcher, swipe sideways to
    // step between apps, and swipe a card up inside the switcher to quit it.
    std::vector<Client*> tabletAppList() const;  // open tablet apps, bottom..top
    bool handleTabletGesturePress(int x, int y);
    void updateTabletGesture(int x, int y);
    void endTabletGesture();
    void switchTabletApp(int dir);
    void openTabletSwitcher();
    void closeTabletSwitcher();
    void layoutTabletSwitcher();
    void drawTabletSwitcher();
    bool handleTabletSwitcherPress(int x, int y, unsigned button);
    void updateTabletSwitchDrag(int x, int y);
    void endTabletSwitchDrag();
    // Home-screen rearrangement: the icons are dragged between grid cells and
    // the widgets are moved and resized exactly as they are on the desktop.
    bool handleTabletIconPress(int x, int y);
    // A drag can be lifted from the grid, from the dock, or from a page of an open
    // folder, and released over any of them; endTabletIconDrag() resolves it.
    void beginTabletIconDrag(int index, int x, int y);
    void beginTabletDockDrag(int index, int x, int y);
    void beginTabletFolderDrag(int cell, int x, int y);
    void updateTabletIconDrag(int x, int y);
    void endTabletIconDrag();
    // A drop helper, a private member rather than a file static because it names
    // the nested entry type. It reports true when the folder is left empty, so the
    // caller can delete it the way iOS does.

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
    Client* contextClient = nullptr;   // null = the desktop (widget) menu
    int contextWidget = -1;            // widget the menu was opened on, or -1
    int contextPin = -1;               // pinned button the menu was opened on, or -1
    int contextApp = -1;               // index into apps (Launchpad tile), or -1
    Rect contextRect;
    int contextHover = -1;
    std::vector<std::string> contextItems;

    struct TaskItem {
        Rect rect;
        Client* client = nullptr;  // null for a pinned launcher
        int pin = -1;              // index into pinned, or -1 for a window
    };
    std::vector<TaskItem> taskItems;
    Rect startButtonRect, showDesktopRect, clockRect;
    int hoverTaskIndex = -1;
    bool hoverStart = false;
    bool hoverShowDesktop = false;
    bool hoverClock = false;

    // ---- pinned taskbar launchers (manager.cpp)
    // The launchers the user pinned, in pin order. They are laid out right after
    // the Start button and before the running windows; each keeps its button
    // whether or not the app is running, and clicking it focuses the app's window
    // when there is one and starts it when there is not.
    std::vector<AppEntry> pinned;
    bool isPinned(const AppEntry& app) const;
    void pinApp(const AppEntry& app);       // no-op when already pinned
    void unpinApp(const std::string& exec);  // no-op when not pinned
    // The topmost live window of a pinned app, or null when it is not running.
    Client* clientForPinned(const AppEntry& app) const;
    void activatePinned(int index);    // focus its window, else launch it
    void activateTaskItem(int index);  // activateTaskbarItem, pins included
    void openPinMenu(int appIndex, int pinIndex, int x, int y);  // right-click menu
    // Reordering: a press on a pinned button arms a drag instead of launching, and
    // once the pointer has travelled past the slop the button lifts out of the bar
    // and the pins swap under it until the button is let go. pinDragOrder is the
    // order as of the press, so Escape can put it back.
    int pinDrag = -1;              // index into pinned being dragged, -1 when idle
    int pinDragPressX = 0;
    bool pinDragMoved = false;
    std::vector<AppEntry> pinDragOrder;
    double pinDragLift = 0.0;      // eased lift of the button being dragged
    void beginPinDrag(int index, int x);
    void updatePinDrag(int x);
    void endPinDrag(bool commit);  // commit = false restores the pressed order

    // Resizing the taskbar by its edge, the way Windows 10 let you: grab the strip
    // along the top of the bar and drag. The grip deliberately straddles the border
    // -- the bar's own empty margin above the buttons, plus a few pixels of desktop
    // above that -- so it can be grabbed from either side and, being proportional
    // to the bar, can never grow into a button.
    bool taskbarGripAt(int x, int y) const;
    void beginTaskbarResize(int y);
    void updateTaskbarResize(int y);
    void endTaskbarResize();       // saves the thickness the drag settled on
    void reflowWorkAreaWindows();  // maximised/snapped windows follow the bar
    int taskbarResizeY = -1;       // pointer y at the press, -1 when not dragging
    int taskbarResizeH = 0;        // thickness at the press, so the bar tracks the
                                   // pointer 1:1 instead of accumulating rounding

    // ---- ambient motion (draw.cpp / manager.cpp)
    // Every hover, selection and flyout eases through these instead of toggling,
    // which is what makes the whole shell read as one continuous surface. The
    // vectors are kept the size of the list they decorate.
    std::vector<double> taskHover;         // per taskbar button
    double startHoverAnim = 0.0;           // Start button wash
    double showDesktopHoverAnim = 0.0;     // show-desktop sliver
    std::vector<double> appHover;          // per Launchpad tile
    std::vector<double> dotHover;          // per page dot
    std::vector<double> ctxHover;          // per context-menu item
    double contextAnim = 0.0;              // context menu open (1) / closed (0)
    std::vector<double> desktopIconHover;  // per desktop icon (hover + selection)
    std::vector<double> widgetHover;       // per desktop widget
    std::vector<double> ccHoverFade;       // per Control Centre control
    double snapPreviewAnim = 0.0;          // snap preview fade

    // ---- desktop icons (live contents of the session's Desktop directory)
    std::vector<DesktopItem> desktopItems;
    std::vector<Rect> desktopIconRects;  // target cells computed by layoutDesktopIcons()
    std::vector<Rect> desktopIconDraw;   // shown cells, eased toward the targets
    int hoverDesktopIcon = -1;
    int selectedDesktopIcon = -1;

    // ---- desktop folders: the entry the desktop menu was opened on, and the two
    // modal dialogs that can sit over the wallpaper while one is being named or
    // deleted.
    int contextDesktop = -1;       // desktop item the menu was opened on, or -1
    int desktopRenameItem = -1;    // the item being renamed, or -1
    std::string desktopRenameText;
    bool confirmDeleteOpen = false;
    // Held by path rather than by index: deleting re-sorts the grid, so an index
    // captured when the menu was built is not the entry by the time it is used.
    std::string confirmDeletePath;
    std::string confirmDeleteName;
    Rect confirmDeleteOk, confirmDeleteCancel;

    std::vector<AppEntry> apps;
    std::string searchText;
    Rect startRect, searchRect;
    std::vector<Rect> appRects;      // cells of the *current* page
    std::vector<size_t> appFiltered;
    std::vector<Rect> appDotRects;   // page-indicator hit targets
    // ---- desktop widgets
    // Cards are a desktop surface only: tablet mode takes them off the screen
    // entirely rather than hiding them behind the home screen.
    std::vector<Widget> widgets;
    // Held aside while tablet mode is up, so leaving it gives the user back the
    // desktop they arranged -- cards included -- exactly as they left it.
    std::vector<Widget> widgetStash;
    int hoverWidget = -1;
    int dragWidget = -1;          // widget being moved/resized, -1 when none
    bool widgetResizing = false;
    Point widgetGrab;             // pointer offset inside the widget at grab
    int batteryPercent = -1;      // -1 = no battery present
    bool batteryCharging = false;
    bool batteryFull = false;
    double lastBatteryProbe = 0.0;
    time_t lastClockSecond = 0;
    int hoverApp = -1;
    int startHoverDot = -1;
    int startPage = 0;
    int startPageCount = 1;
    size_t startPageBase = 0;        // filtered index of appRects[0]
    size_t startPageSize = 0;        // cells per page
    std::vector<Rect> taskViewRects;
    Time lastClickTime = 0;
    Client* lastClickClient = nullptr;
    double startAnim = 0.0;
    double taskViewAnim = 0.0;
    double altTabAnim = 0.0;
    int  taskViewHover = -1;

    // ---- Control Centre
    bool ccOpen = false;
    double ccAnim = 0.0;
    Rect ccRect;                       // the frosted panel
    int ccAnchorX = 0;                 // zoom origin: the point under the clock
    int ccAnchorY = 0;
    std::vector<CcControl> ccControls;  // every clickable part, in draw order
    int ccHover = -1;
    int ccDrag = -1;                   // slider being dragged, -1 when none
    int ccDragValue = 0;               // preview value while dragging
    std::vector<std::string> ccLaunchers;  // exec strings for the launcher row
    SystemControls sysctl;
    bool dnd = false;                  // Do Not Disturb, enforced by the WM

    // ---- tablet / mobile mode
    bool tabletMode = false;          // the mode the shell has settled into
    double tabletAnim = 0.0;          // 0 = desktop, 1 = tablet
    bool modeSwitching = false;       // a splash transition is playing
    bool modeSwapped = false;         // the swap has been applied
    bool modeSwitchTarget = false;    // value tabletMode takes at the midpoint
    double modeSwitchStart = 0.0;
    double splashOpacity = 0.0;       // splash scrim, 0..1
    std::vector<TabletItem> tabletHome;
    std::vector<TabletEntry> tabletDock;
    std::vector<Rect> tabletHomeRects;  // this page's cells only

    // --- the home screen is as many pages wide as it needs ------------------
    // The grid holds a screenful of icons and the rest carry on onto further pages,
    // which turn sideways: dragged with a finger across the wallpaper, or by
    // holding a lifted icon against a screen edge. The page dots between the grid
    // and the dock say how many there are and which one is showing.
    int tabletGridCols = 3;
    int tabletGridCellW = metrics::kTabletCellW;
    int tabletGridCellH = metrics::kTabletCellH;
    int tabletGridX = 0, tabletGridTop = 0;
    int tabletGridStride = 0;      // how far one page slides, in pixels
    int tabletHomePage = 0;
    int tabletHomePageCount = 1;
    int tabletHomeFirst = 0;       // absolute index of this page's first icon
    int tabletHomePerPage = 1;     // icons a page holds at this screen size
    std::vector<Rect> tabletPageDots;
    bool tabletPageSwipe = false;  // a finger is turning the page right now
    int tabletPageSwipeFrom = 0;
    int tabletSwipeStartX = 0;
    double tabletPageOffset = 0.0; // in pages, so a half turn is drawable
    int tabletDragEdge = 0;        // which edge a lifted icon is held against
    Time tabletDragEdgeAt = 0;
    std::vector<Rect> tabletDockRects;

    // --- editing the dock ---------------------------------------------------
    // HarmonyOS edits the dock where it stands rather than on a screen of its
    // own: a long press puts the home screen into rearrange mode, and while it is
    // there every dock icon carries a remove badge in its top-right corner and the
    // row grows a "+" tile that opens the app picker. No button, no second screen.
    Rect tabletDockAddRect;                   // the "+" tile, rearrange mode only
    std::vector<Rect> tabletDockRemoveRects;  // one badge per icon, same mode
    int tabletDockPressRemove = -1;           // the badge a press landed on
    bool tabletDockPressAdd = false;          // the "+" a press landed on

    // The picker behind that "+": every launcher on the machine that is not in the
    // dock yet, twelve to a page, ticked to choose and committed with Done.
    bool tabletDockPickerOpen = false;
    Rect tabletDockPickerPanel, tabletDockPickerDone;
    std::vector<TabletEntry> tabletDockPickerApps;
    std::vector<Rect> tabletDockPickerRects;
    std::vector<Rect> tabletDockPickerDots;
    std::vector<char> tabletDockPickerSel;
    int tabletDockPickerPage = 0;
    int tabletDockPickerPageCount = 1;
    int tabletDockPickerHover = -1;
    int tabletDockPickerPress = -1;        // the tile a press landed on
    bool tabletDockPickerDonePress = false;  // the Done button a press landed on

    // The quick actions sheet: what a still long press opens on an icon. A long
    // press that moves is a drag and always wins; one that holds still opens this
    // instead. It is modal while it is up, so the target can be held by index --
    // nothing can move the layout while the sheet is on screen. Folders are not a
    // target: holding one renames it, which is where iOS puts it.
    bool tabletMenu = false;
    int tabletMenuItem = -1;         // hovered row
    int tabletMenuDock = -1;         // target: dock index, or -1 when it is on the grid
    int tabletMenuHome = -1;         // target: absolute tabletHome index
    bool tabletMenuFolder = false;   // the target is a folder, not an app
    Rect tabletMenuPanel;
    std::vector<Rect> tabletMenuRows;
    std::vector<std::string> tabletMenuLabels;
    std::vector<char> tabletMenuDanger;
    std::vector<double> tabletIconHover;  // per home + dock icon
    Rect tabletStatusRect, tabletDockRect, tabletHomeBarRect;
    int tabletIconSize = metrics::kTabletIcon;
    int tabletDockIconSize = metrics::kTabletDockIcon;
    int tabletHover = -1;
    // Last pointer position, kept because the long press is decided on a tick
    // rather than by an event, so it needs to know where the pointer has got to.
    int pointerX = 0, pointerY = 0;
    bool tabletEntriesBuilt = false;  // the lists survive a reorder
    bool tabletEdit = false;          // rearrange mode, entered by a long press

    // --- app folders ---------------------------------------------------------
    int tabletFolderOpen = -1;      // grid index of the folder on screen, -1 none
    int tabletFolderLast = -1;      // the one still fading out, so closing eases
    int tabletFolderPage = 0;       // which nine of its apps are shown
    double tabletFolderAnim = 0.0;  // 0 closed, 1 fully open
    Rect tabletFolderPanel;          // the open folder's rounded sheet
    std::vector<Rect> tabletFolderRects;  // the 3x3 mini cells on this page
    int tabletFolderPageCount = 1;

    // --- dragging an icon ----------------------------------------------------
    int tabletDragIcon = -1;          // icon being dragged, -1 when none
    int tabletDragTarget = -1;        // grid cell it would drop into
    Point tabletDragGrab;
    Rect tabletDragRect;
    // Where the drag came from, because the drop is resolved against all of them:
    // a grid cell, the dock, or a page of an open folder.
    int tabletDragDockIndex = -1;     // dock slot it was lifted from
    bool tabletDragFromDock = false;
    bool tabletDragFromFolder = false;
    // What it is currently over, all of them tested in priority order on release.
    int tabletDragOverFolder = -1;    // folder icon: the app joins that folder
    int tabletDragOverApp = -1;       // app icon: the two of them make a new folder
    bool tabletDragOverDock = false;
    int tabletDragDockSlot = -1;      // which dock slot, so the drop lands there
    int tabletDragInside = -1;        // mini cell of the open folder

    // --- press and hold ------------------------------------------------------
    // A dwell turns a press into a long press, which is how the home screen is
    // rearranged without hunting for a button: hold an icon and it lifts,
    // hold the background and the whole screen starts jiggling.
    long tabletPressAt = 0;
    Point tabletPressPos;
    int tabletPressItem = -1;         // grid cell under the press, -1 background
    int tabletPressDock = -1;         // dock slot under the press
    int tabletFolderPressCell = -1;   // mini cell under the press, in an open folder
    bool tabletLongPressFired = false;
    void updateTabletLongPress();     // turns a held press into a long press
    void beginTabletRename(int index); // long press on a folder

    // --- renaming a folder ---------------------------------------------------
    // Captured from the keyboard the same way the Start menu's search box is, so
    // no on-screen keyboard is needed: type, Return to commit, Escape to cancel.
    int tabletRenameItem = -1;
    std::string tabletRenameText;

    // ---- iPhone X home-bar gesture
    bool tabletGesture = false;       // a swipe from the bottom edge is running
    int tabletGestureStartX = 0, tabletGestureStartY = 0;
    int tabletGestureCurX = 0, tabletGestureCurY = 0;
    double tabletGestureLastMove = 0.0;  // for the "swipe up and hold" dwell
    bool tabletGestureSwipe = false;     // horizontal: step between apps

    // ---- app switcher (swipe a card up to quit, tap to open)
    bool tabletSwitcher = false;
    double tabletSwitcherAnim = 0.0;
    std::vector<Client*> tabletSwitchOrder;  // live, filtered each frame
    std::vector<Rect> tabletSwitchRects;
    int tabletSwitchDrag = -1;         // card being swiped up, -1 when none
    int tabletSwitchFromY = 0;         // pointer y when the swipe began
    int tabletSwitchTravel = 0;        // current upward travel in pixels
    Rect tabletSwitchDragRect;

    // ---- loop bookkeeping
    const Options* opts = nullptr;    // Queued --exec commands: launched once, right after the first present(),
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
