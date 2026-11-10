#include "lockscreen.h"

#include "apps.h"
#include <compositor.h>
#include "icons.h"
#include "motion.h"
#include "text.h"
#include "theme.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef WIN11WM_HAVE_PAM
#include <security/pam_appl.h>
#endif

namespace wm {
namespace {

bool fileExists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string homeOf(const std::string& user) {
    if (struct passwd* pw = getpwnam(user.c_str())) return pw->pw_dir ? pw->pw_dir : "";
    return {};
}

std::string avatarFor(const std::string& user) {
    if (user.empty()) return {};
    const std::string accounts = "/var/lib/AccountsService/icons/" + user;
    if (fileExists(accounts)) return accounts;
    const std::string home = homeOf(user);
    if (home.empty()) return {};
    for (const char* leaf : {"/.face", "/.face.icon", "/.face.png"}) {
        const std::string p = home + leaf;
        if (fileExists(p)) return p;
    }
    return {};
}

std::string displayName(const struct passwd& pw) {
    if (!pw.pw_gecos || !pw.pw_gecos[0]) return pw.pw_name ? pw.pw_name : "";
    std::string g = pw.pw_gecos;
    const size_t comma = g.find(',');
    if (comma != std::string::npos) g.resize(comma);
    return g.empty() ? (pw.pw_name ? pw.pw_name : "") : g;
}

bool usableShell(const char* shell) {
    if (!shell || !shell[0]) return true;
    const std::string s = shell;
    return s.find("nologin") == std::string::npos && s.find("/false") == std::string::npos;
}

std::string sessionId() {
    const char* v = std::getenv("XDG_SESSION_ID");
    if (!v || !v[0]) return {};
    std::string s = v;
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return s;
}

bool sessionLocked() {
    const std::string sid = sessionId();
    if (sid.empty()) return false;
    const std::string cmd = "loginctl show-session '" + sid + "' -p LockedHint --value 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return false;
    char buf[64] = {};
    const bool ok = std::fgets(buf, sizeof(buf), f) != nullptr;
    pclose(f);
    return ok && trim(buf) == "yes";
}

void runDetached(const std::string& cmd) {
    if (!cmd.empty()) launchApp(cmd);
}

void drawTextCentered(Compositor& comp, Text& text, const std::string& s, int px,
                      Weight weight, const Color& color, const Rect& box, float opacity) {
    if (s.empty()) return;
    const TextTex t = text.get(s, px, weight);
    if (!t.tex) return;
    comp.drawText(t, Rect(box.x + (box.w - t.w) / 2, box.y + (box.h - t.h) / 2, t.w, t.h),
                  color, opacity);
}

void drawTextLeft(Compositor& comp, Text& text, const std::string& s, int px,
                  Weight weight, const Color& color, int x, int centerY, float opacity) {
    if (s.empty()) return;
    const TextTex t = text.get(s, px, weight);
    if (!t.tex) return;
    comp.drawText(t, Rect(x, centerY - t.h / 2, t.w, t.h), color, opacity);
}

}  // namespace

struct LockScreen::Impl {
    struct User {
        std::string name;
        std::string display;
        std::string avatar;
    };

    std::string assetDir;
    bool noAuth = false;
    Mode mode = Mode::Lock;
    bool active = false;
    bool revealDirty = false;
    motion::Spring reveal;
    std::string user;
    std::string password;
    std::string error;
    std::string typedError;
    std::vector<User> users;
    int userIndex = 0;
    double lastPoll = 0.0;
    double suppressUntil = 0.0;
    int lastMinute = -1;

    // iOS-style lock surface: the clock layer is shown first; tapping, pressing
    // Enter or swiping up raises the passcode layer.
    bool credentialSheet = false;
    bool keypad = true;
    // Unlock animation: the passcode layer slides down and the whole lock
    // surface fades while the desktop underneath is already painted.
    bool closing = false;
    double closeStart = 0.0;
    bool pointerDown = false;
    int pressX = 0;
    int pressY = 0;
    Rect fieldRect;
    Rect unlockRect;
    Rect cancelRect;
    Rect keypadToggleRect;
    Rect keyRects[12];
    Rect powerButton;
    bool powerMenu = false;
    std::vector<Rect> userRects;
    Rect powerRects[3];
    int hoverPower = -1;
    int hoverUser = -1;

