// Debug output, AV/HAL stubs, EEPROM settings and XBE section loading.
#include <stdlib.h>
#include <windows.h>

#include <string>

#include "audio.h"
#include "kernel.h"

// ------------------------------------------------------------- DbgPrint
// printf with arguments read from the guest stack.
std::string guest_format(u32 fmt, u32 args) {
    std::string out;
    std::string f = read_cstr(fmt);
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%') {
            out += f[i];
            continue;
        }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < f.size() && strchr("-+ #0", f[j])) spec += f[j++];
        while (j < f.size() && (isdigit((u8)f[j]) || f[j] == '.')) spec += f[j++];
        bool wide = false, longlong = false;
        while (j < f.size() && strchr("hlLwI64", f[j])) {
            if (f[j] == 'l' && j + 1 < f.size() && f[j + 1] == 'l') longlong = true;
            if (f[j] == 'w' || f[j] == 'l') wide = true;
            if (f[j] == 'I' && f.compare(j, 3, "I64") == 0) {
                longlong = true;
                j += 2;
            }
            j++;
        }
        if (j >= f.size()) break;
        char conv = f[j];
        char buf[512];
        switch (conv) {
        case '%':
            out += '%';
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (longlong) {
                u64 v = rd64(args);
                args += 8;
                snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), v);
            } else {
                u32 v = rd32(args);
                args += 4;
                snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
            }
            out += buf;
            break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", rd32(args));
            args += 4;
            out += buf;
            break;
        case 'f': case 'g': case 'e': case 'G': case 'E': {
            double v;
            memcpy(&v, gptr(args), 8);
            args += 8;
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
            out += buf;
            break;
        }
        case 's': case 'S': {
            u32 p = rd32(args);
            args += 4;
            if (!p) {
                out += "(null)";
            } else if ((conv == 'S') != wide) {
                for (u32 k = 0; k < 4096 && rd16(p + 2 * k); k++) out += (char)rd16(p + 2 * k);
            } else {
                out += read_cstr(p);
            }
            break;
        }
        default:
            out += spec + conv;
        }
        i = j;
    }
    return out;
}

