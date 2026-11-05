// Host-side hooks into the game's Unreal engine code (see config/overrides.txt).
#include <string>

#include "funcs.h"
#include "runtime.h"

// Unreal on Xbox uses 16-bit TCHARs.
static std::string read_tchar(u32 p) {
    std::string s;
    if (!p) return "(null)";
    for (u16 ch; s.size() < 4096 && (ch = rd16(p)) != 0; p += 2) s += ch < 0x80 ? (char)ch : '?';
    return s;
}

// FOutputDeviceError::Serialize(const TCHAR* Msg, EName Event), thiscall.
// Print the fatal error before the engine reports it and throws.
void hle_unreal_error_serialize(Ctx* c) {
    log_msg("[unreal] appError: %s", read_tchar(rd32(c->esp + 4)).c_str());
    f_00173A90(c);
}