    void loadUsers() {
        users.clear();
        const uid_t me = getuid();
        if (const struct passwd* self = getpwuid(me)) {
            User u;
            u.name = self->pw_name ? self->pw_name : "";
            u.display = displayName(*self);
            u.avatar = avatarFor(u.name);
            if (!u.name.empty()) users.push_back(std::move(u));
        }
        setpwent();
        while (struct passwd* pw = getpwent()) {
            if (!pw->pw_name || !pw->pw_name[0]) continue;
            if (pw->pw_uid < 1000 || pw->pw_uid >= 65534) continue;
            if (!usableShell(pw->pw_shell)) continue;
            const std::string name = pw->pw_name;
            if (std::any_of(users.begin(), users.end(),
                            [&](const User& u) { return u.name == name; }))
                continue;
            User u;
            u.name = name;
            u.display = displayName(*pw);
            u.avatar = avatarFor(name);
            users.push_back(std::move(u));
        }
        endpwent();
        std::sort(users.begin() + (users.empty() ? 0 : 1), users.end(),
                  [](const User& a, const User& b) { return a.display < b.display; });
        userIndex = 0;
        for (size_t i = 0; i < users.size(); ++i)
            if (users[i].name == user) userIndex = int(i);
    }

    const User* selected() const {
        if (users.empty()) return nullptr;
        return &users[size_t(std::clamp(userIndex, 0, int(users.size()) - 1))];
    }

#ifdef WIN11WM_HAVE_PAM
    static int conversation(int n, const struct pam_message** msg,
                            struct pam_response** resp, void* data) {
        auto* self = static_cast<Impl*>(data);
        if (!self || n <= 0 || !msg || !resp) return PAM_CONV_ERR;
        struct pam_response* out =
            static_cast<struct pam_response*>(std::calloc(size_t(n), sizeof(*out)));
        if (!out) return PAM_BUF_ERR;
        for (int i = 0; i < n; ++i) {
            switch (msg[i]->msg_style) {
                case PAM_PROMPT_ECHO_OFF:
                    out[i].resp = strdup(self->password.c_str());
                    break;
                case PAM_PROMPT_ECHO_ON:
                    out[i].resp = strdup(self->user.c_str());
                    break;
                case PAM_ERROR_MSG:
                case PAM_TEXT_INFO:
                    if (msg[i]->msg && msg[i]->msg[0]) self->typedError = msg[i]->msg;
                    break;
                default:
                    break;
            }
        }
        *resp = out;
        return PAM_SUCCESS;
    }

    bool pamAuth(const std::string& name) {
        struct pam_conv conv = {&Impl::conversation, this};
        struct pam_handle* pamh = nullptr;
        int rc = pam_start("login", name.c_str(), &conv, &pamh);
        if (rc != PAM_SUCCESS) return false;
        rc = pam_authenticate(pamh, PAM_SILENT);
        if (rc == PAM_SUCCESS) rc = pam_acct_mgmt(pamh, PAM_SILENT);
        pam_end(pamh, rc);
        return rc == PAM_SUCCESS;
    }
#else
    bool pamAuth(const std::string&) {
        typedError = "PAM support is unavailable";
        return false;
    }
#endif

    void beginClose() {
        closing = true;
        closeStart = nowMs();
        credentialSheet = true;
        error.clear();
    }

    bool authenticate() {
        if (noAuth) {
            if (mode == Mode::Lock) {
                runDetached("loginctl unlock-session");
                suppressUntil = nowMs() + 2500.0;
            }
            beginClose();
            return true;
        }
        if (password.empty()) {
            error = "Enter your password";
            return true;
        }
        const User* u = selected();
        if (!u) {
            error = "No user selected";
            return true;
        }
        user = u->name;
        typedError.clear();
        const bool ok = pamAuth(user);
        password.clear();
        if (ok) {
            if (mode == Mode::Lock) {
                runDetached("loginctl unlock-session");
                // loginctl is asynchronous; do not immediately re-lock from a
                // stale LockedHint while it catches up.
                suppressUntil = nowMs() + 2500.0;
            }
            beginClose();
        } else {
            error = typedError.empty() ? "Incorrect password" : typedError;
        }
        return true;
    }

    bool submit() {
        if (!active) return false;
        return authenticate();
    }

    bool cycleUser(int delta) {
        if (mode != Mode::Login || users.size() < 2) return false;
        userIndex = (userIndex + delta + int(users.size())) % int(users.size());
        error.clear();
        password.clear();
        return true;
    }

