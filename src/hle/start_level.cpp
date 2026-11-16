// FCI_START_LEVEL=<level>[:<mission>[:<players>]] (debug switch): the level loads right after boot.
//
// The kernel side (k_launch.cpp) boots the game with launch data "-map <level> [-lpc<players>]", the game's own
// developer path: its main loop (FCE 0x000122A9; FCI 0x00012270) turns that into the game command
// "StartLevel level=<level> mission=<level> [localplayers=<n>]" and pushes it with 0x00012050 (FCE: game object
// and command line on the stack, ret 8; FCI 0x00012000: command line in edx, game object on the stack, ret 4)
// onto the command queue at game+0x3BC (FCI +0x3A4). The command
// handler (0x0004C260, "StartLevel" branch 0x0004C70B) reads the keys level, mission, servertype (listen /
// dedicated), checkpoint and localplayers. The main loop always passes the level name as mission; when the
// setting names a mission (single player levels: Mission0; multiplayer maps: FFA, TDM, CTF; the map editor
// templates: Daytime, ...), this hook replaces the first StartLevel command with one that carries it.
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/memory.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return false; }

// The profile manager ([0x00593630]; FCI [0x006DB630], constructed at 0x00050760, vtable 0x0060C254) holds the
// signed-in profile at +0x14. The level start reads it unchecked (FCI 0x00057A80: the checkpoint save of the profile,
// strlen of its name at +2), and on the "-map" path nobody signed in, so the game crashed at a null name.
// 0x001210FB (FCE; FCI 0x0018E330; stdcall, manager; ret 4) is the game's own "sign in the first saved profile" (used
// by multiplayer at 0x000F9079/0x001227B2; FCI 0x0014B3ED/0x00190C76); without any saved profile 0x00120D81 (FCE;
// FCI 0x0018DDB0; stdcall: manager, name as std::wstring, select; ret 0xC) creates one, as the profile menu does
// (0x0012325E; FCI 0x00192262).
constexpr uint32_t kProfileManager = 0x00593630, kCurrentProfile = 0x14;
constexpr uint32_t kSignInFirstProfile = 0x001210FB, kCreateProfile = 0x00120D81;

// FCE: the command text is the second stack argument of Game_PushCommand (FCI: edx)
uint32_t CommandText(const rt::Cpu& c) { return rt::R32(c.esp + 8); }
void SetCommandText(rt::Cpu& c, uint32_t text) { rt::W32(c.esp + 8, text); }

// VC7.1 std::string in guest memory: allocator, 16 bytes in place or a pointer, size, capacity
std::string GuestString(uint32_t s) {
    uint32_t size = rt::R32(s + 0x14), cap = rt::R32(s + 0x18);
    if (size > 256) return "?";
    uint32_t text = cap >= 16 ? rt::R32(s + 4) : s + 4;
    std::string r;
    for (uint32_t i = 0; i < size; i++) r += char(rt::R8(text + i));
    return r;
}

// the multiplayer map list the lobby fills ([0x006B2D90]: entries of 0x70 bytes at +8..+0xC, chosen one +0x14)
// FCE: [0x0056A8B0], passed at 0x000AE521 to the EntityClassPath reader 0x000C3C01, whose game type getter 0x000C3BE4
// reads the same entries (+0x14 x 0x70 + [+8] + 0x54) as FCI's 0x000FE0A0 (FCI 0x006B2D90)
constexpr uint32_t kMapList = 0x0056A8B0, kMapEntrySize = 0x70;
void DumpMapList() {
    uint32_t list = rt::R32(kMapList);
    if (!list) return rt::Log("start level: no map list");
    uint32_t begin = rt::R32(list + 8), end = rt::R32(list + 0xC), index = rt::R32(list + 0x14);
    rt::Log("start level: map list %08X, entries %08X..%08X, chosen %u", list, begin, end, index);
    for (uint32_t e = begin, i = 0; e && e < end && i < 64; e += kMapEntrySize, i++)
        rt::Log("  %u: '%s' '%s' '%s' '%s'", i, GuestString(e).c_str(), GuestString(e + 0x1C).c_str(),
                GuestString(e + 0x38).c_str(), GuestString(e + 0x54).c_str());
}