static u32 DbgPrint(Ctx* c, KArgs) {
    std::string s = guest_format(rd32(c->esp + 4), c->esp + 8);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    log_msg("[dbg] %s", s.c_str());
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------------ AV/HAL
static u32 AvSendTVEncoderOption(Ctx*, KArgs a) {
    constexpr u32 AV_QUERY_AV_CAPABILITIES = 6;
    if (a[3]) wr32(a[3], a[1] == AV_QUERY_AV_CAPABILITIES ? 0x00400101u /* NTSC-M, standard pack, 60 Hz */ : 0);
    return 0;
}
static u32 ret0(Ctx*, KArgs) { return 0; }
static u32 ret_success(Ctx*, KArgs) { return STATUS_SUCCESS; }

static u32 HalReadWritePCISpace(Ctx*, KArgs a) {
    if (!(a[5] & 0xFF) && a[3]) memset(gptr(a[3]), 0, a[4]);
    return 0;
}

// ------------------------------------------------------- quick reboot
// XLaunchNewImage fills LaunchDataPage (kernel export 164) and quick-reboots
// (HalReturnToFirmware(2)); the next boot's XAPI picks the page up. Here a
// reboot is a fresh process that gets the page through a file.
constexpr u32 LAUNCH_DATA_PAGE = 164, PAGE_BYTES = 0x1000;
constexpr u32 RETURN_FIRMWARE_QUICK_REBOOT = 2;

static std::string launch_file() {
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    return std::string(tmp) + "newlegends_launch.bin";
}

void launch_data_restore() {
    const char* f = getenv("NL_LAUNCH_DATA");
    if (!f) return;
    FILE* fp = fopen(f, "rb");
    if (!fp) return;
    u32 page = kernel_contig_alloc(PAGE_BYTES);
    size_t n = fread(gptr(page), 1, PAGE_BYTES, fp);
    fclose(fp);
    remove(f);
    if (n != PAGE_BYTES) return;
    wr32(kernel_data_addr(LAUNCH_DATA_PAGE), page);
    log_msg("launch: relaunched with launch data type %u, path '%s'", rd32(page), (const char*)gptr(page + 8));
}

void host_window_hide();

[[noreturn]] static void relaunch(u32 page) {
    std::string file = launch_file();
    if (FILE* fp = fopen(file.c_str(), "wb")) {
        fwrite(gptr(page), 1, PAGE_BYTES, fp);
        fclose(fp);
    }
    log_msg("launch: quick reboot into '%s' (launch data type %u)", (const char*)gptr(page + 8), rd32(page));
    SetEnvironmentVariableA("NL_LAUNCH_DATA", file.c_str());
    // The new process inherits our console handles; a job ties its lifetime
    // to ours so whoever is waiting on this process sees the whole session.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    STARTUPINFOA si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string cmdline = GetCommandLineA();
    if (!CreateProcessA(exe, cmdline.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        fatal("relaunch failed (error %lu)", GetLastError());
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    // This thread holds the GIL, so no guest code runs here any more; only
    // the window would linger.
    host_window_hide();
    audio_out_stop();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

static u32 HalReturnToFirmware(Ctx*, KArgs a) {
    u32 page = rd32(kernel_data_addr(LAUNCH_DATA_PAGE));
    if (a[0] == RETURN_FIRMWARE_QUICK_REBOOT && page) relaunch(page);
    log_msg("HalReturnToFirmware(%u): game requested reboot/exit", a[0]);
    exit(0);
}
static u32 HalInitiateShutdown(Ctx*, KArgs) {
    log_msg("HalInitiateShutdown");
    exit(0);
}

static u32 KeBugCheck(Ctx*, KArgs a) { fatal("KeBugCheck(%08X)", a[0]); }

// ------------------------------------------------------------------ EEPROM
static u32 ExQueryNonVolatileSetting(Ctx*, KArgs a) {
    u32 index = a[0], type_ptr = a[1], value = a[2], len = a[3], result_len = a[4];
    // winnt.h defines REG_BINARY (3) and REG_DWORD (4) with the same values.
    u32 type = REG_DWORD, dword = 0, size = 4;
    switch (index) {
    case 0x07: dword = 1; break;            // XC_LANGUAGE: English
    case 0x08: dword = 0; break;            // XC_VIDEO flags
    case 0x09: dword = 0; break;            // XC_AUDIO flags
    case 0x0A: dword = 0; break;            // XC_PARENTAL_CONTROL_GAMES: off
    case 0x0C: dword = 0; break;            // XC_PARENTAL_CONTROL_MOVIES
    case 0x11: dword = 0; break;            // XC_MISC
    case 0x12: dword = 1; break;            // XC_DVD_REGION
    case 0x103: dword = 0x00400100; break;  // XC_FACTORY_AV_REGION: NTSC-M
    case 0x104: dword = 1; break;           // XC_FACTORY_GAME_REGION: North America
    case 0x00: case 0x03: case 0x06: dword = 0; break;  // time zone biases
    default:
        type = REG_BINARY;
        size = len;
        break;
    }
    if (type_ptr) wr32(type_ptr, type);
    if (value) {
        memset(gptr(value), 0, len);
        if (type == REG_DWORD && len >= 4) wr32(value, dword);
    }
    if (result_len) wr32(result_len, size);
    return STATUS_SUCCESS;
}

// --------------------------------------------------------------- sections
void xbe_reload_section(u32 header);
constexpr u32 SECTION_REFCOUNT = 0x18;

static u32 XeLoadSection(Ctx*, KArgs a) {
    u32 hdr = a[0];
    u32 n = rd32(hdr + SECTION_REFCOUNT);
    if (n == 0) xbe_reload_section(hdr);
    wr32(hdr + SECTION_REFCOUNT, n + 1);
    return STATUS_SUCCESS;
}

static u32 XeUnloadSection(Ctx*, KArgs a) {
    u32 hdr = a[0];
    u32 n = rd32(hdr + SECTION_REFCOUNT);
    if (n == 0) return STATUS_INVALID_PARAMETER;
    // Memory stays mapped; only the count matters to the game.
    wr32(hdr + SECTION_REFCOUNT, n - 1);
    return STATUS_SUCCESS;
}

static u32 IoCreateDevice(Ctx*, KArgs a) {
    u32 dev = kernel_alloc(0x100 + a[1]);
    if (a[5]) wr32(a[5], dev);
    return STATUS_SUCCESS;
}

void kernel_register_misc() {
    kreg(8, DbgPrint);
    kreg(1, ret0);  // AvGetSavedDataAddress: no saved framebuffer
    kreg(2, AvSendTVEncoderOption);
    kreg(3, ret0);  // AvSetDisplayMode: video is the renderer's business
    kreg(4, ret0);  // AvSetSavedDataAddress
    kreg(24, ExQueryNonVolatileSetting);
    kreg(46, HalReadWritePCISpace);
    kreg(47, ret0);   // HalRegisterShutdownNotification
    kreg(358, ret0);  // HalIsResetOrShutdownPending
    kreg(49, HalReturnToFirmware);
    kreg(360, HalInitiateShutdown);
    kreg(95, KeBugCheck);
    kreg(142, ret_success);  // KeSaveFloatingPointState
    kreg(139, ret_success);  // KeRestoreFloatingPointState
    kreg(327, XeLoadSection);
    kreg(328, XeUnloadSection);
    kreg(65, IoCreateDevice);
    kreg(74, ret_success);  // IoInvalidDeviceRequest
    kreg(81, ret0);         // IoStartNextPacket
    kreg(83, ret0);         // IoStartPacket
    kreg(87, ret0);         // IofCompleteRequest
    kreg(359, ret0);        // IoMarkIrpMustComplete
}
