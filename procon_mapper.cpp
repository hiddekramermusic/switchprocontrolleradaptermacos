// procon_mapper.cpp
// Holds a USB Pro Controller (057e:2009) exclusively, sends only the 80 02
// handshake (80 03 makes the controller stop responding on USB), sets player
// light 1, and turns its input reports into macOS keyboard and mouse events.
// Mappings are read from a config file (default: procon_mapper.conf).
//
// Posting events needs the Accessibility permission for the app that runs this
// binary (e.g. Terminal): System Settings > Privacy & Security > Accessibility.
//
// macOS:  brew install hidapi
//         clang++ -std=c++20 -O2 procon_mapper.cpp $(pkg-config --cflags --libs hidapi) \
//             -framework ApplicationServices -framework CoreFoundation -o procon_mapper

#include <hidapi.h>
#include <hidapi_darwin.h>

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>  // kVK_* virtual key codes

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr unsigned short kVendorId = 0x057e;
constexpr unsigned short kProductId = 0x2009;
constexpr int kReadTimeoutMs = 100;
constexpr int kAckTimeoutMs = 300;
constexpr int kPollIntervalMs = 50;
constexpr double kStickCenter = 2048.0;  // 12-bit stick values rest around 0x800
constexpr double kStickRange = 1400.0;   // nominal deflection from center

volatile std::sig_atomic_t g_stop = 0;

extern "C" void onSignal(int) { g_stop = 1; }

using Clock = std::chrono::steady_clock;

