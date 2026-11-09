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

    // Android-style lock surface: the clock/notification layer is shown first;
    // tapping, pressing Enter or swiping up raises the credential sheet.
    bool credentialSheet = false;
    bool keypad = true;
    // Unlock animation: the credential sheet slides down and the whole lock
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
            // A real swipe up, or a click/tap on desktop, raises the credential
            // sheet. Nothing authenticates on the first gesture, like Android.
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
    const Color muted = rgb(0xD7DEE8, 0.72f);
    const Color dim = rgb(0xFFFFFF, 0.55f);
    const Color accent = rgb(0xA8C7FA);
    const bool sheet = impl_->credentialSheet;
    // `tablet` is the shell's mode flag; a narrow window is touch-first even if
    // it was started without --tablet (the usual mobile.sh path).
    const bool touch = tablet || screenW < 640;

    comp.drawWallpaper();
    // Android's lock screen scrim: dark enough for white type at the top and
    // bottom, almost clear across the middle so the wallpaper still reads.
    comp.drawGradient(Rect(0, 0, screenW, screenH * 0.58f), 0.0f,
                      rgb(0x000000, 0.62f), rgb(0x000000, 0.0f), a);
    comp.drawGradient(Rect(0, screenH * 0.42f, screenW, screenH * 0.58f), 0.0f,
                      rgb(0x000000, 0.0f), rgb(0x000000, 0.72f), a);

    const int pad = touch ? 26 : std::max(40, screenW / 24);
    const int datePx = touch ? 15 : 14;
    const int top = touch ? 52 : std::max(38, screenH / 11);
    const int clockPx = touch ? std::clamp(screenW / 4, 68, 122)
                               : std::clamp(screenW / 11, 64, 104);
    const int clockPxNow = sheet ? int(clockPx * 0.78f) : clockPx;

    char clock[16] = {};
    char date[48] = {};
    const time_t t = time(nullptr);
    struct tm lt {};
    localtime_r(&t, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
    char dateBase[32] = {};
    std::strftime(dateBase, sizeof(dateBase), "%a, %b", &lt);
    std::snprintf(date, sizeof(date), "%s %d", dateBase, lt.tm_mday);

    drawTextLeft(comp, text, date, datePx, Weight::Medium, muted, pad, top + datePx / 2, a);
    const int clockY = top + datePx + 10;
    drawTextLeft(comp, text, clock, clockPxNow, Weight::Regular, white, pad,
                 clockY + clockPxNow / 2, a);

    const int clockBottom = clockY + clockPxNow;

    // One quiet notification card keeps the first screen from reading as a
    // bare clock without pretending to know about real notifications.
    if (!sheet && impl_->mode == Mode::Lock) {
        const int cardW = std::min(screenW - 2 * pad, 420);
        const int cardH = touch ? 68 : 64;
        int cardY = clockBottom + (touch ? 30 : 34);
        cardY = std::min(cardY, screenH - (touch ? 250 : 220) - cardH);
        const Rect card(pad, cardY, cardW, cardH);
        comp.drawRect(card, 22.0f, rgb(0x151A22, 0.58f), a);
        comp.drawRect(card, 22.0f, rgb(0xFFFFFF, 0.08f), a);
        const int ic = touch ? 36 : 34;
        const Rect icon(card.x + 16, card.y + (cardH - ic) / 2, ic, ic);
        comp.drawRect(icon, float(ic) / 2.0f, rgb(0xA8C7FA, 0.20f), a);
        comp.drawArc(icon.x + ic / 2, icon.y + ic / 2, ic * 0.22f, 2.0f,
                     0.0f, 6.2831853f, accent, a * 0.85f);
        const int tx = icon.x + ic + 14;
        drawTextLeft(comp, text, "No notifications", touch ? 15 : 14, Weight::Medium,
                     white, tx, card.y + cardH / 2 - 9, a);
        drawTextLeft(comp, text, "You're all caught up", touch ? 12 : 12, Weight::Regular,
                     muted, tx, card.y + cardH / 2 + 10, a);
    }

    if (!sheet) {
    // Bottom shortcut buttons, the Android lock-screen flashlight/camera pair.
    const int shortcut = touch ? 54 : 46;
    const int shortcutY = screenH - (touch ? 54 : 42) - shortcut;
    auto shortcutCircle = [&](int x) {
        const Rect r(x, shortcutY, shortcut, shortcut);
        comp.drawRect(r, float(shortcut) / 2.0f, rgb(0xFFFFFF, 0.13f), a);
        comp.drawRect(r, float(shortcut) / 2.0f, rgb(0xFFFFFF, 0.10f), a);
        return r;
    };
    const Rect flash = shortcutCircle(pad);
    const Rect camera = shortcutCircle(screenW - pad - shortcut);

    // Flashlight: a simple torch body and beam cone.
    const int fcx = flash.x + flash.w / 2;
    const int fcy = flash.y + flash.h / 2;
    comp.drawRect(Rect(fcx - 4, fcy - 9, 8, 15), 2.5f, white, a);
    comp.drawRect(Rect(fcx - 7, fcy - 12, 14, 5), 2.0f, white, a);
    comp.drawRect(Rect(fcx - 1, fcy + 7, 2, 4), 1.0f, rgb(0xFFFFFF, 0.7f), a);

    // Camera: body, lens ring and viewfinder bump.
    const int ccx = camera.x + camera.w / 2;
    const int ccy = camera.y + camera.h / 2;
    comp.drawRect(Rect(ccx - 11, ccy - 7, 22, 16), 4.0f, white, a);
    comp.drawRect(Rect(ccx - 5, ccy - 10, 10, 4), 1.5f, white, a);
    comp.drawArc(ccx, ccy + 1, 4.5f, 2.0f, 0.0f, 6.2831853f, rgb(0x1A1F28), a);
    }

    if (!sheet) {
        const int hintY = screenH - (touch ? 148 : 122);
        // Lock glyph.
        const int lx = screenW / 2;
        const int ly = hintY - 30;
        comp.drawRect(Rect(lx - 9, ly, 18, 13), 3.0f, white, a * 0.92f);
        comp.drawArc(lx, ly + 1, 6.5f, 2.2f, 4.7123890f, 1.5707963f, white, a * 0.92f);
        comp.drawRect(Rect(lx - 1, ly + 4, 2, 4), 1.0f, rgb(0x1A1F28), a);

        // Up chevron above the lock, then the Android hint.
        const int chY = ly - 20;
        comp.drawRectRotated(lx - 5, chY, 12, 2.5f, -0.7f, 1.2f, white, a * 0.65f);
        comp.drawRectRotated(lx + 5, chY, 12, 2.5f, 0.7f, 1.2f, white, a * 0.65f);
        drawTextCentered(comp, text,
                         touch ? "Swipe up to unlock" : "Click or press Enter to unlock",
                         touch ? 14 : 13, Weight::Medium, white,
                         Rect(0, hintY + 6, screenW, 22), a * 0.90f);
    }

    // Android's credential layer: a bottom sheet with the user identity, a
    // rounded PIN/password field and an accent unlock button.
    if (sheet) {
        const int panelH = touch ? std::min(int(screenH * 0.64f), 540)
                                  : std::min(int(screenH * 0.62f), 500);
        // Slide in from the bottom on open, then slide back down on unlock.
        const int openSlide = int((1.0f - open) * panelH * 0.12f);
        const int closeSlide = int(close * (panelH + 80));
        const int panelY = screenH - panelH + openSlide + closeSlide;
        comp.drawRect(Rect(0, panelY, screenW, panelH), 28.0f, rgb(0x11161F, 0.94f), a);
        comp.drawRect(Rect(0, panelY, screenW, 1), 0.0f, rgb(0xFFFFFF, 0.08f), a);
        comp.drawRect(Rect(screenW / 2 - 18, panelY + 10, 36, 4), 2.0f,
                      rgb(0xFFFFFF, 0.22f), a);

        int y = panelY + 32;
        const Impl::User* selected = impl_->selected();
        impl_->userRects.clear();
        if (impl_->mode == Mode::Login && impl_->users.size() > 1) {
            const int chip = touch ? 38 : 34;
            const int gap = 9;
            const int maxChips = std::min<int>(5, int(impl_->users.size()));
            const int total = maxChips * chip + (maxChips - 1) * gap;
            int x = (screenW - total) / 2;
            for (int i = 0; i < maxChips; ++i) {
                const Rect r(x, y, chip, chip);
                const bool on = i == impl_->userIndex;
                comp.drawRect(r, float(chip) / 2.0f,
                              on ? accent : rgb(0xFFFFFF, 0.10f), a);
                const std::string initial =
                    impl_->users[size_t(i)].display.empty()
                        ? "?"
                        : impl_->users[size_t(i)].display.substr(0, 1);
                drawTextCentered(comp, text, initial, std::max(12, chip / 2), Weight::Bold,
                                 on ? rgb(0x0A1628) : white, r, a);
                impl_->userRects.push_back(r);
                x += chip + gap;
            }
            y += chip + 18;
        }

        const int av = touch ? 56 : 48;
        const std::string name = selected ? selected->display : "User";
        const TextTex nt = text.get(name, touch ? 20 : 18, Weight::Medium);
        const int groupW = av + 12 + (nt.tex ? nt.w : 0);
        const int gx = (screenW - groupW) / 2;
        const Rect avatarRect(gx, y, av, av);
        if (selected && !selected->avatar.empty()) {
            const IconTex ic = icons.get(selected->avatar, av, IconTheme::Hatter);
            if (ic.valid())
                comp.drawTex(ic.tex, avatarRect, float(av) / 2.0f, white, a, true, true);
            else
                comp.drawRect(avatarRect, float(av) / 2.0f, accent, a);
        } else {
            comp.drawRect(avatarRect, float(av) / 2.0f, rgb(0xA8C7FA, 0.85f), a);
            const std::string initial = selected && !selected->display.empty()
                                            ? selected->display.substr(0, 1)
                                            : "?";
            drawTextCentered(comp, text, initial, av / 2, Weight::Bold, rgb(0x0A1628),
                             avatarRect, a);
        }
        drawTextLeft(comp, text, name, touch ? 20 : 18, Weight::Medium, white,
                     avatarRect.right() + 12, avatarRect.y + av / 2, a);
        y += av + 22;

        const int fieldW = std::min(screenW - 2 * pad, 420);
        impl_->fieldRect = Rect((screenW - fieldW) / 2, y, fieldW, touch ? 56 : 52);
        comp.drawRect(impl_->fieldRect, 15.0f, rgb(0x2A303B, 0.92f), a);
        comp.drawRect(impl_->fieldRect, 15.0f,
                      impl_->error.empty() ? rgb(0xFFFFFF, 0.10f) : rgb(0xFF8A8A, 0.72f), a);

        if (impl_->password.empty()) {
            drawTextLeft(comp, text, "PIN or password", touch ? 16 : 15, Weight::Regular,
                         dim, impl_->fieldRect.x + 20, impl_->fieldRect.y + impl_->fieldRect.h / 2,
                         a);
        } else {
            const int count = std::min<int>(24, int(impl_->password.size()));
            const int dot = touch ? 8 : 7;
            const int gap = touch ? 15 : 13;
            int dx = impl_->fieldRect.x + 20;
            const int dy = impl_->fieldRect.y + impl_->fieldRect.h / 2;
            for (int i = 0; i < count; ++i) {
                comp.drawRect(Rect(dx, dy - dot / 2, dot, dot), float(dot) / 2.0f,
                              white, a);
                dx += gap;
            }
        }

        impl_->unlockRect = Rect(impl_->fieldRect.right() - 44, impl_->fieldRect.y + 4, 44, 44);
        comp.drawRect(impl_->unlockRect, 22.0f, accent, a);
        drawTextCentered(comp, text, ">", 22, Weight::Bold, rgb(0x0A1628),
                         impl_->unlockRect, a);
        y = impl_->fieldRect.bottom() + 10;

        if (!impl_->error.empty()) {
            drawTextCentered(comp, text, impl_->error, touch ? 14 : 13, Weight::Regular,
                             rgb(0xFF8A8A), Rect(pad, y, screenW - 2 * pad, 22), a);
            y += 22;
        }

        for (Rect& r : impl_->keyRects) r = Rect{};
        impl_->keypadToggleRect = Rect{};
        if (impl_->keypad) {
            const int key = (touch ? 54 : 50) - (impl_->error.empty() ? 0 : 6);
            const int gap = touch ? 6 : 8;
            const int total = key * 3 + gap * 2;
            const int kx = (screenW - total) / 2;
            const int ky = y + 4;
            const char* labels[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9",
                                      "ABC", "0", "<"};
            for (int i = 0; i < 12; ++i) {
                const int row = i / 3;
                const int col = i % 3;
                const Rect r(kx + col * (key + gap), ky + row * (key + gap), key, key);
                impl_->keyRects[i] = r;
                comp.drawRect(r, float(key) / 2.0f, rgb(0xFFFFFF, 0.08f), a);
                drawTextCentered(comp, text, labels[i], i == 9 ? 12 : 20,
                                 i == 9 ? Weight::Medium : Weight::Regular, white, r, a);
            }
        } else {
            impl_->keypadToggleRect = Rect(screenW / 2 - 80, y + 4, 160, 34);
            comp.drawRect(impl_->keypadToggleRect, 17.0f, rgb(0xFFFFFF, 0.08f), a);
            drawTextCentered(comp, text, "Use PIN keypad", touch ? 14 : 13, Weight::Medium,
                             white, impl_->keypadToggleRect, a);
        }

        impl_->cancelRect = Rect(screenW / 2 - 50, panelY + panelH - 54, 100, 28);
        if (impl_->mode == Mode::Lock) {
            drawTextCentered(comp, text, "Cancel", touch ? 14 : 13, Weight::Medium, muted,
                             impl_->cancelRect, a);
        } else {
            impl_->cancelRect = Rect{};
        }

        const std::string hint = impl_->mode == Mode::Login
                                     ? "Enter  Sign in"
                                     : "Enter  Unlock     Esc  Back";
        drawTextCentered(comp, text, hint, touch ? 12 : 11, Weight::Regular, dim,
                         Rect(pad, panelY + panelH - 28, screenW - 2 * pad, 18), a);
    }

    // Power button, small and out of the way at the top-right. Tapping it opens
    // the Android power menu; there is no visible row of power text on the lock
    // screen itself.
    if (!sheet) {
        const int pb = touch ? 38 : 34;
        impl_->powerButton = Rect(screenW - pad - pb, top - 10, pb, pb);
        comp.drawRect(impl_->powerButton, float(pb) / 2.0f, rgb(0xFFFFFF, 0.10f), a);
        const int px = impl_->powerButton.x + pb / 2;
        const int py = impl_->powerButton.y + pb / 2;
        comp.drawArc(px, py, 7.0f, 2.0f, 0.7853982f, 5.4977871f, white, a * 0.85f);
        comp.drawRect(Rect(px - 1, py - 10, 2, 9), 1.0f, white, a * 0.85f);
    } else {
        impl_->powerButton = Rect{};
    }

    if (impl_->powerMenu) {
        comp.drawRect(Rect(0, 0, screenW, screenH), 0.0f, rgb(0x000000, 0.45f), a);
        const int menuW = std::min(screenW - 2 * pad, 340);
        const int menuH = 250;
        const int mx = (screenW - menuW) / 2;
        const int my = (screenH - menuH) / 2;
        comp.drawRect(Rect(mx, my, menuW, menuH), 26.0f, rgb(0x171C25, 0.96f), a);
        drawTextCentered(comp, text, "Power", 18, Weight::Medium, white,
                         Rect(mx, my + 18, menuW, 28), a);
        const char* labels[3] = {"Sleep", "Restart", "Shut down"};
        for (int i = 0; i < 3; ++i) {
            const Rect r(mx + 18, my + 58 + i * 56, menuW - 36, 48);
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
