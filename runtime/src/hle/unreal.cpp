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


// CRT _JumpToContinuation(void* target, EHRegistrationNode* frame): leave a
// finished catch block. Drop the top EH frame, restore the frame's esp (saved
// just below it) and ebp (the frame sits 12 bytes under it), and go on at the
// continuation, which is in an outer guest function.
void hle_JumpToContinuation(Ctx* c) {
    u32 target = rd32(c->esp + 4), frame = rd32(c->esp + 8);
    wr32(c->fs_base, rd32(rd32(c->fs_base)));
    c->esp = rd32(frame - 4);
    c->ebp = frame + 12;
    throw GuestResume{target};
}
