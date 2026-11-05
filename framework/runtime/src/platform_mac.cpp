// macOS host (Apple silicon): memory (mmap at fixed addresses), pthreads, an SDL3 window with a Metal layer for
// MoltenVK (VK_EXT_metal_surface), SDL3 gamepad + keyboard, SDL3 audio, a signal crash handler and main.
//
// Cocoa owns the main thread: main() starts the game (nx::boot) on its own thread and then runs the SDL event loop.
// Anything that touches the window (creating it, its Metal view) is done there; the game's threads ask for it and
// wait. Input is sampled on the main thread into a snapshot read_pad() copies.
//
// Data: NX_DATA_ROOT or games/<game>'s pc_data_root ("private/<game>"): analysis/{main,sdk}.bin, extract/romfs, user/.
// Settings: NX_<KEY> environment variables, as on Windows.
#include <dlfcn.h>
#include <pthread.h>
#include <cstdlib>
#include <mach/thread_switch.h>
#include <pthread/qos.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx {
void boot();
}

#ifdef NX_TITLE_LIVE
namespace tsuki_title {
void hotkey(int dir);  // title_bg.cpp: F5 / F6
}
#endif

namespace nx::plat {

namespace {

// Work for the main thread (window creation), run between event polls.
std::mutex g_main_lock;
std::deque<std::function<void()>> g_main_work;

void run_on_main(std::function<void()> fn) {
    if (pthread_main_np()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard lk(g_main_lock);
        g_main_work.push_back([&] {
            fn();
            std::lock_guard l2(m);
            done = true;
            cv.notify_one();
        });
    }
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return done; });
}

SDL_Window* g_window = nullptr;
SDL_MetalView g_view = nullptr;
void* g_layer = nullptr;  // CAMetalLayer*
std::atomic<bool> g_minimized{false};
std::atomic<uint32_t> g_pixel_w{0}, g_pixel_h{0};

std::mutex g_pad_lock;
PadState g_pad;

}  // namespace

// ---- memory ---------------------------------------------------------------------------------------------------

// No MAP_FIXED_NOREPLACE on Darwin: ask for the address as a hint and give it back if the kernel chose another.
bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {
        munmap(p, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize()) - 1;  // 16 KiB on Apple silicon
    uint64_t a = addr & ~page, e = (addr + size + page) & ~page;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

// The guest frees 4 KiB pages; Apple silicon pages are 16 KiB, and mprotect would take the neighbours of a partly
// freed page with it (SIGBUS in Wonder when nn::os's VAMM unmapped single pages). Only whole host pages are
// released; the freed part of a shared one is zeroed, which is what the guest would see if it maps it again.
void decommit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize());
    uint64_t lo = (addr + page - 1) & ~(page - 1), hi = (addr + size) & ~(page - 1);
    if (lo >= hi) {
        std::memset(reinterpret_cast<void*>(addr), 0, size);
        return;
    }
    if (addr < lo) std::memset(reinterpret_cast<void*>(addr), 0, lo - addr);
    if (hi < addr + size) std::memset(reinterpret_cast<void*>(hi), 0, addr + size - hi);
    madvise(reinterpret_cast<void*>(lo), hi - lo, MADV_FREE);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    // The game's threads (render, jobs, audio) are latency critical: default QoS lets macOS run them on the M1's
    // efficiency cores and stretch their sleeps and wakeups.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack_size + 0x3FFF) & ~size_t(0x3FFF));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) { _exit(code); }

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

// macOS sched_yield drops the caller to priority 0 for a while: measured 20-180 us per call on the M1, and Wonder's
// job system yields after every job item and in its barriers, so the main thread spent ~60% of its frame there and
// the game's governor fell to 30 fps (docs/nvn/DEBUG.md #44). thread_switch with no hint (2-4 us) gives the core to
// another ready thread without that penalty. NX_YIELD=sched: the old call.
void yield() {
    static const bool sched = [] {
        const char* e = std::getenv("NX_YIELD");
        return e && std::strcmp(e, "sched") == 0;
    }();
    if (sched)
        sched_yield();
    else
        thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
}
void set_thread_name(const char* name) { pthread_setname_np(name); }  // Darwin: the calling thread only

int64_t current_thread_id() {
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return int64_t(tid);
}

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }  // use Instruments

uint64_t monotonic_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? r : nx::game::kInfo.pc_data_root;
}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- window ---------------------------------------------------------------------------------------------------