const wchar_t* errorText(hid_device* dev) {
    const wchar_t* e = hid_error(dev);
    return e != nullptr ? e : L"(no error text)";
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------
// Controller input (report 0x30, byte offsets include the report ID at [0])

struct Button { const char* name; int byte; unsigned char mask; };

constexpr Button kButtons[] = {
    {"y", 3, 0x01},     {"x", 3, 0x02},      {"b", 3, 0x04},     {"a", 3, 0x08},
    {"r", 3, 0x40},     {"zr", 3, 0x80},
    {"minus", 4, 0x01}, {"plus", 4, 0x02},   {"rstick", 4, 0x04}, {"lstick", 4, 0x08},
    {"home", 4, 0x10},  {"capture", 4, 0x20},
    {"down", 5, 0x01},  {"up", 5, 0x02},     {"right", 5, 0x04}, {"left", 5, 0x08},
    {"l", 5, 0x40},     {"zl", 5, 0x80},
};
constexpr int kButtonCount = sizeof kButtons / sizeof kButtons[0];

struct Stick { double x = 0, y = 0; };  // -1..1, y positive = up

Stick decodeStick(const unsigned char* p) {
    const int rawX = p[0] | ((p[1] & 0x0F) << 8);
    const int rawY = (p[1] >> 4) | (p[2] << 4);
    Stick s;
    s.x = std::clamp((rawX - kStickCenter) / kStickRange, -1.0, 1.0);
    s.y = std::clamp((rawY - kStickCenter) / kStickRange, -1.0, 1.0);
    return s;
}

// Radial deadzone, then a squared response curve for finer control near center.
Stick shapeStick(Stick s, double deadzone) {
    const double mag = std::hypot(s.x, s.y);
    if (mag <= deadzone) return {};
    const double scaled = std::min(1.0, (mag - deadzone) / (1.0 - deadzone));
    const double k = scaled * scaled / mag;
    return {s.x * k, s.y * k};
}

// ---------------------------------------------------------------------------
// Actions and config

enum class ActionKind { None, Mouse, Key };
enum class StickMode { None, Mouse, Scroll, Keys };

struct Action {
    ActionKind kind = ActionKind::None;
    CGMouseButton mouseButton = kCGMouseButtonLeft;
    std::vector<CGKeyCode> modifiers;
    CGKeyCode key = 0;
};

// Direction indices for StickConfig::keys and stick key state.
enum Direction { kUp, kLeft, kDown, kRight, kDirectionCount };

struct StickConfig {
    StickMode mode = StickMode::None;
    CGKeyCode keys[kDirectionCount]{};  // used when mode == Keys
};

struct Config {
    Action buttons[kButtonCount];
    StickConfig leftStick{StickMode::Mouse};
    StickConfig rightStick{StickMode::Scroll};
    double mouseSpeed = 1200;   // pixels per second at full deflection
    double scrollSpeed = 800;   // pixels per second at full deflection
    double deadzone = 0.15;
    double keyThreshold = 0.5;  // deflection at which a stick presses its keys
};

const std::map<std::string, CGKeyCode>& keyNames() {
    static const std::map<std::string, CGKeyCode> names = [] {
        std::map<std::string, CGKeyCode> m{
            {"a", kVK_ANSI_A}, {"b", kVK_ANSI_B}, {"c", kVK_ANSI_C}, {"d", kVK_ANSI_D},
            {"e", kVK_ANSI_E}, {"f", kVK_ANSI_F}, {"g", kVK_ANSI_G}, {"h", kVK_ANSI_H},
            {"i", kVK_ANSI_I}, {"j", kVK_ANSI_J}, {"k", kVK_ANSI_K}, {"l", kVK_ANSI_L},
            {"m", kVK_ANSI_M}, {"n", kVK_ANSI_N}, {"o", kVK_ANSI_O}, {"p", kVK_ANSI_P},
            {"q", kVK_ANSI_Q}, {"r", kVK_ANSI_R}, {"s", kVK_ANSI_S}, {"t", kVK_ANSI_T},
            {"u", kVK_ANSI_U}, {"v", kVK_ANSI_V}, {"w", kVK_ANSI_W}, {"x", kVK_ANSI_X},
            {"y", kVK_ANSI_Y}, {"z", kVK_ANSI_Z},
            {"0", kVK_ANSI_0}, {"1", kVK_ANSI_1}, {"2", kVK_ANSI_2}, {"3", kVK_ANSI_3},
            {"4", kVK_ANSI_4}, {"5", kVK_ANSI_5}, {"6", kVK_ANSI_6}, {"7", kVK_ANSI_7},
            {"8", kVK_ANSI_8}, {"9", kVK_ANSI_9},
            {"minus", kVK_ANSI_Minus}, {"equal", kVK_ANSI_Equal},
            {"leftbracket", kVK_ANSI_LeftBracket}, {"rightbracket", kVK_ANSI_RightBracket},
            {"semicolon", kVK_ANSI_Semicolon}, {"quote", kVK_ANSI_Quote},
            {"comma", kVK_ANSI_Comma}, {"period", kVK_ANSI_Period}, {"slash", kVK_ANSI_Slash},
            {"backslash", kVK_ANSI_Backslash}, {"grave", kVK_ANSI_Grave},
            {"space", kVK_Space}, {"return", kVK_Return}, {"enter", kVK_Return},
            {"tab", kVK_Tab}, {"escape", kVK_Escape}, {"esc", kVK_Escape},
            {"delete", kVK_Delete}, {"forwarddelete", kVK_ForwardDelete},
            {"up", kVK_UpArrow}, {"down", kVK_DownArrow},
            {"left", kVK_LeftArrow}, {"right", kVK_RightArrow},
            {"home", kVK_Home}, {"end", kVK_End}, {"pageup", kVK_PageUp}, {"pagedown", kVK_PageDown},
            {"f1", kVK_F1}, {"f2", kVK_F2}, {"f3", kVK_F3}, {"f4", kVK_F4},
            {"f5", kVK_F5}, {"f6", kVK_F6}, {"f7", kVK_F7}, {"f8", kVK_F8},
            {"f9", kVK_F9}, {"f10", kVK_F10}, {"f11", kVK_F11}, {"f12", kVK_F12},
            {"shift", kVK_Shift}, {"ctrl", kVK_Control}, {"option", kVK_Option},
            {"alt", kVK_Option}, {"cmd", kVK_Command},
        };
        return m;
    }();
    return names;
}

// Event flag for a modifier key, or 0 for any other key.
CGEventFlags modifierFlag(CGKeyCode key) {
    switch (key) {
        case kVK_Command: return kCGEventFlagMaskCommand;
        case kVK_Shift:   return kCGEventFlagMaskShift;
        case kVK_Control: return kCGEventFlagMaskControl;
        case kVK_Option:  return kCGEventFlagMaskAlternate;
        default:          return 0;
    }
}

bool parseKey(const std::string& name, CGKeyCode& out, std::string& err) {
    const auto k = keyNames().find(name);
    if (k == keyNames().end()) { err = "unknown key '" + name + "'"; return false; }
    out = k->second;
    return true;
}

// Parses "key cmd+shift+tab", "mouse left" or "none".
bool parseAction(const std::string& text, Action& out, std::string& err) {
    std::istringstream in(text);
    std::string kind, arg;
    in >> kind >> arg;
    kind = lower(kind);
    arg = lower(arg);
    out = Action{};
    if (kind == "none") return true;
    if (kind == "mouse") {
        out.kind = ActionKind::Mouse;
        if (arg == "left") out.mouseButton = kCGMouseButtonLeft;
        else if (arg == "right") out.mouseButton = kCGMouseButtonRight;
        else if (arg == "middle") out.mouseButton = kCGMouseButtonCenter;
        else { err = "unknown mouse button '" + arg + "'"; return false; }
        return true;
    }
    if (kind == "key") {
        out.kind = ActionKind::Key;
        std::vector<std::string> parts;
        std::istringstream combo(arg);
        for (std::string part; std::getline(combo, part, '+');) parts.push_back(part);
        if (parts.empty()) { err = "missing key name"; return false; }
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            CGKeyCode m = 0;
            if (!parseKey(parts[i], m, err) || modifierFlag(m) == 0) {
                err = "unknown modifier '" + parts[i] + "'";
                return false;
            }
            out.modifiers.push_back(m);
        }
        return parseKey(parts.back(), out.key, err);
    }
    err = "unknown action '" + kind + "' (use key, mouse or none)";
    return false;
}

