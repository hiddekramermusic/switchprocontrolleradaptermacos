// procon_init.cpp
// Waits for a Pro Controller (057e:2009) on USB, sends the console-style
// init sequence (80 02, 80 03, 80 02, 80 04), then monitors until the device
// drops. Repeats for every plug-in. Ctrl+C to quit.
//
// Diagnostic flags:
//   --exclusive   open with kIOHIDOptionsTypeSeizeDevice (macOS only)
//   --no-init     send nothing, only monitor input
//   --seq 02,03   send these 80 xx commands instead of the default sequence
//                 (an ack is expected for every command except 04)
//   --player N    after the sequence, set player light N (1-4) with subcommand 0x30
//
// macOS:  brew install hidapi
//         clang++ -std=c++20 -O2 procon_init.cpp $(pkg-config --cflags --libs hidapi) -o procon_init
// Linux:  sudo apt install libhidapi-dev
//         g++ -std=c++20 -O2 procon_init.cpp $(pkg-config --cflags --libs hidapi-hidraw) -o procon_init

#include <hidapi.h>
#ifdef __APPLE__
#include <hidapi_darwin.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdio>
#include <thread>

namespace {

constexpr unsigned short kVendorId = 0x057e;
constexpr unsigned short kProductId = 0x2009;
constexpr int kReadTimeoutMs = 100;
constexpr int kAckTimeoutMs = 300;
constexpr int kPollIntervalMs = 50;

constexpr long kLoggedReports = 20;

volatile std::sig_atomic_t g_stop = 0;

struct Step { unsigned char cmd; bool expectAck; };

struct Options {
    bool exclusive = false;
    bool init = true;
    int player = 0;  // 0 = leave the player lights alone
    std::vector<Step> sequence{
        {0x02, true},   // handshake
        {0x03, true},   // high-speed mode
        {0x02, true},   // handshake
        {0x04, false},  // force USB-only (no reply expected)
    };
};

// Parses "02,03,04" into steps. Returns false on malformed input.
bool parseSequence(const char* text, std::vector<Step>& out) {
    out.clear();
    const char* p = text;
    while (*p != '\0') {
        char* end = nullptr;
        const unsigned long v = std::strtoul(p, &end, 16);
        if (end == p || v > 0xff) return false;
        const auto cmd = static_cast<unsigned char>(v);
        out.push_back({cmd, cmd != 0x04});
        p = end;
        if (*p == ',') ++p;
        else if (*p != '\0') return false;
    }
    return !out.empty();
}

extern "C" void onSignal(int) { g_stop = 1; }

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

const wchar_t* errorText(hid_device* dev) {
    const wchar_t* e = hid_error(dev);
    return e != nullptr ? e : L"(no error text)";
}

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

enum class Result { Ack, NoAckExpected, Timeout, Lost };

const char* toString(Result r) {
    switch (r) {
        case Result::Ack:           return "ack";
        case Result::NoAckExpected: return "sent";
        case Result::Timeout:       return "no ack (timeout)";
        case Result::Lost:          return "device lost";
    }
    return "?";
}

// Sends 80 <cmd>. If an ack is expected, waits for an 81 <cmd> reply,
// skipping any other input reports that arrive in between.
void logReport(Clock::time_point start, const unsigned char* buf, int n) {
    std::printf("[%6.3f s]   in  id=%02x len=%2d :", secondsSince(start), buf[0], n);
    for (int i = 1; i < n && i < 12; ++i) std::printf(" %02x", buf[i]);
    std::printf("\n");
}

Result sendCommand(Clock::time_point start, hid_device* dev, unsigned char cmd, bool expectAck) {
    const unsigned char report[2]{0x80, cmd};
    if (hid_write(dev, report, sizeof report) < 0) {
        std::fprintf(stderr, "  write 80 %02x failed: %ls\n", cmd, errorText(dev));
        return Result::Lost;
    }
    if (!expectAck) return Result::NoAckExpected;

    const auto deadline = Clock::now() + std::chrono::milliseconds(kAckTimeoutMs);
    while (Clock::now() < deadline && g_stop == 0) {
        unsigned char buf[64]{};
        const int n = hid_read_timeout(dev, buf, sizeof buf, kReadTimeoutMs);
        if (n < 0) return Result::Lost;
        if (n > 0) logReport(start, buf, n);
        if (n >= 2 && buf[0] == 0x81 && buf[1] == cmd) return Result::Ack;
    }
    return Result::Timeout;
}

// Sends subcommand 0x30 (set player lights) in output report 0x01 with neutral
// rumble data, and waits for the 0x21 reply that echoes subcommand 0x30.
Result setPlayerLights(Clock::time_point start, hid_device* dev, unsigned char mask) {
    unsigned char report[12]{0x01, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
                             0x30, mask};
    if (hid_write(dev, report, sizeof report) < 0) {
        std::fprintf(stderr, "  write 01 .. 30 %02x failed: %ls\n", mask, errorText(dev));
        return Result::Lost;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(kAckTimeoutMs);
    while (Clock::now() < deadline && g_stop == 0) {
        unsigned char buf[64]{};
        const int n = hid_read_timeout(dev, buf, sizeof buf, kReadTimeoutMs);
        if (n < 0) return Result::Lost;
        if (n >= 15 && buf[0] == 0x21 && buf[14] == 0x30) {
            logReport(start, buf, n);
            return Result::Ack;
        }
    }
    return Result::Timeout;
}

void runSession(hid_device* dev, const Options& opt) {
    const auto start = Clock::now();
    std::printf("[%6.3f s] connected\n", 0.0);

    for (const Step& s : opt.sequence) {
        if (!opt.init) break;
        if (g_stop != 0) return;
        const Result r = sendCommand(start, dev, s.cmd, s.expectAck);
        std::printf("[%6.3f s] 80 %02x -> %s\n", secondsSince(start), s.cmd, toString(r));
        if (r == Result::Lost) return;
    }

    if (opt.player != 0 && g_stop == 0) {
        const auto mask = static_cast<unsigned char>(1u << (opt.player - 1));
        const Result r = setPlayerLights(start, dev, mask);
        std::printf("[%6.3f s] player lights %02x -> %s\n", secondsSince(start), mask, toString(r));
        if (r == Result::Lost) return;
    }

    std::printf("[%6.3f s] sequence done, monitoring input\n", secondsSince(start));
    std::fflush(stdout);

    long reports = 0;
    auto lastStatus = Clock::now();
    while (g_stop == 0) {
        unsigned char buf[64]{};
        const int n = hid_read_timeout(dev, buf, sizeof buf, kReadTimeoutMs);
        if (n < 0) {
            std::printf("[%6.3f s] device lost after %ld input reports\n",
                        secondsSince(start), reports);
            return;
        }
        if (n > 0 && ++reports <= kLoggedReports) logReport(start, buf, n);
        if (secondsSince(lastStatus) >= 1.0) {
            std::printf("[%6.3f s] %ld input reports\n", secondsSince(start), reports);
            std::fflush(stdout);
            lastStatus = Clock::now();
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--exclusive") == 0) opt.exclusive = true;
        else if (std::strcmp(argv[i], "--no-init") == 0) opt.init = false;
        else if (std::strcmp(argv[i], "--seq") == 0 && i + 1 < argc &&
                 parseSequence(argv[i + 1], opt.sequence)) ++i;
        else if (std::strcmp(argv[i], "--player") == 0 && i + 1 < argc &&
                 argv[i + 1][0] >= '1' && argv[i + 1][0] <= '4' && argv[i + 1][1] == '\0')
            opt.player = argv[++i][0] - '0';
        else {
            std::fprintf(stderr, "usage: %s [--exclusive] [--no-init] [--seq 02,03,...] [--player 1-4]\n", argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (hid_init() != 0) {
        std::fprintf(stderr, "hid_init failed: %ls\n", errorText(nullptr));
        return 1;
    }
#ifdef __APPLE__
    // hidapi seizes devices on macOS by default; only do that when asked.
    hid_darwin_set_open_exclusive(opt.exclusive ? 1 : 0);
#endif

    while (g_stop == 0) {
        hid_device* dev = waitForDevice();
        if (dev == nullptr) break;
        runSession(dev, opt);
        hid_close(dev);
    }

    hid_exit();
    std::printf("Stopped.\n");
    return 0;
}