bool headless() {
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void* create_window() {
    run_on_main([] {
        if (g_window) return;
        int w = 1280, h = 720;
        std::string r = setting("resolution");
        if (!r.empty()) std::sscanf(r.c_str(), "%dx%d", &w, &h);
        g_window = SDL_CreateWindow(nx::game::kInfo.title, w, h,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL |
                                        (headless() ? SDL_WINDOW_HIDDEN : 0));
        if (!g_window) {
            log_line(SDL_GetError());
            return;
        }
        if (!headless()) SDL_RaiseWindow(g_window);  // without an app bundle it does not come to the front
        g_view = SDL_Metal_CreateView(g_window);
        g_layer = SDL_Metal_GetLayer(g_view);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        g_pixel_w = uint32_t(pw), g_pixel_h = uint32_t(ph);
    });
    return g_layer;  // the Vulkan surface is made from the CAMetalLayer, from any thread
}

void release_window(void*) {}
uint64_t window_generation() { return 1; }
bool window_minimized() { return g_minimized; }

void window_size(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }
float default_render_scale() { return 1.0f; }

// ---- input: first SDL gamepad + keyboard (sampled on the main thread) -----------------------------------------

// Mouse (platform.h PointerState): written by the main thread's event loop, read by the game's threads.
static std::atomic<int32_t> g_mouse_x{0}, g_mouse_y{0}, g_mouse_wheel{0};
static std::atomic<uint32_t> g_mouse_buttons{0};
static std::atomic<uint32_t> g_mouse_downs[3];
static std::atomic<bool> g_focused{false};
static std::mutex g_mac_pad_lock;
static MacPad g_mac_pad;
static bool g_mac_pad_on = false;

void read_pointer(PointerState& p) {
    p.x = g_mouse_x;
    p.y = g_mouse_y;
    window_size(p.w, p.h);
    p.buttons = g_mouse_buttons;
    for (int k = 0; k < 3; k++) p.downs[k] = g_mouse_downs[k];
    p.wheel = g_mouse_wheel;
}

bool mac_pad(MacPad& out) {
    std::lock_guard lk(g_mac_pad_lock);
    if (g_mac_pad_on) out = g_mac_pad;
    return g_mac_pad_on;
}

static int scancode_of_vk(int vk) {
    if (vk >= 'A' && vk <= 'Z') return SDL_SCANCODE_A + (vk - 'A');
    if (vk >= '1' && vk <= '9') return SDL_SCANCODE_1 + (vk - '1');
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= 0x70 && vk <= 0x7B) return SDL_SCANCODE_F1 + (vk - 0x70);
    if (vk >= 0x7C && vk <= 0x87) return SDL_SCANCODE_F13 + (vk - 0x7C);
    if (vk >= 0x61 && vk <= 0x69) return SDL_SCANCODE_KP_1 + (vk - 0x61);
    switch (vk) {
    case 0x60: return SDL_SCANCODE_KP_0;
    case 0x20: return SDL_SCANCODE_SPACE;
    case 0x0D: return SDL_SCANCODE_RETURN;
    case 0x1B: return SDL_SCANCODE_ESCAPE;
    case 0x09: return SDL_SCANCODE_TAB;
    case 0x08: return SDL_SCANCODE_BACKSPACE;
    case 0xA0: return SDL_SCANCODE_LSHIFT;
    case 0xA1: return SDL_SCANCODE_RSHIFT;
    case 0xA2: return SDL_SCANCODE_LCTRL;
    case 0xA3: return SDL_SCANCODE_RCTRL;
    case 0xA4: return SDL_SCANCODE_LALT;
    case 0xA5: return SDL_SCANCODE_RALT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x21: return SDL_SCANCODE_PAGEUP;
    case 0x22: return SDL_SCANCODE_PAGEDOWN;
    case 0x24: return SDL_SCANCODE_HOME;
    case 0x23: return SDL_SCANCODE_END;
    case 0x2D: return SDL_SCANCODE_INSERT;
    case 0x2E: return SDL_SCANCODE_DELETE;
    case 0xBC: return SDL_SCANCODE_COMMA;
    case 0xBE: return SDL_SCANCODE_PERIOD;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

bool mac_key(int vk) {
    if (!g_focused) return false;
    const bool* k = SDL_GetKeyboardState(nullptr);
    switch (vk) {  // either side
    case 0x10: return k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
    case 0x11: return k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL];
    case 0x12: return k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT];
    default: break;
    }
    const int sc = scancode_of_vk(vk);
    return sc != SDL_SCANCODE_UNKNOWN && k[sc];
}

