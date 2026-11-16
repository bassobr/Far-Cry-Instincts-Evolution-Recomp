// Host controllers through SDL3, read into the Xbox pad's report. Adapted from SCDA-XBERecomp (MIT, Copyright (c)
// 2026 TGP482, Joshhhuaaa), src/core/input.cpp: the slots (a controller takes the lowest free one when plugged in
// and keeps it until unplugged, as XInput numbers them), SDL's start (joystick thread, background events), the read
// of buttons, triggers and sticks (SDL's y points down) and the rumble that runs until set again.
//
// FCI differences to SCDA: four slots for the game's four ports in order (no player-to-controller page); SDL's own
// choice of Windows drivers (XInput for Xbox pads, HIDAPI for PlayStation and Switch pads, DirectInput for generic
// ones, each device once), which SDL's environment hints (SDL_JOYSTICK_WGI=1, ...) still change; the bindings are
// lists per Xbox button like the keyboard's (bind_<button>); pad_backend = xinput reads XInput alone as before;
// virtual SDL controllers for the self test (FCI_VIRTUAL_PADS); the player LEDs show the slot.
//
// The pad thread (generic: also for FCE), off by default, FCI_PAD_THREAD=1 turns it on: the guest's threads make no
// SDL or XInput calls. Under Proton on a Steam Deck SDL's joystick update took 0.4 ms per call on average and 2-18 ms
// in 2-3 % of the calls, a rumble write (SDL_RumbleGamepad, a synchronous write to the device) about 2 ms, all on the
// game's thread that polls the pad. A thread of its own polls the controllers (device changes, SDL_UpdateJoysticks,
// the state of each slot into a snapshot) and sends the motors; XInputGetState and XInputSetState only read the
// snapshot and hand over the motor speeds. It polls once a frame, 2 ms before the game's next poll is due (SDL as
// often as the former way, the state about 2-3 ms old); FCI_PAD_POLL_MS=<n> every n ms instead, 0 right after each
// poll of the game. Off by default since FCE 0.1.0: under Proton on the Steam Deck the guest's frame grew linearly
// with it after 3-5 minutes in a level (~0.4 ms more per frame each second, down to 8 fps; every poll mode, also with
// SDL started on the pad thread; not on the PC), cause open. The default is the former way (every call on the game's
// thread); FCI_PAD_SYNC=1 still forces it. FCI_TRACE_PAD_COST=1: what the calls cost.
#include "hle/host_pad.h"

#include <windows.h>
#include <xinput.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.h"
#include "hle/prompts.h"
#include "runtime/log.h"