    bool pollLock(double now) {
        if (active || mode != Mode::Lock || now < suppressUntil) return false;
        if (now - lastPoll < 1000.0) return false;
        lastPoll = now;
        if (!sessionLocked()) return false;
        start(Mode::Lock);
        return true;
    }

    void start(Mode m) {
        mode = m;
        active = true;
        credentialSheet = (m == Mode::Login);
        keypad = true;
        closing = false;
        closeStart = 0.0;
        pointerDown = false;
        powerMenu = false;
        password.clear();
        error.clear();
        typedError.clear();
        reveal.value = 0.0;
        reveal.velocity = 0.0;
        reveal.configureIOS(0.28, 0.86);
        revealDirty = true;
        lastMinute = -1;
        if (const char* env = std::getenv("USER"); env && env[0]) user = env;
        if (user.empty()) {
            if (const struct passwd* pw = getpwuid(getuid())) user = pw->pw_name ? pw->pw_name : "";
        }
        loadUsers();
    }

    bool tick(double now) {
        bool changed = false;
        if (!active) return pollLock(now);
        if (closing) {
            if (now - closeStart >= 320.0) {
                active = false;
                closing = false;
                credentialSheet = false;
                password.clear();
            }
            return true;
        }
        if (!reveal.settled(1.0, 0.002)) {
            reveal.step(1.0, 1.0 / 60.0);
            changed = true;
        } else {
            reveal.value = 1.0;
        }
        if (mode == Mode::Lock) {
            const time_t t = time(nullptr);
            struct tm lt {};
            localtime_r(&t, &lt);
            if (lt.tm_min != lastMinute) {
                lastMinute = lt.tm_min;
                changed = true;
            }
        }
        return changed;
    }
};

LockScreen::LockScreen() : impl_(std::make_unique<Impl>()) {}
LockScreen::~LockScreen() = default;

void LockScreen::init(const std::string& assetDir, bool noAuth) {
    impl_->assetDir = assetDir;
    impl_->noAuth = noAuth;
    if (const char* env = std::getenv("USER"); env && env[0]) impl_->user = env;
    if (impl_->user.empty()) {
        if (const struct passwd* pw = getpwuid(getuid()))
            impl_->user = pw->pw_name ? pw->pw_name : "";
    }
}

void LockScreen::shutdown() { impl_->active = false; }

void LockScreen::start(Mode mode) { impl_->start(mode); }
void LockScreen::stop() { impl_->active = false; }
bool LockScreen::active() const { return impl_->active; }

bool LockScreen::tick(double nowMs) { return impl_->tick(nowMs); }

bool LockScreen::handleText(const std::string& utf8) {
    if (!impl_->active || impl_->closing || utf8.empty()) return false;
    for (unsigned char ch : utf8) {
        if (ch < 0x20 || ch == 0x7F) continue;
        if (impl_->password.size() < 128) impl_->password.push_back(char(ch));
    }
    impl_->error.clear();
    return true;
}

bool LockScreen::handleKey(uint32_t sym, const std::string& typed, bool pressed) {
    if (!impl_->active || impl_->closing || !pressed) return false;

    if (!impl_->credentialSheet) {
        switch (sym) {
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space:
            case XKB_KEY_Up:
            case XKB_KEY_Down:
            case XKB_KEY_Tab:
                impl_->credentialSheet = true;
                impl_->error.clear();
                return true;
            case XKB_KEY_Escape:
                return false;
            default:
                if (!typed.empty()) {
                    impl_->credentialSheet = true;
                    return handleText(typed);
                }
                return false;
        }
    }

    switch (sym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            return impl_->submit();
        case XKB_KEY_BackSpace:
            if (!impl_->password.empty()) {
                impl_->password.pop_back();
                impl_->error.clear();
                return true;
            }
            return false;
        case XKB_KEY_Escape:
            if (!impl_->password.empty() || !impl_->error.empty()) {
                impl_->password.clear();
                impl_->error.clear();
                return true;
            }
            if (impl_->mode == Mode::Lock) {
                impl_->credentialSheet = false;
                impl_->keypad = true;
                return true;
            }
            return false;
        case XKB_KEY_Tab:
        case XKB_KEY_Down:
        case XKB_KEY_Right:
            return impl_->cycleUser(1);
        case XKB_KEY_Up:
        case XKB_KEY_Left:
            return impl_->cycleUser(-1);
        default:
            break;
    }
    return handleText(typed);
}

bool LockScreen::controllerNav(int dx, int dy, bool activate, bool back) {
    if (!impl_->active || impl_->closing) return false;
    if (!impl_->credentialSheet) {
        if (activate || dy < 0) {
            impl_->credentialSheet = true;
            impl_->error.clear();
            return true;
        }
        return false;
    }
    if (activate) return impl_->submit();
    if (back) {
        if (!impl_->password.empty() || !impl_->error.empty()) {
            impl_->password.clear();
            impl_->error.clear();
            return true;
        }
        if (impl_->mode == Mode::Lock) {
            impl_->credentialSheet = false;
            impl_->keypad = true;
            return true;
        }
        return false;
    }
    if (dx) return impl_->cycleUser(dx);
    if (dy) return impl_->cycleUser(dy);
    return false;
}

void LockScreen::pointerMove(int x, int y) {
    if (!impl_->active || impl_->closing) return;
    impl_->hoverPower = -1;
    if (impl_->powerMenu) {
        for (int i = 0; i < 3; ++i)
            if (impl_->powerRects[i].contains(x, y)) impl_->hoverPower = i;
    }
    impl_->hoverUser = -1;
    if (impl_->credentialSheet) {
        for (size_t i = 0; i < impl_->userRects.size(); ++i)
            if (impl_->userRects[i].contains(x, y)) impl_->hoverUser = int(i);
    }
}

bool LockScreen::pointerButton(int x, int y, unsigned button, bool pressed) {
    if (!impl_->active || impl_->closing || button != 1) return false;

    if (!pressed) {
        if (!impl_->credentialSheet && impl_->pointerDown) {
            const int dx = x - impl_->pressX;
            const int dy = y - impl_->pressY;
            impl_->pointerDown = false;
            // A real swipe up, or a click/tap on desktop, raises the passcode
            // layer. Nothing authenticates on the first gesture.
            if (dy < -36 || (std::abs(dx) < 14 && std::abs(dy) < 14)) {
                impl_->credentialSheet = true;
                impl_->error.clear();
                return true;
            }
        }
        impl_->pointerDown = false;
        return false;
    }

    if (impl_->powerMenu) {
        for (int i = 0; i < 3; ++i) {
            if (impl_->powerRects[i].contains(x, y)) {
                if (i == 0) runDetached("loginctl suspend");
                else if (i == 1) runDetached("loginctl reboot");
                else runDetached("loginctl poweroff");
                impl_->powerMenu = false;
                return true;
            }
        }
        impl_->powerMenu = false;
        return true;
    }

    if (impl_->powerButton.contains(x, y)) {
        impl_->powerMenu = true;
        return true;
    }

    if (!impl_->credentialSheet) {
        impl_->pointerDown = true;
        impl_->pressX = x;
        impl_->pressY = y;
        return true;
    }

    for (size_t i = 0; i < impl_->userRects.size(); ++i) {
        if (impl_->userRects[i].contains(x, y)) {
            impl_->userIndex = int(i);
            impl_->error.clear();
            impl_->password.clear();
            return true;
        }
    }
    if (impl_->keypad) {
        for (int i = 0; i < 12; ++i) {
            if (!impl_->keyRects[i].contains(x, y)) continue;
            if (i == 9) {
                impl_->keypad = false;
            } else if (i == 11) {
                if (!impl_->password.empty()) impl_->password.pop_back();
                impl_->error.clear();
            } else {
                const char digit = i < 9 ? char('1' + i) : '0';
                if (impl_->password.size() < 128) impl_->password.push_back(digit);
                impl_->error.clear();
            }
            return true;
        }
    } else if (impl_->keypadToggleRect.contains(x, y)) {
        impl_->keypad = true;
        return true;
    }
    if (impl_->unlockRect.contains(x, y)) return impl_->submit();
    if (impl_->mode == Mode::Lock && impl_->cancelRect.contains(x, y)) {
        impl_->credentialSheet = false;
        impl_->keypad = true;
        impl_->password.clear();
        impl_->error.clear();
        return true;
    }
    return impl_->fieldRect.contains(x, y);
}

void LockScreen::draw(Compositor& comp, Text& text, IconStore& icons, int screenW,
                      int screenH, bool tablet) {
    if (!impl_->active) return;
    const float open = float(std::clamp(impl_->reveal.value, 0.0, 1.0));
    const float close = impl_->closing
                            ? float(std::clamp((nowMs() - impl_->closeStart) / 320.0, 0.0, 1.0))
                            : 0.0f;
    const float a = open * (1.0f - close);
    if (a <= 0.001f) return;

    const Color white = rgb(0xFFFFFF);
    const Color muted = rgb(0xFFFFFF, 0.62f);
    const Color dim = rgb(0xFFFFFF, 0.38f);
    const Color accent = rgb(0x0A84FF);  // iOS system blue
    const bool sheet = impl_->credentialSheet;
    const bool touch = tablet || screenW < 640;

    comp.drawWallpaper();
    // iOS lock screen keeps the wallpaper clear in the middle and only darkens
    // the top for the clock and the bottom for the controls.
    comp.drawGradient(Rect(0, 0, screenW, screenH * 0.48f), 0.0f,
                      rgb(0x000000, 0.40f), rgb(0x000000, 0.0f), a);
    comp.drawGradient(Rect(0, screenH * 0.54f, screenW, screenH * 0.46f), 0.0f,
                      rgb(0x000000, 0.0f), rgb(0x000000, 0.48f), a);
    if (sheet) {
        // The passcode layer is a full-screen dim, not a bottom sheet. The
        // stronger scrim stands in for iOS's blurred wallpaper.
        comp.drawRect(Rect(0, 0, screenW, screenH), 0.0f, rgb(0x000000, 0.58f), a);
    }

    const int pad = touch ? 24 : std::max(40, screenW / 24);
    const int statusY = touch ? 12 : 16;
    const Impl::User* selected = impl_->selected();

    // ---------------------------------------------------------- status bar
    if (selected && !selected->display.empty()) {
        drawTextLeft(comp, text, selected->display, touch ? 11 : 12, Weight::Medium,
                     rgb(0xFFFFFF, 0.58f), pad, statusY + 8, a);
    }

    const int bw = touch ? 24 : 26;
    const int bh = touch ? 12 : 13;
    const Rect batt(screenW - pad - bw, statusY + 3, bw, bh);
    comp.drawRect(Rect(batt.x, batt.y, batt.w, 2), 1.0f, rgb(0xFFFFFF, 0.72f), a);
    comp.drawRect(Rect(batt.x, batt.bottom() - 2, batt.w, 2), 1.0f, rgb(0xFFFFFF, 0.72f), a);
    comp.drawRect(Rect(batt.x, batt.y, 2, batt.h), 1.0f, rgb(0xFFFFFF, 0.72f), a);
    comp.drawRect(Rect(batt.right() - 2, batt.y, 2, batt.h), 1.0f, rgb(0xFFFFFF, 0.72f), a);
    comp.drawRect(Rect(batt.x + 3, batt.y + 3, int((batt.w - 6) * 0.78f), batt.h - 6),
                  1.5f, white, a * 0.82f);
    comp.drawRect(Rect(batt.right(), batt.y + batt.h / 3, 2, batt.h / 3), 1.0f,
                  rgb(0xFFFFFF, 0.62f), a);
    // The battery is the status-bar affordance for the power menu.
    impl_->powerButton = Rect(batt.x - 10, batt.y - 8, batt.w + 22, batt.h + 16);

    // ------------------------------------------------------------- clock
    char clock[16] = {};
    char date[64] = {};
    const time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
    char dateBase[32] = {};
    std::strftime(dateBase, sizeof(dateBase), "%A, %B", &lt);
    std::snprintf(date, sizeof(date), "%s %d", dateBase, lt.tm_mday);

    const int clockPx = touch ? std::clamp(int(screenW * 0.30f), 88, 140)
                               : std::clamp(screenW / 8, 100, 176);
    const int clockPxNow = sheet ? int(clockPx * 0.62f) : clockPx;
    const int dateY = sheet ? (touch ? 46 : 38) : (touch ? 70 : 58);
    const int clockTop = dateY + (sheet ? 24 : 30);

    drawTextCentered(comp, text, date, touch ? 15 : 15, Weight::Medium,
                     rgb(0xFFFFFF, 0.92f), Rect(0, dateY, screenW, 22), a);
    drawTextCentered(comp, text, clock, clockPxNow, Weight::Medium,
                     rgb(0x000000, 0.22f), Rect(0, clockTop + 2, screenW, clockPxNow + 12), a);
    drawTextCentered(comp, text, clock, clockPxNow, Weight::Medium, white,
                     Rect(0, clockTop, screenW, clockPxNow + 12), a);

    // -------------------------------------------------------- lock controls
    if (!sheet) {
        const int shortcut = touch ? 54 : 46;
        const int rowCy = screenH - (touch ? 76 : 64);
        const int sy = rowCy - shortcut / 2;
        auto glassCircle = [&](int x) {
            const Rect r(x, sy, shortcut, shortcut);
            comp.drawRect(r, float(shortcut) / 2.0f, rgb(0x1C1C1E, 0.42f), a);
            comp.drawRect(r, float(shortcut) / 2.0f, rgb(0xFFFFFF, 0.12f), a);
            return r;
        };
        const Rect flash = glassCircle(pad);
        const Rect camera = glassCircle(screenW - pad - shortcut);

        const int fcx = flash.x + flash.w / 2;
        const int fcy = flash.y + flash.h / 2;
        comp.drawRect(Rect(fcx - 4, fcy - 9, 8, 15), 2.5f, white, a);
        comp.drawRect(Rect(fcx - 7, fcy - 12, 14, 5), 2.0f, white, a);
        comp.drawRect(Rect(fcx - 1, fcy + 7, 2, 4), 1.0f, rgb(0xFFFFFF, 0.7f), a);

        const int ccx = camera.x + camera.w / 2;
        const int ccy = camera.y + camera.h / 2;
        comp.drawRect(Rect(ccx - 11, ccy - 7, 22, 16), 4.0f, white, a);
        comp.drawRect(Rect(ccx - 5, ccy - 10, 10, 4), 1.5f, white, a);
        comp.drawArc(ccx, ccy + 1, 4.5f, 2.0f, 0.0f, 6.2831853f, rgb(0x1C1C1E), a);

        // Center lock glyph, iOS bottom row.
        const int lx = screenW / 2;
        const int ly = rowCy - 6;
        comp.drawRect(Rect(lx - 9, ly, 18, 13), 3.0f, rgb(0xFFFFFF, 0.94f), a);
        comp.drawArc(lx, ly + 1, 6.5f, 2.2f, 4.7123890f, 1.5707963f,
                     rgb(0xFFFFFF, 0.94f), a);
        comp.drawRect(Rect(lx - 1, ly + 4, 2, 4), 1.0f, rgb(0x1C1C1E), a);

        // iOS home indicator.
        const int hw = std::min(180, int(screenW * 0.34f));
        comp.drawRect(Rect((screenW - hw) / 2, screenH - 16, hw, 5), 2.5f,
                      rgb(0xFFFFFF, 0.82f), a);
    }

    // ------------------------------------------------------ passcode layer
    if (sheet) {
        const int uiShift = int((1.0f - open) * 30.0f + close * 60.0f);
        const int titleY = int(screenH * (impl_->mode == Mode::Login ? (touch ? 0.36f : 0.34f)
                                                                     : (touch ? 0.34f : 0.30f))) +
                           uiShift;

        impl_->userRects.clear();
        if (impl_->mode == Mode::Login) {
            int uy = titleY - 58;
            if (impl_->users.size() > 1) {
                const int chip = touch ? 34 : 30;
                const int gap = 8;
                const int maxChips = std::min<int>(5, int(impl_->users.size()));
                const int total = maxChips * chip + (maxChips - 1) * gap;
                int x = (screenW - total) / 2;
                for (int i = 0; i < maxChips; ++i) {
                    const Rect r(x, titleY - 98, chip, chip);
                    const bool on = i == impl_->userIndex;
                    comp.drawRect(r, float(chip) / 2.0f,
                                  on ? accent : rgb(0xFFFFFF, 0.14f), a);
                    const std::string initial =
                        impl_->users[size_t(i)].display.empty()
                            ? "?"
                            : impl_->users[size_t(i)].display.substr(0, 1);
                    drawTextCentered(comp, text, initial, std::max(11, chip / 2),
                                     Weight::Bold, white, r, a);
                    impl_->userRects.push_back(r);
                    x += chip + gap;
                }
            }
            const int av = touch ? 40 : 36;
            const std::string name = selected ? selected->display : "User";
            const TextTex nt = text.get(name, touch ? 17 : 16, Weight::Medium);
            const int groupW = av + 10 + (nt.tex ? nt.w : 0);
            const int gx = (screenW - groupW) / 2;
            const Rect avatarRect(gx, uy, av, av);
            if (selected && !selected->avatar.empty()) {
                const IconTex ic = icons.get(selected->avatar, av, IconTheme::Hatter);
                if (ic.valid())
                    comp.drawTex(ic.tex, avatarRect, float(av) / 2.0f, white, a, true, true);
                else
                    comp.drawRect(avatarRect, float(av) / 2.0f, accent, a);
            } else {
                comp.drawRect(avatarRect, float(av) / 2.0f, rgb(0xFFFFFF, 0.16f), a);
                const std::string initial =
                    selected && !selected->display.empty() ? selected->display.substr(0, 1) : "?";
                drawTextCentered(comp, text, initial, av / 2, Weight::Bold, white,
                                 avatarRect, a);
            }
            drawTextLeft(comp, text, name, touch ? 17 : 16, Weight::Medium, white,
                         avatarRect.right() + 10, avatarRect.y + av / 2, a);
        }

        drawTextCentered(comp, text,
                         impl_->mode == Mode::Login ? "Enter Password" : "Enter Passcode",
                         touch ? 15 : 15, Weight::Medium, rgb(0xFFFFFF, 0.92f),
                         Rect(0, titleY, screenW, 20), a);

        const int dotsY = titleY + 34;
        const int count = std::min<int>(12, int(impl_->password.size()));
        const int slots = std::max(6, count);
        const int dot = touch ? 9 : 8;
        const int gap = touch ? 18 : 16;
        const int total = slots * gap - (gap - dot);
        int dx = (screenW - total) / 2;
        for (int i = 0; i < slots; ++i) {
            const Rect r(dx, dotsY, dot, dot);
            comp.drawRect(r, float(dot) / 2.0f,
                          i < count ? white : rgb(0xFFFFFF, 0.22f), a);
            dx += gap;
        }

        // A small check affordance is needed because this password can be any
        // length; iOS auto-submits a fixed-length passcode, we cannot.
        if (impl_->keypad && !impl_->password.empty()) {
            impl_->unlockRect = Rect(dx + 4, dotsY - 10, 30, 30);
            comp.drawRect(impl_->unlockRect, 15.0f, accent, a);
            comp.drawRectRotated(impl_->unlockRect.x + 9, impl_->unlockRect.y + 15,
                                 9, 2.0f, -0.7f, 1.0f, white, a);
            comp.drawRectRotated(impl_->unlockRect.x + 16, impl_->unlockRect.y + 13,
                                 15, 2.0f, 0.6f, 1.0f, white, a);
        } else if (!impl_->keypad) {
            // Set in the text-field branch below.
        } else {
            impl_->unlockRect = Rect{};
        }

        int y = dotsY + 42;
        if (!impl_->error.empty()) {
            drawTextCentered(comp, text, impl_->error, touch ? 13 : 12, Weight::Regular,
                             rgb(0xFF453A), Rect(pad, y - 8, screenW - 2 * pad, 20), a);
            y += 22;
        }

        for (Rect& r : impl_->keyRects) r = Rect{};
        impl_->keypadToggleRect = Rect{};
        if (impl_->keypad) {
            impl_->fieldRect = Rect{};
            const int keyW = touch ? 78 : 72;
            const int keyH = touch ? 62 : 56;
            const int gapX = 6;
            const int gapY = 4;
            const int keypadH = keyH * 3 + gapY * 2;
            const int keypadTop =
                std::min(int(screenH * (touch ? 0.53f : 0.50f)),
                         screenH - keypadH - (touch ? 120 : 96));
            y = std::max(y, std::max(0, keypadTop));
            const int totalW = keyW * 3 + gapX * 2;
            const int kx = (screenW - totalW) / 2;
            const char* labels[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9",
                                      "ABC", "0", "<"};
            const char* letters[8] = {"ABC", "DEF", "GHI", "JKL",
                                      "MNO", "PQRS", "TUV", "WXYZ"};
            for (int i = 0; i < 12; ++i) {
                const int row = i / 3;
                const int col = i % 3;
                const Rect r(kx + col * (keyW + gapX), y + row * (keyH + gapY),
                             keyW, keyH);
                impl_->keyRects[i] = r;
                drawTextCentered(comp, text, labels[i], i == 9 ? 13 : (touch ? 28 : 26),
                                 i == 9 ? Weight::Medium : Weight::Regular, white, r, a);
                if (i >= 1 && i <= 8) {
                    drawTextCentered(comp, text, letters[i - 1], touch ? 9 : 8,
                                     Weight::Medium, rgb(0xFFFFFF, 0.48f),
                                     Rect(r.x, r.y + int(r.h * 0.64f), r.w, 12), a);
                }
            }
        } else {
            const int fieldW = std::min(screenW - 2 * pad, 360);
            impl_->fieldRect = Rect((screenW - fieldW) / 2, y + 8, fieldW, touch ? 54 : 50);
            comp.drawRect(impl_->fieldRect, 14.0f, rgb(0x1C1C1E, 0.55f), a);
            comp.drawRect(impl_->fieldRect, 14.0f,
                          impl_->error.empty() ? rgb(0xFFFFFF, 0.14f) : rgb(0xFF453A, 0.72f), a);
            if (impl_->password.empty()) {
                drawTextLeft(comp, text, "Password", touch ? 16 : 15, Weight::Regular,
                             dim, impl_->fieldRect.x + 18,
                             impl_->fieldRect.y + impl_->fieldRect.h / 2, a);
            } else {
                int px = impl_->fieldRect.x + 18;
                const int dy = impl_->fieldRect.y + impl_->fieldRect.h / 2;
                for (size_t i = 0; i < impl_->password.size() && i < 24; ++i) {
                    comp.drawRect(Rect(px, dy - 4, 8, 8), 4.0f, white, a);
                    px += 14;
                }
            }
            impl_->unlockRect = Rect(impl_->fieldRect.right() - 44,
                                     impl_->fieldRect.y + 4, 44, impl_->fieldRect.h - 8);
            comp.drawRect(impl_->unlockRect, float(impl_->unlockRect.h) / 2.0f, accent, a);
            drawTextCentered(comp, text, ">", 21, Weight::Bold, white, impl_->unlockRect, a);
            impl_->keypadToggleRect = Rect(screenW / 2 - 82, impl_->fieldRect.bottom() + 18,
                                           164, 32);
            drawTextCentered(comp, text, "Use PIN keypad", touch ? 14 : 13, Weight::Medium,
                             rgb(0xFFFFFF, 0.72f), impl_->keypadToggleRect, a);
        }

        impl_->cancelRect = Rect(screenW / 2 - 60, screenH - (touch ? 72 : 64), 120, 30);
        if (impl_->mode == Mode::Lock) {
            drawTextCentered(comp, text, "Cancel", touch ? 14 : 13, Weight::Medium, muted,
                             impl_->cancelRect, a);
        } else {
            impl_->cancelRect = Rect{};
        }

        // Home indicator is still visible on the passcode layer.
        const int hw = std::min(180, int(screenW * 0.34f));
        comp.drawRect(Rect((screenW - hw) / 2, screenH - 16, hw, 5), 2.5f,
                      rgb(0xFFFFFF, 0.82f), a);
    }

    if (impl_->powerMenu) {
        comp.drawRect(Rect(0, 0, screenW, screenH), 0.0f, rgb(0x000000, 0.48f), a);
        const int menuW = std::min(screenW - 2 * pad, 320);
        const int menuH = 236;
        const int mx = (screenW - menuW) / 2;
        const int my = (screenH - menuH) / 2;
        comp.drawRect(Rect(mx, my, menuW, menuH), 28.0f, rgb(0x1C1C1E, 0.96f), a);
        drawTextCentered(comp, text, "Power", 17, Weight::Medium, white,
                         Rect(mx, my + 18, menuW, 26), a);
        const char* labels[3] = {"Sleep", "Restart", "Shut Down"};
        for (int i = 0; i < 3; ++i) {
            const Rect r(mx + 16, my + 54 + i * 56, menuW - 32, 48);
            impl_->powerRects[i] = r;
            if (impl_->hoverPower == i)
                comp.drawRect(r, 14.0f, rgb(0xFFFFFF, 0.08f), a);
            drawTextLeft(comp, text, labels[i], 15, Weight::Regular, white, r.x + 16,
                         r.y + r.h / 2, a);
        }
    } else {
        for (Rect& r : impl_->powerRects) r = Rect{};
    }
}

void requestSessionLock() { runDetached("loginctl lock-session"); }

}  // namespace wm