// The pad for a game's own reading (pad_source): XInput bits by position, as pad_sdl.cpp.
static void publish_pad(SDL_Gamepad* pad) {
    MacPad m;
    if (pad) {
        static const struct {
            SDL_GamepadButton b;
            uint32_t bit;
        } map[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
            {SDL_GAMEPAD_BUTTON_START, 0x0010},       {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
            {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
            {SDL_GAMEPAD_BUTTON_WEST, 0x4000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
            {SDL_GAMEPAD_BUTTON_GUIDE, 1u << 18},     {SDL_GAMEPAD_BUTTON_TOUCHPAD, 1u << 19},
            {SDL_GAMEPAD_BUTTON_MISC1, 1u << 20},
        };
        for (const auto& e : map)
            if (SDL_GetGamepadButton(pad, e.b)) m.buttons |= e.bit;
        m.lt = uint8_t(std::clamp(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128, 0, 255));
        m.rt = uint8_t(std::clamp(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128, 0, 255));
        auto up = [](int v) { return int16_t(std::clamp(-v, -32767, 32767)); };
        m.lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        m.ly = up(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        m.rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        m.ry = up(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: m.kind = 1; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: m.kind = 2; break;
        default: break;
        }
    }
    std::lock_guard lk(g_mac_pad_lock);
    g_mac_pad = m;
    g_mac_pad_on = pad != nullptr;
}

static void sample_input() {
    g_focused = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
    PadState p;
    static SDL_Gamepad* pad = nullptr;
    if (!pad) {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
    publish_pad(pad);
    if (pad) {
        // SDL names buttons by position (south/east/...); Nintendo's A is the east button, B the south one.
        struct Map {
            SDL_GamepadButton b;
            uint64_t bit;
        };
        static const Map map[] = {
            {SDL_GAMEPAD_BUTTON_EAST, kA},           {SDL_GAMEPAD_BUTTON_SOUTH, kB},
            {SDL_GAMEPAD_BUTTON_NORTH, kX},          {SDL_GAMEPAD_BUTTON_WEST, kY},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
            {SDL_GAMEPAD_BUTTON_START, kPlus},       {SDL_GAMEPAD_BUTTON_BACK, kMinus},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        };
        for (const Map& m : map)
            if (SDL_GetGamepadButton(pad, m.b)) p.buttons |= m.bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) p.buttons |= kZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) p.buttons |= kZR;
        auto axis = [&](SDL_GamepadAxis a, bool invert) {
            int v = SDL_GetGamepadAxis(pad, a);
            if (v > -7849 && v < 7849) return 0;
            v = std::clamp(v, -kStickMax, kStickMax);
            return invert ? -v : v;
        };
        p.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
        p.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: down is positive; Horizon: up is positive
        p.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
        p.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
    }
    // Keyboard: WASD / arrows move, Space A, Shift B (run), J X, K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -.
    if (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) p.lx = -kStickMax, p.buttons |= kLeft;
        if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) p.lx = kStickMax, p.buttons |= kRight;
        if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) p.ly = kStickMax, p.buttons |= kUp;
        if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) p.ly = -kStickMax, p.buttons |= kDown;
        if (k[SDL_SCANCODE_SPACE]) p.buttons |= kA;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) p.buttons |= kB;
        if (k[SDL_SCANCODE_J]) p.buttons |= kX;
        if (k[SDL_SCANCODE_K]) p.buttons |= kY;
        if (k[SDL_SCANCODE_Q]) p.buttons |= kL;
        if (k[SDL_SCANCODE_E]) p.buttons |= kR;
        if (k[SDL_SCANCODE_Z]) p.buttons |= kZL;
        if (k[SDL_SCANCODE_C]) p.buttons |= kZR;
        if (k[SDL_SCANCODE_RETURN]) p.buttons |= kPlus;
        if (k[SDL_SCANCODE_ESCAPE]) p.buttons |= kMinus;
    }
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
}

void read_pad(PadState& p) {
    if (pad_source) {  // the game reads the controller and keyboard itself (Tsukihime: input.ini)
        pad_source(p);
        input_script(p);
        return;
    }
    std::lock_guard lk(g_pad_lock);
    p.buttons |= g_pad.buttons;
    if (g_pad.lx) p.lx = g_pad.lx;
    if (g_pad.ly) p.ly = g_pad.ly;
    if (g_pad.rx) p.rx = g_pad.rx;
    if (g_pad.ry) p.ry = g_pad.ry;
    input_script(p);
}

// ---- audio: SDL3 stream, s16 stereo 48 kHz, fed from the queue of submitted buffers ----------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
AudioDone g_done = nullptr;
SDL_AudioStream* g_stream = nullptr;
std::mutex g_audio;
std::deque<Pending> g_pending;