namespace hle::host {

namespace {

using namespace hle::pc;

// ---- the controller's buttons, by position on an Xbox pad (a = the bottom face button on every pad) ----

enum Physical {
    kPA, kPB, kPX, kPY, kPLB, kPRB, kPLT, kPRT, kPStart, kPBack, kPLS, kPRS, kPUp, kPDown, kPLeft, kPRight,
    kPGuide, kPMisc1, kPRPaddle1, kPLPaddle1, kPRPaddle2, kPLPaddle2, kPTouchpad, kPhysicalCount
};
const char* const kPhysicalNames[kPhysicalCount] = {
    "a",     "b",    "x",    "y",     "lb",    "rb",    "lt",       "rt",       "start",    "back",     "ls",      "rs",
    "up",    "down", "left", "right", "guide", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2", "touchpad",
};
// PlayStation and Switch names, SDL's positional ones
constexpr struct {
    const char* name;
    int button;
} kAliases[] = {
    {"south", kPA},    {"cross", kPA},   {"east", kPB},     {"circle", kPB},  {"west", kPX},    {"square", kPX},
    {"north", kPY},    {"triangle", kPY}, {"l1", kPLB},     {"r1", kPRB},     {"l2", kPLT},     {"r2", kPRT},
    {"l3", kPLS},      {"r3", kPRS},     {"lthumb", kPLS},  {"rthumb", kPRS}, {"options", kPStart}, {"menu", kPStart},
    {"plus", kPStart}, {"select", kPBack}, {"view", kPBack}, {"minus", kPBack}, {"home", kPGuide}, {"ps", kPGuide},
    {"capture", kPMisc1}, {"mic", kPMisc1},
};
// SDL's button for each (the triggers are axes)
constexpr SDL_GamepadButton kSdlButtons[kPhysicalCount] = {
    SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,         SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_INVALID,       SDL_GAMEPAD_BUTTON_INVALID,       SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_LEFT_STICK,    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,     SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    SDL_GAMEPAD_BUTTON_GUIDE,         SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,  SDL_GAMEPAD_BUTTON_TOUCHPAD,
};
// XInput's (no guide, misc, paddles or touchpad there)
constexpr WORD kXInputMasks[kPhysicalCount] = {
    XINPUT_GAMEPAD_A,         XINPUT_GAMEPAD_B,          XINPUT_GAMEPAD_X,
    XINPUT_GAMEPAD_Y,         XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
    0,                        0,                         XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_BACK,      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP,   XINPUT_GAMEPAD_DPAD_DOWN,  XINPUT_GAMEPAD_DPAD_LEFT,
    XINPUT_GAMEPAD_DPAD_RIGHT,
};

// what the controller has now: each button 0-255 (the triggers analog), the sticks with y up
struct State {
    uint8_t value[kPhysicalCount] = {};
    int16_t thumb[4] = {};  // LX LY RX RY
};

// ---- the bindings: Xbox button = controller buttons ----

struct Target {
    const char* name;  // the names of bind_<button> and FCI_INPUT_SCRIPT
    uint16_t button;   // digital button bit
    int analog;        // analog button index, -1 for none
    const char* defaults;
};
// The Xbox pad's black and white buttons sit where the later pads have their shoulder buttons, as SCDA maps them
// (and as XInput did here before): white = LB, black = RB.
constexpr Target kTargets[] = {
    {"a", 0, kA, "a"},           {"b", 0, kB, "b"},           {"x", 0, kX, "x"},
    {"y", 0, kY, "y"},           {"white", 0, kWhite, "lb"},  {"black", 0, kBlack, "rb"},
    {"lt", 0, kLeftTrigger, "lt"}, {"rt", 0, kRightTrigger, "rt"}, {"start", kStart, -1, "start"},
    {"back", kBack, -1, "back"}, {"lthumb", kLeftThumb, -1, "ls"}, {"rthumb", kRightThumb, -1, "rs"},
    {"up", kDpadUp, -1, "up"},   {"down", kDpadDown, -1, "down"}, {"left", kDpadLeft, -1, "left"},
    {"right", kDpadRight, -1, "right"},
};
constexpr int kTargetCount = int(sizeof kTargets / sizeof kTargets[0]);
constexpr int kThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD: an analog control this far presses a digital one

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

int ParseButton(const std::string& text) {
    std::string name = Lower(Trim(text));
    for (int b = 0; b < kPhysicalCount; b++)
        if (name == kPhysicalNames[b]) return b;
    for (const auto& a : kAliases)
        if (name == a.name) return a.button;
    return -1;
}

std::vector<int> ParseButtons(const std::string& list, const char* target) {
    std::vector<int> out;
    size_t at = 0;
    while (at <= list.size()) {
        size_t end = list.find_first_of(", ", at);
        if (end == std::string::npos) end = list.size();
        std::string name = Trim(list.substr(at, end - at));
        at = end + 1;
        if (name.empty() || Lower(name) == "none") continue;
        int b = ParseButton(name);
        if (b < 0) rt::Log("pad: pad_bind_%s: unknown controller button '%s'", target, name.c_str());
        else out.push_back(b);
    }
    return out;
}

// as the keyboard's: a controller button the ini binds leaves the default bindings of the other Xbox buttons
std::vector<std::vector<int>> LoadBindings() {
    std::vector<std::vector<int>> b(kTargetCount);
    std::vector<bool> set(kTargetCount);
    std::vector<int> claimed;
    for (int i = 0; i < kTargetCount; i++) {
        std::string key = std::string("pad_bind_") + kTargets[i].name;
        std::string value = settings::String(key.c_str(), kTargets[i].defaults);
        b[i] = ParseButtons(value, kTargets[i].name);
        set[i] = value != kTargets[i].defaults;
        if (set[i]) claimed.insert(claimed.end(), b[i].begin(), b[i].end());
    }
    std::string changed;
    for (int i = 0; i < kTargetCount; i++) {
        if (!set[i])
            std::erase_if(b[i], [&](int v) { return std::find(claimed.begin(), claimed.end(), v) != claimed.end(); });
        if (b[i] != ParseButtons(kTargets[i].defaults, kTargets[i].name)) {
            changed += std::string(" ") + kTargets[i].name + "=";
            for (size_t k = 0; k < b[i].size(); k++) changed += std::string(k ? "+" : "") + kPhysicalNames[b[i][k]];
            if (b[i].empty()) changed += "none";
        }
    }
    rt::Log("pad: controller bindings%s", changed.empty() ? ": defaults" : (":" + changed).c_str());
    return b;
}

// ---- the backends ----

enum class Backend { Off, Sdl, XInput };

// every SDL and XInput call (the guest's threads, the settings menu; FCI_PAD_THREAD: the pad thread)
std::recursive_mutex g_lock;
// what the guest's threads use besides (the snapshot, the motors, the bindings): held only briefly, never around an
// SDL or XInput call. Taken after g_lock where both are held.
std::mutex g_snap;
bool g_started = false;
std::atomic<bool> g_ready{false};  // Start() has finished
bool g_async = false;              // the pad thread polls (FCI_PAD_THREAD=1, a backend)
Backend g_backend = Backend::Off;
std::vector<std::vector<int>> g_binds;
float g_dead_zone = 0;  // a stick inside this share of full deflection counts as centred (0: the game's own only)
int g_strength = 100;   // rumble in percent

SDL_Gamepad* g_pads[kSlots] = {};
int g_xinput[kSlots] = {-1, -1, -1, -1};  // XInput index per slot (pad_backend = xinput)

struct Motor {
    uint16_t low = 0, high = 0;
};
Motor g_motor[kSlots];
bool g_motor_due[kSlots] = {};  // set by the game, not yet sent to the controller
bool g_paused = false;
uint64_t g_rumble_set[kSlots] = {};  // SDL ticks of the last rumble call
// pad thread: what the slot's motors run (the game's speeds at its last poll of the port) and whether to send it
Motor g_send[kSlots];
bool g_send_due[kSlots] = {};
HANDLE g_wake = nullptr;  // wakes the pad thread for a send

// ---- FCI_TRACE_PAD_COST=1: the time of each kind of call, every 5 s; a call over 2 ms on its own (150 of each) ----
// age: how old the pad thread's snapshot was at the game's poll (the latency it adds)
enum CostKind { kCostRead, kCostConnected, kCostRumble, kCostUpdate, kCostSend, kCostPoll, kCostFeed, kCostAge, kCostKinds };
const char* const kCostNames[kCostKinds] = {"read", "connected", "rumble", "update", "send", "poll", "feed", "age"};
constexpr double kCostEdges[] = {0.5, 1, 2, 5, 10, 20, 50};  // ms
constexpr int kCostBuckets = int(sizeof kCostEdges / sizeof kCostEdges[0]);
struct Cost {
    uint64_t calls = 0, slow[kCostBuckets] = {};
    double ms = 0, max = 0;
};
struct CostTrace {
    bool on = false;
    std::mutex lock;
    Cost window[kCostKinds];
    uint64_t sends = 0, nonzero = 0, refused = 0;
    ULONGLONG since = 0;
    int slow_lines[kCostKinds] = {};
    double frequency = 0;
};
CostTrace g_cost;

int64_t CostNow() {
    if (!g_cost.on) return 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void CostAdd(CostKind kind, double ms, int slot = -1) {
    std::lock_guard lock(g_cost.lock);
    Cost& c = g_cost.window[kind];
    c.calls++, c.ms += ms, c.max = std::max(c.max, ms);
    for (int b = 0; b < kCostBuckets; b++)
        if (ms >= kCostEdges[b]) c.slow[b]++;
    if (ms >= 2 && kind != kCostAge && g_cost.slow_lines[kind] < 150) {
        g_cost.slow_lines[kind]++;
        rt::Log("pad: cost: slow %s %.2f ms (slot %d, thread %lu)", kCostNames[kind], ms, slot + 1, GetCurrentThreadId());
    }
    const ULONGLONG now = GetTickCount64();
    if (!g_cost.since) g_cost.since = now;
    if (now - g_cost.since < 5000) return;
    g_cost.since = now;
    char line[1600];
    int n = snprintf(line, sizeof line, "pad: cost 5 s (%s):", g_async ? "pad thread" : "game's threads");
    for (int k = 0; k < kCostKinds; k++) {
        Cost& w = g_cost.window[k];
        if (!w.calls) continue;
        n += snprintf(line + n, sizeof line - n, " %s %llu avg %.3f max %.2f ms [", kCostNames[k],
                      (unsigned long long)w.calls, w.ms / double(w.calls), w.max);
        for (int b = 0; b < kCostBuckets; b++)
            n += snprintf(line + n, sizeof line - n, "%s%llu", b ? " " : "", (unsigned long long)w.slow[b]);
        n += snprintf(line + n, sizeof line - n, "];");
        w = Cost{};
    }
    snprintf(line + n, sizeof line - n, " sends %llu (nonzero %llu, refused %llu)", (unsigned long long)g_cost.sends,
             (unsigned long long)g_cost.nonzero, (unsigned long long)g_cost.refused);
    g_cost.sends = g_cost.nonzero = g_cost.refused = 0;
    rt::Log("%s", line);
}

void CostDone(CostKind kind, int64_t start, int slot = -1) {
    if (!g_cost.on || !start) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    CostAdd(kind, double(t.QuadPart - start) * 1000.0 / g_cost.frequency, slot);
}
// ---- end FCI_TRACE_PAD_COST ----

// FCI_VIRTUAL_PADS
struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    Motor rumble;
};
VirtualPad g_virtual[kSlots];
int g_virtual_count = 0;  // attached now
// FCI_VIRTUAL_PADS=<n>[:<plug in at>[:<unplug at>]] (script seconds): the hot-plug path
int g_virtual_want = 0;
double g_virtual_at = 0, g_virtual_until = 0;
bool g_virtual_done = false;  // attached once (they are not attached again after the unplug)

void AttachVirtualPads();
void DetachVirtualPads();

bool VirtualRumble(void* userdata, Uint16 low, Uint16 high) {
    VirtualPad& v = g_virtual[reinterpret_cast<intptr_t>(userdata)];
    static int logged = 0;
    if ((low != v.rumble.low || high != v.rumble.high) && logged++ < 200)
        rt::Log("pad: virtual pad %d rumble %u %u", int(reinterpret_cast<intptr_t>(userdata)) + 1, low, high);
    v.rumble = {low, high};
    return true;
}

VirtualPad* VirtualIn(int slot) {
    if (slot < 0 || slot >= kSlots || !g_pads[slot]) return nullptr;
    SDL_JoystickID id = SDL_GetGamepadID(g_pads[slot]);
    for (int i = 0; i < g_virtual_count; i++)
        if (g_virtual[i].id == id) return &g_virtual[i];
    return nullptr;
}

constexpr uint32_t kRumbleMs = 60000;  // SDL resends a timed rumble (some pads stop on their own), renewed below

// the speeds the slot's motors should run: the game's (with the pad thread: as of its last poll of the port)
Motor Wanted(int slot, bool* paused = nullptr) {
    std::lock_guard snap(g_snap);
    if (paused) *paused = g_paused;
    return g_async ? g_send[slot] : g_motor[slot];
}

void ApplyRumble(int slot) {
    Motor m;
    int strength;
    {
        std::lock_guard snap(g_snap);
        if (!g_async) g_motor_due[slot] = false;  // the pad thread: the poll of the port took it (ReadAsync)
        m = g_paused ? Motor{} : g_async ? g_send[slot] : g_motor[slot];
        strength = g_strength;
    }
    uint16_t low = uint16_t(uint32_t(m.low) * strength / 100), high = uint16_t(uint32_t(m.high) * strength / 100);
    bool sent = false, ok = true;
    const int64_t t = CostNow();
    if (g_backend == Backend::Sdl && g_pads[slot]) {
        ok = SDL_RumbleGamepad(g_pads[slot], low, high, kRumbleMs);
        g_rumble_set[slot] = SDL_GetTicks();
        sent = true;
    } else if (g_backend == Backend::XInput && g_xinput[slot] >= 0) {
        XINPUT_VIBRATION v{low, high};
        ok = XInputSetState(DWORD(g_xinput[slot]), &v) == ERROR_SUCCESS;
        sent = true;
    }
    if (!sent) return;
    if (!ok) {
        static int logged = 0;
        if (logged++ < 3)
            rt::Log("pad: controller %d refused the rumble%s%s", slot + 1, g_backend == Backend::Sdl ? ": " : "",
                    g_backend == Backend::Sdl ? SDL_GetError() : "");
    }
    if (g_cost.on) {
        CostDone(kCostSend, t, slot);
        std::lock_guard lock(g_cost.lock);
        g_cost.sends++, g_cost.nonzero += (low || high), g_cost.refused += !ok;
    }
}

// ---- button prompts (hle/prompts.h, GitHub #15): the controller's family for its prompt glyphs ----
// FCI_VIRTUAL_PAD_TYPE=ps3|ps4|ps5|switch|xbox: what the virtual pads of FCI_VIRTUAL_PADS count as (tests)
prompts::PadFamily Family(int slot, int* generation) {
    *generation = 0;
    if (g_backend != Backend::Sdl || !g_pads[slot]) return prompts::PadFamily::kXbox;
    if (VirtualIn(slot)) {
        char text[16] = {};
        GetEnvironmentVariableA("FCI_VIRTUAL_PAD_TYPE", text, sizeof text);
        std::string t = Lower(text);
        if (t.size() == 3 && t.compare(0, 2, "ps") == 0) {
            *generation = t[2] - '0';
            return prompts::PadFamily::kPlayStation;
        }
        if (t == "switch") return prompts::PadFamily::kSwitch;
        return prompts::PadFamily::kXbox;
    }
    switch (SDL_GetGamepadType(g_pads[slot])) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return prompts::PadFamily::kXbox;
    case SDL_GAMEPAD_TYPE_PS3: *generation = 3; return prompts::PadFamily::kPlayStation;
    case SDL_GAMEPAD_TYPE_PS4: *generation = 4; return prompts::PadFamily::kPlayStation;
    case SDL_GAMEPAD_TYPE_PS5: *generation = 5; return prompts::PadFamily::kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return prompts::PadFamily::kSwitch;
    default: return prompts::PadFamily::kGeneric;
    }
}
prompts::PadFamily g_family[kSlots] = {};  // each slot's, from its plug-in (the pad thread's snapshot)
int g_family_generation[kSlots] = {};
// ---- end button prompts ----

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) {
        rt::Log("pad: controller '%s' ignored, all four ports have one", SDL_GetGamepadNameForID(id));
        return;
    }
    int n = int(slot - g_pads);
    *slot = SDL_OpenGamepad(id);
    if (!*slot) {
        rt::Log("pad: controller %d failed to open: %s", n + 1, SDL_GetError());
        return;
    }
    SDL_SetGamepadPlayerIndex(*slot, n);  // the player LEDs, where the pad has them
    // a virtual one by its own name (SDL names the gamepads of one mapping alike)
    const char* name = VirtualIn(n) ? SDL_GetJoystickName(SDL_GetGamepadJoystick(*slot)) : SDL_GetGamepadName(*slot);
    SDL_PropertiesID props = SDL_GetGamepadProperties(*slot);
    rt::Log("pad: controller %d plugged in: %s (%s, %04X:%04X%s%s)", n + 1, name ? name : "?",
            SDL_GetGamepadStringForType(SDL_GetGamepadType(*slot)), SDL_GetGamepadVendor(*slot),
            SDL_GetGamepadProduct(*slot), SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? ", rumble" : "",
            VirtualIn(n) ? ", virtual" : "");
    Motor wanted = Wanted(n);
    if (wanted.low || wanted.high) ApplyRumble(n);
    int generation;  // button prompts
    prompts::PadFamily family = Family(n, &generation);
    g_family[n] = family, g_family_generation[n] = generation;  // for the snapshot
    prompts::NotePadConnected(family, generation);
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < kSlots; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        rt::Log("pad: controller %d unplugged", i + 1);
    }
}

