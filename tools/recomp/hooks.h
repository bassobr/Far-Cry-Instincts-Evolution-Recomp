// Guest functions the runtime replaces with host code (src/hle). For each, codegen emits f_<address> as a
// call to hle::<name>, and the recompiled original as o_<address> so the host side can still use it.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace recomp {

struct Hook {
    uint32_t address;
    const char* name;
};

inline constexpr Hook kHooks[] = {
    // XPP (XDK 5849): controllers are host devices instead of USB devices behind the OHCI stack
    // (Far Cry Instincts Evolution 55530060: XbSymbolDatabase, XInputPoll by signature)
    {0x004720C7, "XGetDevices"},
    {0x004720E9, "XGetDeviceChanges"},
    {0x00478C08, "XInputOpen"},
    {0x00478C5E, "XInputClose"},
    {0x00478C6A, "XInputGetState"},
    {0x00478CDD, "XInputSetState"},
    {0x004794A0, "XInputPoll"},
    // ---- PC comfort features: skip_intro (src/hle/skip_intro.cpp) ----
    {0x0045CC90, "BinkOpen"},  // BinkOpen(name, flags), stdcall, called by the game's video player 0x00196D41 (FCE)
    // ---- menu videos at their own frame rate (src/hle/video_pace.cpp, GitHub #13) ----
    {0x00196AD6, "MenuVideoUpdate"},  // a menu background video's update, once per game frame (FCE; FCI 0x0021FFA0)
    {0x0045E4D0, "BinkWait"},         // BinkWait(bink), stdcall: the update asks it for videos below 25 fps
};

// The CRT's _setjmp3. longjmp ends in a jump to the address after a call to it, in the middle of the caller
// (Lua's protected calls: a script error unwinds to luaD_rawrunprotected). Codegen puts a host setjmp after
// each call, registered under that address and the stack pointer, and rt::Call lands there.
inline constexpr uint32_t kSetjmp = 0x003B587C;  // FCE, by signature (FCI 0x00500030)

// The CRT's _ftol2: st0 truncated toward zero as a 64-bit integer in edx:eax (the integer indefinite when out of
// range), st0 popped; the code of every (int)float. Calls to it are emitted inline (Codegen::IsFtol2 checks the code
// first): its result is exactly that for any rounding mode.
inline constexpr uint32_t kFtol2 = 0x0039ED10;  // FCE, by signature (FCI 0x004ECC18)

// symbols of the XBE (config/symbols.txt, config/symbols_fci.txt): name -> address, functions and data
struct Symbol {
    uint32_t address;
    bool function;
};
inline std::map<std::string, Symbol>& Symbols() {
    static std::map<std::string, Symbol> symbols;
    return symbols;
}

// replacements that are off: address 0 above or a symbol at address 0 (not found in this XBE). Their host
// functions are still declared and exported (the executable builds the same), the game code keeps the original.
inline std::set<std::string>& OffHooks() {
    static std::set<std::string> off;
    return off;
}

// guest functions replaced by host code: the static ones above and the symbols config/hle.txt names
inline std::map<uint32_t, std::string>& Hooks() {
    static std::map<uint32_t, std::string> hooks = [] {
        std::map<uint32_t, std::string> h;
        for (const auto& hook : kHooks)
            if (hook.address) h[hook.address] = hook.name;
            else OffHooks().insert(hook.name);
        return h;
    }();
    return hooks;
}

inline const char* HookName(uint32_t address) {
    auto it = Hooks().find(address);
    return it == Hooks().end() ? nullptr : it->second.c_str();
}

}  // namespace recomp