// Split screen: with more than one local player (or online) the level start loads
// Scripts\Multiplayer\MultiplayerConfig.lua and its EntityClassPath[<game type>] (0x000FE0D0), the game type
// being the chosen entry of the list above (entries: mission name +0x1C, script folder +0x38, game type +0x54;
// CTF StealTheSample, FFA Chaos, ASSAULT, TDM VitruvianHunt, FFA Chaos, Default). The lobby picks the entry; on
// the "-map" path the choice stays at Default, the table has no such key and the game loaded a script named
// NULL (crash at 0x0028A830). Picks the entry of the mission.
void ChooseGameType(const std::string& mission) {
    uint32_t list = rt::R32(kMapList);
    if (!list) return;
    uint32_t begin = rt::R32(list + 8), end = rt::R32(list + 0xC);
    for (uint32_t e = begin, i = 0; e && e < end && i < 64; e += kMapEntrySize, i++) {
        std::string name = GuestString(e + 0x1C);
        if (_stricmp(name.c_str(), mission.c_str()) == 0) {
            rt::W32(list + 0x14, i);
            rt::Log("start level: game type %u '%s' (%s) for split screen", i, GuestString(e + 0x54).c_str(),
                    name.c_str());
            return;
        }
    }
    rt::Log("start level: no game type for mission '%s'", mission.c_str());
}

uint32_t CurrentProfile() {
    uint32_t mgr = rt::R32(kProfileManager);
    return mgr ? rt::R32(mgr + kCurrentProfile) : 0;
}

void EnsureProfile(rt::Cpu& c) {
    uint32_t mgr = rt::R32(kProfileManager);
    if (!mgr || CurrentProfile()) return;
    uint32_t regs[8] = {c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi};
    uint32_t signed_in = rt::GuestCall(c, kSignInFirstProfile, {mgr}) & 0xFF;
    rt::Log("start level: sign-in of the first saved profile: %s", signed_in && CurrentProfile() ? "done" : "none");
    if (!CurrentProfile()) {
        // std::wstring (VC7.1): allocator, 8 wchar_t in place, size, capacity
        c.esp -= 0x40;
        uint32_t name = c.esp + 0x20;
        std::memset(rt::Host(name), 0, 0x1C);
        const wchar_t* text = L"Tester";
        for (int i = 0; text[i]; i++) rt::W16(name + 4 + 2 * i, uint16_t(text[i]));
        rt::W32(name + 0x14, 6);
        rt::W32(name + 0x18, 7);
        rt::GuestCall(c, kCreateProfile, {mgr, name, 1});
        rt::Log("start level: no saved profile, created 'Tester'");
    }
    c.eax = regs[0], c.ecx = regs[1], c.edx = regs[2], c.ebx = regs[3];
    c.esp = regs[4], c.ebp = regs[5], c.esi = regs[6], c.edi = regs[7];
    uint32_t profile = CurrentProfile();
    rt::Log("start level: signed in profile %08X '%s'", profile,
            profile ? [&] {
                std::string n;
                for (uint32_t a = profile + 2; rt::R16(a) && n.size() < 32; a += 2) n += char(rt::R16(a));
                return n;
            }().c_str()
                    : "(none)");
}

// FCE: the difficulty (game [0x0056B594] + 0x3D4, read by Game:GetDifficultyLevel 0x0008C10E; 0 easy .. 3 extreme)
// starts at -1 = not chosen (the game's constructor, 0x00039849). The menus choose it: Story asks while it is -1
// (0x000FC1B9) and keeps the choice in the profile (+0x180, 0x000FC9C7; copied to the game when a profile signs in,
// 0x0012155E), and the Tutorial button sets medium when it is still -1 (0x000FB157). On the "-map" path neither runs
// (a new profile has -1 as well), and GameRules looks up difficulty_level_damage_multipliers[difficulty + 1] = nil: its
// OnDamage fails and the player takes no damage (falls, drowning). Medium, as the Tutorial button does. (FCI: the
// constructor sets 0, game [0x006B3984] + 0x3BC, so FCI needs nothing here.)
void EnsureDifficulty() {
    constexpr uint32_t kGame = 0x0056B594, kDifficulty = 0x3D4, kMedium = 1;
    const uint32_t game = rt::R32(kGame);
    if (!game || int32_t(rt::R32(game + kDifficulty)) != -1) return;
    rt::W32(game + kDifficulty, kMedium);
    rt::Log("start level: no difficulty chosen, medium (as the Tutorial button)");
}