// device changes, at most once a millisecond however many ports and threads read; motors renewed
void Update() {
    if (g_backend == Backend::XInput) {
        // XInputGetState is slow for empty slots, so they are looked for once a second
        static ULONGLONG last = 0;
        if (last && GetTickCount64() - last < 1000) return;
        last = GetTickCount64();
        const int64_t t = CostNow();
        int slot = 0;
        XINPUT_STATE xs;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
            if (XInputGetState(i, &xs) == ERROR_SUCCESS && slot < kSlots) g_xinput[slot++] = int(i);
        while (slot < kSlots) g_xinput[slot++] = -1;
        CostDone(kCostUpdate, t);
        return;
    }
    if (g_backend != Backend::Sdl) return;
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (now == last) return;
    last = now;
    const int64_t t = CostNow();
    if (g_virtual_want && !g_virtual_done && ScriptSeconds() >= g_virtual_at) AttachVirtualPads();
    if (g_virtual_count && g_virtual_until > 0 && ScriptSeconds() >= g_virtual_until) DetachVirtualPads();
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
    CostDone(kCostUpdate, t);
    for (int i = 0; i < kSlots; i++) {
        if (!g_pads[i]) continue;
        bool paused;
        Motor m = Wanted(i, &paused);
        if ((m.low || m.high) && !paused && now - g_rumble_set[i] > kRumbleMs / 2) ApplyRumble(i);
    }
}