void SDLCALL audio_cb(void*, SDL_AudioStream* s, int additional, int) {
    uint8_t buf[4096];
    while (additional > 0) {
        int n = std::min<int>(additional, sizeof buf);
        int filled = 0;
        std::vector<void*> finished;
        {
            std::lock_guard lk(g_audio);
            while (filled < n && !g_pending.empty()) {
                Pending& p = g_pending.front();
                size_t take = std::min(size_t(n - filled), p.size - p.pos);
                std::memcpy(buf + filled, p.data + p.pos, take);
                filled += int(take);
                p.pos += take;
                if (p.pos == p.size) {
                    finished.push_back(p.ctx);
                    g_pending.pop_front();
                }
            }
        }
        if (filled < n) std::memset(buf + filled, 0, size_t(n - filled));  // underrun: silence
        SDL_PutAudioStreamData(s, buf, n);
        for (void* ctx : finished) g_done(ctx);
        additional -= n;
    }
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) return false;
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        SDL_PauseAudioStreamDevice(g_stream);
    else
        SDL_ResumeAudioStreamDevice(g_stream);
}

// ---- Vulkan (MoltenVK through the Vulkan loader SDL finds) ------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    static void* gipa = [] {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            log_line(SDL_GetError());
            return static_cast<void*>(nullptr);
        }
        return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    }();
    return gipa;
}

const char* vulkan_surface_extension() { return VK_EXT_METAL_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateMetalSurfaceEXT"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    ci.pLayer = static_cast<const CAMetalLayer*>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

void crash_handler(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    const auto* m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    Dl_info d{};
    uint64_t pc = m->__ss.__pc;
    dladdr(reinterpret_cast<void*>(pc), &d);
    nx::log("CRASH: signal %d at %s+%llx, data address %llx", sig, d.dli_fname ? d.dli_fname : "?",
            (unsigned long long)(pc - uint64_t(d.dli_fbase)), (unsigned long long)uint64_t(info->si_addr));
    nx::CPU& c = nx::current_cpu();
    nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
    for (int k = 0; k < 32; k += 4)
        nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
                (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    nx::backtrace(c);
    nx::log_recent_files();
    signal(sig, SIG_DFL);
    raise(sig);
}

void game_thread() {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

int main() {
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine

    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread keeps Cocoa's
    // event loop.
    nx::plat::start_thread([](void*) { game_thread(); nx::plat::exit_process(0); }, nullptr, 64ull << 20);

    using namespace nx::plat;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                nx::log("platform: quit requested (window closed / Cmd-Q)");
                exit_process(0);
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
                g_minimized = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                g_minimized = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_pixel_w = uint32_t(e.window.data1), g_pixel_h = uint32_t(e.window.data2);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (g_window && (e.key.scancode == SDL_SCANCODE_F11 ||
                                 (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                    SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
#ifdef NX_TITLE_LIVE
                if ((e.key.scancode == SDL_SCANCODE_F5 || e.key.scancode == SDL_SCANCODE_F6) && !e.key.repeat)
                    tsuki_title::hotkey(e.key.scancode == SDL_SCANCODE_F6 ? 1 : -1);
#endif
#ifdef NX_FIXED_ASPECT  // Ctrl+1/2/3: the picture at 50/75/100% of 1920 px wide
                if (g_window && (e.key.mod & SDL_KMOD_CTRL) && !e.key.repeat && e.key.scancode >= SDL_SCANCODE_1 &&
                    e.key.scancode <= SDL_SCANCODE_3 && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)) {
                    const int pct = e.key.scancode == SDL_SCANCODE_1 ? 50 : e.key.scancode == SDL_SCANCODE_2 ? 75 : 100;
                    const float d = SDL_GetWindowPixelDensity(g_window);
                    const int cw = int(1920 * pct / 100 / (d > 0 ? d : 1));
                    SDL_SetWindowSize(g_window, cw, cw * 9 / 16);
                }
#endif
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                const float d = g_window ? SDL_GetWindowPixelDensity(g_window) : 1;
                g_mouse_x = int32_t(e.motion.x * d), g_mouse_y = int32_t(e.motion.y * d);
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                const int k = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1
                              : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
                if (k < 0) break;
                const float d = g_window ? SDL_GetWindowPixelDensity(g_window) : 1;
                g_mouse_x = int32_t(e.button.x * d), g_mouse_y = int32_t(e.button.y * d);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    g_mouse_buttons |= 1u << k;
                    g_mouse_downs[k]++;
                } else {
                    g_mouse_buttons &= ~(1u << k);
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL: {  // 120 per notch; a trackpad's small steps add up
                static float carry = 0;
                carry += e.wheel.y * 120.0f;
                const int whole = int(carry);
                carry -= float(whole);
                g_mouse_wheel += whole;
                break;
            }
            default:
                break;
            }
        }
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lk(g_main_lock);
            work.swap(g_main_work);
        }
        for (auto& fn : work) fn();
        sample_input();
        SDL_Delay(2);
    }
}