// FCE: the input manager ([0x0056B4F0]) maps controller ports to players: +0x464 + 4 * player = its port, +0x474 +
// 4 * port = its player (-1 = none), +0x498 = how many; +0x488 = the port that pressed Start at "PRESS START" (-1 until
// then). The PRESS START handler (0x000D428A) sets it, and the level starts (0x00048E58, 0x00049020, 0x00049319,
// 0x000FC3CC, 0x000FCB2C) give player 1 that port (0x0017AFB3: eax = manager, ecx = port, edi = player; nothing for
// port -1); a relaunch between levels carries it in the game's own launch data (+0x10C, restored at 0x00039A6F). On the
// "-map" path nobody pressed Start: player 1 had no port, and the menus inside the level (the tutorial's welcome
// dialog, "Switch resume nosound" from 0x000F9BF4) never saw a button, so the level stayed paused. Port 0, as if
// Start had been pressed there (keyboard and the first controller). (FCI's start-level path has no such step.)
void EnsureControllerPort() {
    constexpr uint32_t kInput = 0x0056B4F0, kStartPort = 0x488;
    const uint32_t input = rt::R32(kInput);
    if (!input || rt::R32(input + kStartPort) < 4) return;
    rt::W32(input + kStartPort, 0);
    rt::Log("start level: no controller pressed Start, port 0 (as PRESS START there)");
}

// Split screen: each local player i takes the profile signed in on its controller port,
// [manager + 0x18 + 4 * port] with port = [game + 0x40 + 4 * i] (0x000EEF27; one player: manager + 0x14). The
// lobby signs in every joining player; here they all get the one profile (the game reads its name unchecked,
// 0x000F0101, and crashed at a null one).
void ShareProfile() {
    uint32_t mgr = rt::R32(kProfileManager), profile = CurrentProfile();
    if (!mgr || !profile) return;
    for (uint32_t port = 0; port < 4; port++)
        if (!rt::R32(mgr + 0x18 + 4 * port)) rt::W32(mgr + 0x18 + 4 * port, profile);
    rt::Log("start level: profile %08X on all controller ports (split screen)", profile);
}

// FCI_TEST_COMMAND_DELAY=<seconds> (tests): the game object of the level start whose FCI_TEST_COMMAND is still due
uint32_t g_delayed_game = 0;

// FCI_TEST_COMMAND after the level start command (FCE: on the stack; FCI: in edx; pushed first): at once, or with FCI_TEST_COMMAND_DELAY
// later by StartLevelTick; false = no test command
bool QueueTestCommand(rt::Cpu& c, uint32_t* result, const char* why) {
    char test[200];
    DWORD n = GetEnvironmentVariableA("FCI_TEST_COMMAND", test, sizeof test);
    if (!n || n >= sizeof test) return false;
    if (GetEnvironmentVariableA("FCI_TEST_COMMAND_DELAY", nullptr, 0) > 0) {
        g_delayed_game = rt::R32(c.esp + 4);
        *result = xdk::CallOriginal(c, GUEST_ORIG(Game_PushCommand));
        return true;
    }
    uint32_t text = CommandText(c);
    *result = xdk::CallOriginal(c, GUEST_ORIG(Game_PushCommand));
    std::memcpy(rt::Host(text), test, n + 1);
    SetCommandText(c, text);
    rt::Log("start level: test command '%s'%s", test, why);
    xdk::CallOriginal(c, GUEST_ORIG(Game_PushCommand));
    return true;
}
}  // namespace

// FCI_TEST_COMMAND_DELAY=<seconds> (tests): FCI_TEST_COMMAND only after that many seconds of the level (the physics
// step asks every frame), so with FCI_TEST_COMMAND_REPEAT each process of a restart chain plays that long
void xdk::StartLevelTick(rt::Cpu& c) {
    if (!g_delayed_game) return;
    static const double delay = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("FCI_TEST_COMMAND_DELAY", v, sizeof v) ? std::atof(v) : 0.0;
    }();
    if (xdk::LevelSeconds() < delay) return;
    char test[200];
    DWORD n = GetEnvironmentVariableA("FCI_TEST_COMMAND", test, sizeof test);
    static const uint32_t text = mem::Contiguous().Alloc(0x1000);
    const uint32_t game = g_delayed_game;
    g_delayed_game = 0;
    if (!n || n >= sizeof test || !text) return;
    std::memcpy(rt::Host(text), test, n + 1);
    const rt::Cpu saved = c;
    rt::GuestCall(c, SYM(Game_PushCommand), {game, text});  // FCE: both on the stack (FCI: text in edx)
    c = saved;
    rt::Log("start level: test command '%s' after %.1f s of the level", test, xdk::LevelSeconds());
}