void ReadVirtualPads() {
    char text[64] = {};
    if (!GetEnvironmentVariableA("FCI_VIRTUAL_PADS", text, sizeof text)) return;
    g_virtual_want = std::clamp(std::atoi(text), 0, kSlots);
    if (char* at = std::strchr(text, ':')) {
        g_virtual_at = std::atof(at + 1);
        if (char* until = std::strchr(at + 1, ':')) g_virtual_until = std::atof(until + 1);
    }
}

void AttachVirtualPads() {
    g_virtual_done = true;
    for (int i = 0; i < g_virtual_want; i++) {
        static char names[kSlots][32];
        snprintf(names[i], sizeof names[i], "FCI virtual pad %d", i + 1);
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;  // all of them: joystick button n = gamepad button n
        desc.name = names[i];
        desc.userdata = reinterpret_cast<void*>(intptr_t(i));
        desc.Rumble = VirtualRumble;
        VirtualPad& v = g_virtual[i];
        v.id = SDL_AttachVirtualJoystick(&desc);
        v.joystick = v.id ? SDL_OpenJoystick(v.id) : nullptr;
        if (!v.joystick) {
            rt::Log("pad: virtual pad %d failed: %s", i + 1, SDL_GetError());
            break;
        }
        g_virtual_count = i + 1;
    }
    rt::Log("pad: FCI_VIRTUAL_PADS: %d virtual controllers attached", g_virtual_count);
}

void DetachVirtualPads() {
    for (int i = 0; i < g_virtual_count; i++) {
        SDL_CloseJoystick(g_virtual[i].joystick);
        SDL_DetachVirtualJoystick(g_virtual[i].id);
        g_virtual[i].joystick = nullptr;
    }
    rt::Log("pad: FCI_VIRTUAL_PADS: %d virtual controllers detached", g_virtual_count);
    g_virtual_count = 0;  // their slots empty with SDL's removal events
}