// Parses "mouse", "scroll", "none", "wasd", "arrows" or "keys <up> <left> <down> <right>".
bool parseStick(const std::string& text, StickConfig& out, std::string& err) {
    std::istringstream in(lower(text));
    std::string mode;
    in >> mode;
    out = StickConfig{};
    if (mode == "mouse") out.mode = StickMode::Mouse;
    else if (mode == "scroll") out.mode = StickMode::Scroll;
    else if (mode == "none") out.mode = StickMode::None;
    else if (mode == "wasd") out = {StickMode::Keys, {kVK_ANSI_W, kVK_ANSI_A, kVK_ANSI_S, kVK_ANSI_D}};
    else if (mode == "arrows") out = {StickMode::Keys, {kVK_UpArrow, kVK_LeftArrow, kVK_DownArrow, kVK_RightArrow}};
    else if (mode == "keys") {
        out.mode = StickMode::Keys;
        for (CGKeyCode& key : out.keys) {
            std::string name;
            if (!(in >> name)) { err = "keys needs four keys: up left down right"; return false; }
            if (!parseKey(name, key, err)) return false;
        }
    } else {
        err = "stick mode must be mouse, scroll, wasd, arrows, keys <up> <left> <down> <right> or none";
        return false;
    }
    std::string extra;
    if (in >> extra) { err = "unexpected '" + extra + "'"; return false; }
    return true;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

bool loadConfig(const char* path, Config& cfg) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "cannot open config %s\n", path);
        return false;
    }
    int lineNo = 0;
    for (std::string line; std::getline(file, line);) {
        ++lineNo;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "%s:%d: expected 'name = value'\n", path, lineNo);
            return false;
        }
        const std::string name = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        std::string err;
        bool ok = true;
        if (name == "left_stick") ok = parseStick(value, cfg.leftStick, err);
        else if (name == "right_stick") ok = parseStick(value, cfg.rightStick, err);
        else if (name == "stick_key_threshold")
            cfg.keyThreshold = std::clamp(std::atof(value.c_str()), 0.1, 0.95);
        else if (name == "mouse_speed") cfg.mouseSpeed = std::atof(value.c_str());
        else if (name == "scroll_speed") cfg.scrollSpeed = std::atof(value.c_str());
        else if (name == "deadzone") cfg.deadzone = std::clamp(std::atof(value.c_str()), 0.0, 0.9);
        else {
            int i = 0;
            while (i < kButtonCount && name != kButtons[i].name) ++i;
            if (i == kButtonCount) err = "unknown control '" + name + "'";
            else ok = parseAction(value, cfg.buttons[i], err);
        }
        if (!ok || !err.empty()) {
            std::fprintf(stderr, "%s:%d: %s\n", path, lineNo, err.c_str());
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Event output

class Output {
public:
    Output() : source_(CGEventSourceCreate(kCGEventSourceStateHIDSystemState)) {}
    ~Output() {
        releaseAll();
        if (source_ != nullptr) CFRelease(source_);
    }

    void press(const Action& a, bool down) {
        if (a.kind == ActionKind::Mouse) {
            mouseDown_[a.mouseButton] = down;
            postMouse(mouseButtonEvent(a.mouseButton, down), a.mouseButton, cursor(), 1);
        } else if (a.kind == ActionKind::Key) {
            if (down) {
                for (CGKeyCode m : a.modifiers) key(m, true);
                key(a.key, true);
            } else {
                key(a.key, false);
                for (auto m = a.modifiers.rbegin(); m != a.modifiers.rend(); ++m) key(*m, false);
            }
        }
    }

    // Presses or releases a key. Keys are counted, so a key held by two
    // controls is only released when both let go.
    void key(CGKeyCode k, bool down) {
        int& count = held_[k];
        if (down && ++count == 1) postKey(k, true);
        else if (!down && count > 0 && --count == 0) postKey(k, false);
    }

    void moveMouse(double dx, double dy) {
        accX_ += dx;
        accY_ += dy;
        const double stepX = std::trunc(accX_), stepY = std::trunc(accY_);
        if (stepX == 0 && stepY == 0) return;
        accX_ -= stepX;
        accY_ -= stepY;
        CGPoint p = cursor();
        p.x += stepX;
        p.y += stepY;
        p = clampToDisplays(p);
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (mouseDown_[kCGMouseButtonLeft]) type = kCGEventLeftMouseDragged;
        else if (mouseDown_[kCGMouseButtonRight]) { type = kCGEventRightMouseDragged; button = kCGMouseButtonRight; }
        else if (mouseDown_[kCGMouseButtonCenter]) { type = kCGEventOtherMouseDragged; button = kCGMouseButtonCenter; }
        CGEventRef e = CGEventCreateMouseEvent(source_, type, p, button);
        CGEventSetFlags(e, flags_);
        CGEventSetIntegerValueField(e, kCGMouseEventDeltaX, static_cast<int64_t>(stepX));
        CGEventSetIntegerValueField(e, kCGMouseEventDeltaY, static_cast<int64_t>(stepY));
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    void scroll(double dx, double dy) {
        scrollX_ += dx;
        scrollY_ += dy;
        const double stepX = std::trunc(scrollX_), stepY = std::trunc(scrollY_);
        if (stepX == 0 && stepY == 0) return;
        scrollX_ -= stepX;
        scrollY_ -= stepY;
        CGEventRef e = CGEventCreateScrollWheelEvent2(source_, kCGScrollEventUnitPixel, 2,
                                                      static_cast<int32_t>(stepY),
                                                      static_cast<int32_t>(stepX), 0);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    // Lifts every mouse button and key that is still down.
    void releaseAll() {
        for (auto& [button, down] : mouseDown_) {
            if (down) postMouse(mouseButtonEvent(button, false), button, cursor(), 1);
            down = false;
        }
        for (auto& [k, count] : held_) {
            if (count > 0) postKey(k, false);
            count = 0;
        }
    }

private:
    static CGEventType mouseButtonEvent(CGMouseButton b, bool down) {
        switch (b) {
            case kCGMouseButtonLeft:  return down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
            case kCGMouseButtonRight: return down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
            default:                  return down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        }
    }

    static CGPoint cursor() {
        CGEventRef e = CGEventCreate(nullptr);
        const CGPoint p = CGEventGetLocation(e);
        CFRelease(e);
        return p;
    }

    static CGPoint clampToDisplays(CGPoint p) {
        CGDirectDisplayID ids[16];
        uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess || count == 0) return p;
        CGRect all = CGDisplayBounds(ids[0]);
        for (uint32_t i = 1; i < count; ++i) all = CGRectUnion(all, CGDisplayBounds(ids[i]));
        p.x = std::clamp(p.x, CGRectGetMinX(all), CGRectGetMaxX(all) - 1);
        p.y = std::clamp(p.y, CGRectGetMinY(all), CGRectGetMaxY(all) - 1);
        return p;
    }

    void postMouse(CGEventType type, CGMouseButton button, CGPoint p, int64_t clickState) {
        CGEventRef e = CGEventCreateMouseEvent(source_, type, p, button);
        CGEventSetFlags(e, flags_);
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, clickState);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    // Posts a key event carrying the modifiers currently held by this output.
    void postKey(CGKeyCode key, bool down) {
        if (down) flags_ |= modifierFlag(key);
        else flags_ &= ~modifierFlag(key);
        CGEventRef e = CGEventCreateKeyboardEvent(source_, key, down);
        CGEventSetFlags(e, flags_);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
    }

    CGEventSourceRef source_;
    std::map<CGMouseButton, bool> mouseDown_{
        {kCGMouseButtonLeft, false}, {kCGMouseButtonRight, false}, {kCGMouseButtonCenter, false}};
    std::map<CGKeyCode, int> held_;
    CGEventFlags flags_ = 0;
    double accX_ = 0, accY_ = 0, scrollX_ = 0, scrollY_ = 0;
};

// ---------------------------------------------------------------------------
// Device session

hid_device* waitForDevice() {
    std::printf("Waiting for %04x:%04x ...\n", kVendorId, kProductId);
    std::fflush(stdout);
    while (g_stop == 0) {
        hid_device* dev = hid_open(kVendorId, kProductId, nullptr);
        if (dev != nullptr) return dev;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    return nullptr;
}

// Writes a report and waits until accept(buf, n) returns true for an input report.
template <typename Accept>
bool request(hid_device* dev, const unsigned char* report, size_t size, Accept accept) {
    if (hid_write(dev, report, size) < 0) return false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(kAckTimeoutMs);
    while (Clock::now() < deadline && g_stop == 0) {
        unsigned char buf[64]{};
        const int n = hid_read_timeout(dev, buf, sizeof buf, kReadTimeoutMs);
        if (n < 0) return false;
        if (accept(buf, n)) return true;
    }
    return false;
}

bool handshake(hid_device* dev) {
    const unsigned char report[2]{0x80, 0x02};
    return request(dev, report, sizeof report,
                   [](const unsigned char* b, int n) { return n >= 2 && b[0] == 0x81 && b[1] == 0x02; });
}

// Subcommand 0x30 (set player lights) in output report 0x01 with neutral rumble data.
bool setPlayerLights(hid_device* dev, unsigned char mask) {
    const unsigned char report[12]{0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
                                   0x30, mask};
    return request(dev, report, sizeof report,
                   [](const unsigned char* b, int n) { return n >= 15 && b[0] == 0x21 && b[14] == 0x30; });
}

// Directions pressed for each of the 8 sectors, counter-clockwise from right.
constexpr Direction kSectorDirections[8][2] = {
    {kRight, kRight}, {kUp, kRight}, {kUp, kUp},     {kUp, kLeft},
    {kLeft, kLeft},   {kDown, kLeft}, {kDown, kDown}, {kDown, kRight},
};

// Presses one key for straight directions and two for diagonals. Once a key
// is down the stick has to drop 0.1 below the threshold to release it.
void applyStickKeys(Output& out, const StickConfig& sc, Stick raw, double threshold,
                    bool (&down)[kDirectionCount]) {
    bool want[kDirectionCount]{};
    const bool anyDown = std::find(std::begin(down), std::end(down), true) != std::end(down);
    if (std::hypot(raw.x, raw.y) > (anyDown ? threshold - 0.1 : threshold)) {
        const int sector = static_cast<int>(std::lround(std::atan2(raw.y, raw.x) / (M_PI / 4))) & 7;
        want[kSectorDirections[sector][0]] = true;
        want[kSectorDirections[sector][1]] = true;
    }
    for (int d = 0; d < kDirectionCount; ++d) {
        if (want[d] != down[d]) {
            down[d] = want[d];
            out.key(sc.keys[d], want[d]);
        }
    }
}

void applyStick(Output& out, const StickConfig& sc, Stick raw, const Config& cfg, double dt,
                bool (&keysDown)[kDirectionCount]) {
    const Stick s = shapeStick(raw, cfg.deadzone);
    switch (sc.mode) {
        case StickMode::Mouse:  out.moveMouse(s.x * cfg.mouseSpeed * dt, -s.y * cfg.mouseSpeed * dt); break;
        case StickMode::Scroll: out.scroll(-s.x * cfg.scrollSpeed * dt, s.y * cfg.scrollSpeed * dt); break;
        case StickMode::Keys:   applyStickKeys(out, sc, raw, cfg.keyThreshold, keysDown); break;
        case StickMode::None:   break;
    }
}

void runSession(hid_device* dev, const Config& cfg) {
    std::printf("connected\n");
    if (!handshake(dev)) {
        std::printf("no handshake ack (%ls)\n", errorText(dev));
        return;
    }
    std::printf("handshake ok, player lights %s\n", setPlayerLights(dev, 0x01) ? "set" : "not acknowledged");
    std::printf("mapping input, Ctrl+C to quit\n");
    std::fflush(stdout);

    Output out;
    bool pressed[kButtonCount]{};
    bool leftKeys[kDirectionCount]{}, rightKeys[kDirectionCount]{};
    auto last = Clock::now();
    while (g_stop == 0) {
        unsigned char buf[64]{};
        const int n = hid_read_timeout(dev, buf, sizeof buf, kReadTimeoutMs);
        if (n < 0) {
            std::printf("device lost (%ls)\n", errorText(dev));
            return;
        }
        if (n < 12 || buf[0] != 0x30) continue;

        const auto now = Clock::now();
        const double dt = std::min(0.05, std::chrono::duration<double>(now - last).count());
        last = now;

        for (int i = 0; i < kButtonCount; ++i) {
            const bool down = (buf[kButtons[i].byte] & kButtons[i].mask) != 0;
            if (down != pressed[i]) {
                pressed[i] = down;
                out.press(cfg.buttons[i], down);
            }
        }
        applyStick(out, cfg.leftStick, decodeStick(buf + 6), cfg, dt, leftKeys);
        applyStick(out, cfg.rightStick, decodeStick(buf + 9), cfg, dt, rightKeys);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [config file]\n", argv[0]);
        return 2;
    }
    Config cfg;
    if (!loadConfig(argc == 2 ? argv[1] : "procon_mapper.conf", cfg)) return 1;

    if (!CGPreflightPostEventAccess()) {
        CGRequestPostEventAccess();
        std::fprintf(stderr,
                     "No permission to post input events. Allow this app (e.g. Terminal) under\n"
                     "System Settings > Privacy & Security > Accessibility, then run again.\n");
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (hid_init() != 0) {
        std::fprintf(stderr, "hid_init failed: %ls\n", errorText(nullptr));
        return 1;
    }
    // Exclusive access keeps macOS's own driver from taking the controller down.
    hid_darwin_set_open_exclusive(1);

    while (g_stop == 0) {
        hid_device* dev = waitForDevice();
        if (dev == nullptr) break;
        runSession(dev, cfg);
        hid_close(dev);
    }

    hid_exit();
    std::printf("Stopped.\n");
    return 0;
}