// 0x00012050: push a game command ([esp+4] = game, [esp+8] = text), ret 8 (FCI 0x00012000: edx = text, [esp+4] =
// game, ret 4)
HLE_STDCALL(Game_PushCommand, 2) {
    static bool done = false;
    // FCI_TRACE_COMMANDS=1: every game command (menu actions, level start, reboot requests)
    static const bool trace = GetEnvironmentVariableA("FCI_TRACE_COMMANDS", nullptr, 0) > 0;
    if (trace) {
        uint32_t profiles = rt::R32(0x00593630);  // FCE (FCI 0x006DB630)
        rt::Log("game command '%s' (profile manager %08X, profile %08X)", kernel::ReadCString(CommandText(c)).c_str(),
                profiles, profiles ? rt::R32(profiles + 0x14) : 0);
    }
    // FCI_TEST_COMMAND_ON=<prefix> (tests): FCI_TEST_COMMAND replaces the first game command that starts with <prefix>
    // instead of following the level start, e.g. "LoadGame" = when "Load savepoint" is chosen in the pause menu (so
    // a level change can follow what was done in the level before: settings, the game's options)
    {
        static bool replaced = false;
        char on[64], test[200];
        DWORD m = GetEnvironmentVariableA("FCI_TEST_COMMAND_ON", on, sizeof on);
        DWORD n = GetEnvironmentVariableA("FCI_TEST_COMMAND", test, sizeof test);
        if (!replaced && m && m < sizeof on && n && n < sizeof test && kernel::ReadCString(CommandText(c)).rfind(on, 0) == 0 &&
            GetEnvironmentVariableA("FCI_LAUNCH_DATA", nullptr, 0) == 0) {
            replaced = true;
            static const uint32_t text = mem::Contiguous().Alloc(0x1000);
            if (text) {
                rt::Log("start level: game command '%s' replaced by the test command '%s'",
                        kernel::ReadCString(CommandText(c)).c_str(), test);
                std::memcpy(rt::Host(text), test, n + 1);
                SetCommandText(c, text);
                return xdk::CallOriginal(c, GUEST_ORIG(Game_PushCommand));
            }
        }
    }
    std::string level, mission;
    int players = 0;
    // a relaunch (kernel::Relaunch: the game's reboot between levels, "-map <level>") boots the same way
    static const bool relaunched = GetEnvironmentVariableA("FCI_LAUNCH_DATA", nullptr, 0) > 0;
    if (!done && relaunched) {
        std::string cmd = kernel::ReadCString(CommandText(c));
        if (cmd.rfind("StartLevel level=", 0) == 0) {
            done = true;
            EnsureProfile(c);
            // a level change of an FCI_START_LEVEL session (the profile of the first process has no difficulty)
            if (GetEnvironmentVariableA("FCI_START_LEVEL", nullptr, 0) > 0) EnsureDifficulty(), EnsureControllerPort();
            // FCI_TEST_COMMAND_REPEAT=1 (tests): FCI_TEST_COMMAND in every relaunched process too, a chain of level
            // starts (FadeOutRestartStartLevel into the same level: one start every ~15 s without the menus)
            uint32_t r;
            if (GetEnvironmentVariableA("FCI_TEST_COMMAND_REPEAT", nullptr, 0) > 0 &&
                QueueTestCommand(c, &r, " (relaunched)"))
                return r;
        }
    }
    if (!done && kernel::StartLevelSetting(&level, &mission, &players)) {
        std::string cmd = kernel::ReadCString(CommandText(c));
        // the main loop's command for the launch data (not one from the menus)
        std::string boot = "StartLevel level=" + level + " mission=" + level;
        if (cmd.rfind(boot, 0) == 0) {
            done = true;
            EnsureProfile(c);
            EnsureDifficulty();
            EnsureControllerPort();
            if (trace) DumpMapList();
            if (players >= 2 && !mission.empty()) ChooseGameType(mission);
            if (players >= 2) ShareProfile();
            if (!mission.empty()) {
                std::string mine = "StartLevel level=" + level + " mission=" + mission;
                if (players >= 2 && players <= 4) mine += " localplayers=" + std::to_string(players);
                // the main loop's buffer is 256 bytes (FCE [ebp+0x1D38] up to [ebp+0x1E38]; FCI 248 bytes, [esp+0x70]
                // up to the launch data copy at [esp+0x168])
                if (mine.size() < 200) {
                    std::memcpy(rt::Host(CommandText(c)), mine.c_str(), mine.size() + 1);
                    cmd = mine;
                }
            }
            rt::Log("start level: game command '%s'", cmd.c_str());
            // FCI_TEST_COMMAND=<game command>: queued right after the level start (test of the game's own
            // reboot paths, e.g. "FadeOutRestartStartLevel level=02_Native" = the reboot between levels)
            uint32_t r;
            if (GetEnvironmentVariableA("FCI_TEST_COMMAND_ON", nullptr, 0) == 0 && QueueTestCommand(c, &r, ""))
                return r;
        }
    }
    return xdk::CallOriginal(c, GUEST_ORIG(Game_PushCommand));
}