bool StartSdl() {
    // reads come from the guest's threads, so SDL's own thread takes the device notices; pads read without focus
    // as XInput's did (the keyboard alone needs it)
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        rt::Log("pad: SDL failed to start: %s", SDL_GetError());
        return false;
    }
    int v = SDL_GetVersion();
    rt::Log("pad: SDL %d.%d.%d, controllers through SDL", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
            SDL_VERSIONNUM_MICRO(v));
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
    ReadVirtualPads();
    if (g_virtual_want && g_virtual_at <= 0) AttachVirtualPads();
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
    int joysticks = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&joysticks)) {
        for (int i = 0; i < joysticks; i++)
            if (!SDL_IsGamepad(ids[i]))
                rt::Log("pad: joystick '%s' has no gamepad mapping, not used", SDL_GetJoystickNameForID(ids[i]));
        SDL_free(ids);
    }
    if (!std::any_of(std::begin(g_pads), std::end(g_pads), [](SDL_Gamepad* p) { return p != nullptr; }))
        rt::Log("pad: no controller connected (plugging one in later works)");
    return true;
}

void StartPadThread();

void Start() {
    if (g_started) return;
    g_started = true;
    if (GetEnvironmentVariableA("FCI_TRACE_PAD_COST", nullptr, 0) > 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_cost.frequency = double(f.QuadPart);
        g_cost.on = true;
    }
    g_binds = LoadBindings();
    g_dead_zone = std::clamp(settings::Float("pad_dead_zone", 0.0f), 0.0f, 0.9f);
    g_strength = std::clamp(settings::Int("pad_rumble", 100), 0, 100);
    std::string backend = Lower(Trim(settings::String("pad_backend", "sdl")));
    if (backend == "off" || backend == "none") {
        rt::Log("pad: pad_backend = off, no controllers (keyboard and scripts only)");
        return;
    }
    if (backend == "xinput") {
        g_backend = Backend::XInput;
        rt::Log("pad: pad_backend = xinput, controllers through XInput");
    } else {
        if (backend != "sdl") rt::Log("pad: unknown pad_backend '%s', using sdl", backend.c_str());
        g_backend = Backend::Sdl;
        if (!StartSdl()) {
            g_backend = Backend::XInput;
            rt::Log("pad: falling back to XInput");
        }
    }
    if (g_dead_zone > 0 || g_strength != 100)
        rt::Log("pad: dead zone %.2f, rumble %d%%", g_dead_zone, g_strength);
    Update();
    // the pad thread only with FCI_PAD_THREAD=1 (off by default: the Deck's slowdown above); FCI_PAD_SYNC=1 wins
    char thread[8] = {}, sync[8] = {};
    const bool want = GetEnvironmentVariableA("FCI_PAD_THREAD", thread, sizeof thread) && thread[0] != '0';
    const bool forced = GetEnvironmentVariableA("FCI_PAD_SYNC", sync, sizeof sync) && sync[0] != '0';
    if (want && !forced) {
        g_async = true;
        StartPadThread();
    } else {
        rt::Log("pad: controllers polled and rumble sent on the game's threads (%s)",
                forced ? "FCI_PAD_SYNC=1" : "FCI_PAD_THREAD=1: on a thread of their own");
    }
}

bool ReadSdl(int slot, State& s) {
    SDL_Gamepad* pad = g_pads[slot];
    if (!pad) return false;
    for (int b = 0; b < kPhysicalCount; b++)
        if (kSdlButtons[b] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad, kSdlButtons[b])) s.value[b] = 255;
    // 0 to 32767 into 0 to 255
    auto trigger = [&](SDL_GamepadAxis axis) { return uint8_t(std::max<int>(SDL_GetGamepadAxis(pad, axis), 0) >> 7); };
    s.value[kPLT] = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s.value[kPRT] = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    // SDL's sticks have y down; not plain negation: -32768 has no positive
    s.thumb[0] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    s.thumb[1] = int16_t(-1 - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    s.thumb[2] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    s.thumb[3] = int16_t(-1 - SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    return true;
}

bool ReadXInput(int slot, State& s) {
    XINPUT_STATE xs;
    if (g_xinput[slot] < 0 || XInputGetState(DWORD(g_xinput[slot]), &xs) != ERROR_SUCCESS) return false;
    const XINPUT_GAMEPAD& g = xs.Gamepad;
    for (int b = 0; b < kPhysicalCount; b++)
        if (kXInputMasks[b] & g.wButtons) s.value[b] = 255;
    s.value[kPLT] = g.bLeftTrigger;
    s.value[kPRT] = g.bRightTrigger;
    s.thumb[0] = g.sThumbLX, s.thumb[1] = g.sThumbLY, s.thumb[2] = g.sThumbRX, s.thumb[3] = g.sThumbRY;
    return true;
}

// ---- button prompts: a button newly pressed, a trigger past half way or a stick past half its deflection (drift
// stays below) means the player uses this controller ----
uint32_t g_prompt_held[kSlots] = {};

// known: the pad thread's family of the slot (without it, SDL is asked here)
void NoteUse(int slot, const State& s, const prompts::PadFamily* known = nullptr, int known_generation = 0) {
    uint32_t held = 0;
    for (int b = 0; b < kPhysicalCount; b++)
        if (s.value[b] > 128) held |= 1u << b;
    bool used = (held & ~g_prompt_held[slot]) != 0;
    g_prompt_held[slot] = held;
    for (int16_t t : s.thumb) used = used || t > 16384 || t < -16384;
    if (!used) return;
    int generation = known_generation;
    prompts::PadFamily family = known ? *known : Family(slot, &generation);
    prompts::NotePad(family, generation);
}
// ---- end button prompts ----

// the bindings and the dead zone: the controller onto the Xbox pad
void ToPad(const State& s, Pad& p) {
    p = Pad{};
    for (int i = 0; i < kTargetCount; i++) {
        uint8_t v = 0;
        for (int b : g_binds[i]) v = std::max(v, s.value[b]);
        if (kTargets[i].analog >= 0) p.analog[kTargets[i].analog] = v;
        else if (v > kThreshold) p.buttons |= kTargets[i].button;
    }
    for (int stick = 0; stick < 2; stick++) {
        int x = s.thumb[2 * stick], y = s.thumb[2 * stick + 1];
        double zone = g_dead_zone * 32767.0;
        if (zone > 0 && double(x) * x + double(y) * y < zone * zone) x = y = 0;
        p.thumb[2 * stick] = int16_t(x), p.thumb[2 * stick + 1] = int16_t(y);
    }
}

// ---- the pad thread ----

// what the guest's threads see of a slot, as of the pad thread's last poll
struct Snapshot {
    bool present = false;  // a controller in the slot (Connected)
    bool read = false;     // and its state was read
    bool virtual_pad = false;
    State state;
    prompts::PadFamily family = prompts::PadFamily::kXbox;  // button prompts
    int generation = 0;
};
Snapshot g_snapshot[kSlots];
int64_t g_snapshot_qpc = 0;  // when it was taken (FCI_TRACE_PAD_COST: its age at the game's poll)
ULONGLONG g_last_read = 0;  // the game's last poll of a port (the pad thread polls slower without one)

// under g_lock: every slot's state into the snapshot
void Publish() {
    Snapshot next[kSlots];
    for (int i = 0; i < kSlots; i++) {
        Snapshot& n = next[i];
        if (g_backend == Backend::Sdl) {
            n.present = g_pads[i] != nullptr;
            n.read = ReadSdl(i, n.state);
            n.virtual_pad = VirtualIn(i) != nullptr;
            n.family = g_family[i], n.generation = g_family_generation[i];
        } else if (g_backend == Backend::XInput) {
            n.present = g_xinput[i] >= 0;
            n.read = ReadXInput(i, n.state);
        }
    }
    const int64_t taken = CostNow();
    std::lock_guard snap(g_snap);
    std::copy(std::begin(next), std::end(next), std::begin(g_snapshot));
    g_snapshot_qpc = taken;
}

// under g_lock: the motors the game set at its last polls
void Send() {
    bool due[kSlots];
    {
        std::lock_guard snap(g_snap);
        for (int i = 0; i < kSlots; i++) due[i] = g_send_due[i], g_send_due[i] = false;
    }
    for (int i = 0; i < kSlots; i++)
        if (due[i]) ApplyRumble(i);
}

// under g_lock: device changes, the motors, the snapshot
void Poll() {
    const int64_t t = CostNow();
    Update();
    Send();
    Publish();
    CostDone(kCostPoll, t);
}

// FCI_PAD_POLL_MS=<n>: the pad thread polls every n ms while the game polls; 0: once after each poll of the game
// (its state then is one frame older); auto (-1, the default): once per frame, timed kLeadMs before the game's next
// poll. Steam Deck, FCE at 90 fps, pad thread CPU / state age at the game's poll: 1 ms 52 ms/s / 0.7 ms, 4 ms 34 ms/s
// / 2.6 ms, auto 24 ms/s / 2.2 ms (one frame old in ~4 % of the polls, when the game polls early), 0 22 ms/s /
// 10.5 ms; the former way's SDL work was about as much as 0, in the game's thread
int g_poll_ms = -1;
constexpr double kLeadMs = 2;
double g_frame_read = 0;  // QPC ms of the game's first poll in its latest frame (polls 2 ms apart start a frame)
double g_frame_ms = 0;    // the game's frame time between those polls, averaged

double NowMs() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / frequency;
}

// auto: woken by the game's poll (or a send), the pad thread sends at once and polls once more shortly before the
// game's next poll is due, so the state is about kLeadMs old and SDL is updated once a frame as the former way did
void PadThreadAuto() {
    double handled = 0;
    for (;;) {
        const bool woken = WaitForSingleObject(g_wake, 100) == WAIT_OBJECT_0;
        bool sends = false;
        double frame, period;
        {
            std::lock_guard snap(g_snap);
            for (bool due : g_send_due) sends = sends || due;
            frame = g_frame_read, period = g_frame_ms;
        }
        if (!woken) {  // no poll of the game for 100 ms: device changes, the motors' renewal
            std::lock_guard lock(g_lock);
            Poll();
            continue;
        }
        if (sends) {  // a new speed at once
            std::lock_guard lock(g_lock);
            Send();
        }
        if (frame == handled) continue;  // another port's poll in a frame already handled
        handled = frame;
        const double wait = frame + period - kLeadMs - NowMs();
        if (period > 2 * kLeadMs && wait >= 1 && wait < 100) Sleep(DWORD(wait));
        std::lock_guard lock(g_lock);
        Poll();
    }
}

void PadThread() {
    SetThreadDescription(GetCurrentThread(), L"FCI pads");
    if (g_poll_ms < 0) return PadThreadAuto();
    for (;;) {
        ULONGLONG last;
        {
            std::lock_guard snap(g_snap);
            last = g_last_read;
        }
        // every g_poll_ms (or woken by the game's poll of a port) and for a send; without polls of the game
        // (loading, the title held) every 100 ms for device changes and the motors' renewal
        const bool polled = GetTickCount64() - last < 500;
        WaitForSingleObject(g_wake, g_poll_ms && polled ? DWORD(g_poll_ms) : 100);
        std::lock_guard lock(g_lock);
        Poll();
    }
}

void StartPadThread() {
    char ms[16] = {};
    if (GetEnvironmentVariableA("FCI_PAD_POLL_MS", ms, sizeof ms))
        g_poll_ms = Lower(ms) == "auto" ? -1 : std::clamp(std::atoi(ms), 0, 100);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Publish();
    std::thread(PadThread).detach();
    if (g_poll_ms > 0)
        rt::Log("pad: controllers polled on a thread of their own, every %d ms (FCI_PAD_THREAD=1)",
                g_poll_ms);
    else if (g_poll_ms < 0)
        rt::Log("pad: controllers polled on a thread of their own once a frame, %.0f ms before the game's poll "
                "(FCI_PAD_THREAD=1)", kLeadMs);
    else
        rt::Log("pad: controllers polled on a thread of their own after each poll of the game "
                "(FCI_PAD_THREAD=1)");
}

void Wake() {
    if (g_wake) SetEvent(g_wake);
}

// Start() once, then without g_lock: the guest's threads wait for no SDL call
void EnsureStarted() {
    if (g_ready.load(std::memory_order_acquire)) return;
    std::lock_guard lock(g_lock);
    Start();
    g_ready.store(true, std::memory_order_release);
}

// the game's poll of a port: the snapshot of the pad thread's last poll (made after the game's previous poll); the
// speeds it set since its last poll go out with the next send
bool ReadAsync(int slot, Pad& p) {
    const int64_t t = CostNow();
    bool wake = false, ok = false;
    {
        std::lock_guard snap(g_snap);
        g_last_read = GetTickCount64();
        const double now = NowMs();
        if (now - g_frame_read >= 2) {  // the frame's first poll
            const double d = now - g_frame_read;
            if (g_frame_read > 0 && d < 100) g_frame_ms = g_frame_ms > 0 ? g_frame_ms + (d - g_frame_ms) / 8 : d;
            g_frame_read = now;
        }
        if (g_motor_due[slot]) {  // a changed speed goes out (SDL would skip the same one; the 30-s renewal keeps it)
            g_motor_due[slot] = false;
            if (g_motor[slot].low != g_send[slot].low || g_motor[slot].high != g_send[slot].high) {
                g_send[slot] = g_motor[slot];
                g_send_due[slot] = wake = true;
            }
        }
        const Snapshot& n = g_snapshot[slot];
        if (n.read) {
            NoteUse(slot, n.state, &n.family, n.generation);  // button prompts
            ToPad(n.state, p);
            ok = true;
        }
        if (ok && g_snapshot_qpc) CostDone(kCostAge, g_snapshot_qpc, slot);
    }
    if (wake || g_poll_ms <= 0) Wake();  // a send; 0/auto: the next state for the game's next poll
    CostDone(kCostRead, t, slot);
    return ok;
}
// ---- end pad thread ----

}  // namespace

bool Connected(int slot) {
    EnsureStarted();
    const int64_t t = CostNow();
    bool present = false;
    if (g_async) {
        std::lock_guard snap(g_snap);
        present = slot >= 0 && slot < kSlots && g_snapshot[slot].present;
    } else {
        std::lock_guard lock(g_lock);
        Update();
        if (slot >= 0 && slot < kSlots) {
            if (g_backend == Backend::Sdl) present = g_pads[slot] != nullptr;
            else present = g_backend == Backend::XInput && g_xinput[slot] >= 0;
        }
    }
    CostDone(kCostConnected, t, slot);
    return present;
}

bool Read(int slot, Pad& p) {
    EnsureStarted();
    if (slot < 0 || slot >= kSlots) return false;
    if (g_async) return ReadAsync(slot, p);
    const int64_t t = CostNow();
    bool ok = false;
    {
        std::lock_guard lock(g_lock);
        Update();
        if (g_motor_due[slot]) ApplyRumble(slot);
        State s;
        if (g_backend == Backend::Sdl ? ReadSdl(slot, s) : g_backend == Backend::XInput && ReadXInput(slot, s)) {
            NoteUse(slot, s);  // button prompts
            ToPad(s, p);
            ok = true;
        }
    }
    CostDone(kCostRead, t, slot);
    return ok;
}

void Rumble(int slot, uint16_t low, uint16_t high) {
    EnsureStarted();
    if (slot < 0 || slot >= kSlots) return;
    const int64_t t = CostNow();
    {
        // without the pad thread: g_lock as before (g_snap for the pad thread's reads of the motors)
        std::unique_lock<std::recursive_mutex> lock(g_lock, std::defer_lock);
        if (!g_async) lock.lock();
        std::lock_guard snap(g_snap);
        // Every frame FCI stops the motors and sets them again right after (two calls each). The controller gets
        // what was set last at the next poll of the port, not each call: else it would get a stop between the
        // frames' speeds.
        g_motor[slot] = {low, high};
        g_motor_due[slot] = true;
    }
    CostDone(kCostRumble, t, slot);
}

namespace {
// the pad thread: every slot's motors again with its next poll
void SendAll() {
    {
        std::lock_guard snap(g_snap);
        for (bool& due : g_send_due) due = true;
    }
    Wake();
}
}  // namespace

void PauseRumble(bool paused) {
    EnsureStarted();
    if (g_async) {
        {
            std::lock_guard snap(g_snap);
            g_paused = paused;
        }
        return SendAll();
    }
    std::lock_guard lock(g_lock);
    {
        std::lock_guard snap(g_snap);
        g_paused = paused;
    }
    for (int i = 0; i < kSlots; i++) ApplyRumble(i);
}

void StopRumble() {
    // on this thread also with the pad thread: the process ends right after
    std::lock_guard lock(g_lock);
    if (!g_started) return;
    {
        std::lock_guard snap(g_snap);
        g_paused = true;
    }
    for (int i = 0; i < kSlots; i++) ApplyRumble(i);
}

// ---- settings menu (app/menu.cpp, the CONTROLLER page) ----
int BindingCount() { return kTargetCount; }
const char* BindingName(int target) { return target >= 0 && target < kTargetCount ? kTargets[target].name : ""; }

std::vector<int> BindingButtons(int target) {
    std::lock_guard lock(g_lock);
    Start();
    return target >= 0 && target < kTargetCount ? g_binds[size_t(target)] : std::vector<int>{};
}

const char* ButtonName(int button) { return button >= 0 && button < kPhysicalCount ? kPhysicalNames[button] : "?"; }

namespace {
std::string ButtonList(const std::vector<int>& buttons) {
    std::string out;
    for (int b : buttons) out += (out.empty() ? "" : ", ") + std::string(kPhysicalNames[b]);
    return out.empty() ? "none" : out;
}
}  // namespace

void SetBinding(int target, int slot, int button) {
    std::lock_guard lock(g_lock);
    Start();
    if (target < 0 || target >= kTargetCount) return;
    std::vector<std::vector<int>> b = g_binds;
    std::vector<int>& list = b[size_t(target)];
    if (button >= 0 && button < kPhysicalCount) {
        // a controller button is bound once: it leaves the other Xbox buttons (their lines change too)
        for (int i = 0; i < kTargetCount; i++)
            if (i != target && std::erase(b[size_t(i)], button))
                settings::Set((std::string("pad_bind_") + kTargets[i].name).c_str(), ButtonList(b[size_t(i)]));
        std::erase(list, button);
        if (slot < int(list.size())) list[size_t(slot)] = button;
        else list.push_back(button);
    } else if (slot >= 0 && slot < int(list.size())) {
        list.erase(list.begin() + slot);
    }
    settings::Set((std::string("pad_bind_") + kTargets[target].name).c_str(), ButtonList(list));
    std::vector<std::vector<int>> binds = LoadBindings();
    {
        std::lock_guard snap(g_snap);
        g_binds = std::move(binds);
    }
    prompts::BindingsChanged();  // button prompts
}

void ResetBindings() {
    std::lock_guard lock(g_lock);
    Start();
    for (int i = 0; i < kTargetCount; i++)
        settings::Set((std::string("pad_bind_") + kTargets[i].name).c_str(), kTargets[i].defaults);
    std::vector<std::vector<int>> binds = LoadBindings();
    {
        std::lock_guard snap(g_snap);
        g_binds = std::move(binds);
    }
    prompts::BindingsChanged();  // button prompts
}

void ReloadOptions() {
    std::lock_guard lock(g_lock);
    Start();
    {
        std::lock_guard snap(g_snap);
        g_dead_zone = std::clamp(settings::Float("pad_dead_zone", 0.0f), 0.0f, 0.9f);
        g_strength = std::clamp(settings::Int("pad_rumble", 100), 0, 100);
    }
    rt::Log("pad: dead zone %.2f, rumble %d%% (settings menu)", g_dead_zone, g_strength);
    if (g_async) return SendAll();
    for (int i = 0; i < kSlots; i++) ApplyRumble(i);
}

int PressedButton() {
    std::lock_guard lock(g_lock);
    Start();
    Update();
    for (int slot = 0; slot < kSlots; slot++) {
        State s;
        if (!(g_backend == Backend::Sdl ? ReadSdl(slot, s) : g_backend == Backend::XInput && ReadXInput(slot, s)))
            continue;
        for (int b = 0; b < kPhysicalCount; b++)
            if (s.value[b] > (b == kPLT || b == kPRT ? 128 : kThreshold)) return b;  // a trigger half way
    }
    return -1;
}

std::string Name(int slot) {
    std::lock_guard lock(g_lock);
    Start();
    Update();
    if (slot < 0 || slot >= kSlots) return {};
    if (g_backend == Backend::Sdl && g_pads[slot]) {
        const char* name = VirtualIn(slot) ? SDL_GetJoystickName(SDL_GetGamepadJoystick(g_pads[slot]))
                                           : SDL_GetGamepadName(g_pads[slot]);
        return name ? name : "controller";
    }
    if (g_backend == Backend::XInput && g_xinput[slot] >= 0) return "XInput controller " + std::to_string(g_xinput[slot] + 1);
    return {};
}

const char* BackendName() {
    std::lock_guard lock(g_lock);
    Start();
    return g_backend == Backend::Sdl ? "SDL3" : g_backend == Backend::XInput ? "XInput" : "off";
}
// ---- end settings menu ----

bool Virtual(int slot) {
    EnsureStarted();
    if (g_async) {
        std::lock_guard snap(g_snap);
        return slot >= 0 && slot < kSlots && g_snapshot[slot].virtual_pad;
    }
    std::lock_guard lock(g_lock);
    Update();
    return g_backend == Backend::Sdl && VirtualIn(slot) != nullptr;
}

// the Xbox pad onto the virtual controller's buttons and axes, the reverse of ReadSdl and the default bindings
void FeedVirtual(int slot, const Pad& p) {
    const int64_t t = CostNow();
    std::lock_guard lock(g_lock);
    VirtualPad* v = g_backend == Backend::Sdl ? VirtualIn(slot) : nullptr;
    if (!v) return;
    auto button = [&](SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(v->joystick, int(b), down); };
    button(SDL_GAMEPAD_BUTTON_SOUTH, p.analog[kA] > 0);
    button(SDL_GAMEPAD_BUTTON_EAST, p.analog[kB] > 0);
    button(SDL_GAMEPAD_BUTTON_WEST, p.analog[kX] > 0);
    button(SDL_GAMEPAD_BUTTON_NORTH, p.analog[kY] > 0);
    button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, p.analog[kWhite] > 0);
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, p.analog[kBlack] > 0);
    button(SDL_GAMEPAD_BUTTON_START, p.buttons & kStart);
    button(SDL_GAMEPAD_BUTTON_BACK, p.buttons & kBack);
    button(SDL_GAMEPAD_BUTTON_LEFT_STICK, p.buttons & kLeftThumb);
    button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, p.buttons & kRightThumb);
    button(SDL_GAMEPAD_BUTTON_DPAD_UP, p.buttons & kDpadUp);
    button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, p.buttons & kDpadDown);
    button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, p.buttons & kDpadLeft);
    button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, p.buttons & kDpadRight);
    // a trigger's joystick axis rests at its minimum
    auto axis = [&](SDL_GamepadAxis a, int value) { SDL_SetJoystickVirtualAxis(v->joystick, int(a), int16_t(value)); };
    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768 + p.analog[kLeftTrigger] * 257);
    axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768 + p.analog[kRightTrigger] * 257);
    axis(SDL_GAMEPAD_AXIS_LEFTX, p.thumb[0]);
    axis(SDL_GAMEPAD_AXIS_LEFTY, -1 - p.thumb[1]);
    axis(SDL_GAMEPAD_AXIS_RIGHTX, p.thumb[2]);
    axis(SDL_GAMEPAD_AXIS_RIGHTY, -1 - p.thumb[3]);
    SDL_UpdateJoysticks();  // into the gamepad's state now, for the read that follows
    if (g_async) Publish();  // and into the snapshot (self tests only: SDL calls on the guest's thread)
    CostDone(kCostFeed, t, slot);
}

}  // namespace hle::host
