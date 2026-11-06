// Rtl*: strings, critical sections, time conversion, SHA-1 and exceptions.
#include <windows.h>

#include "kernel.h"

std::string read_cstr(u32 addr) {
    std::string s;
    if (!addr) return s;
    for (u8 ch; (ch = rd8(addr)) != 0; addr++) s += (char)ch;
    return s;
}

std::string read_ansi_string(u32 str) {
    if (!str) return {};
    u16 len = rd16(str);
    u32 buf = rd32(str + 4);
    return buf ? std::string((const char*)gptr(buf), len) : std::string();
}

// ------------------------------------------------------------------ strings
static u32 RtlInitAnsiString(Ctx*, KArgs a) {
    u32 dst = a[0], src = a[1];
    u32 n = src ? (u32)strlen((const char*)gptr(src)) : 0;
    wr16(dst, (u16)n);
    wr16(dst + 2, (u16)(src ? n + 1 : 0));
    wr32(dst + 4, src);
    return 0;
}

static u32 RtlInitUnicodeString(Ctx*, KArgs a) {
    u32 dst = a[0], src = a[1], n = 0;
    if (src)
        while (rd16(src + 2 * n)) n++;
    wr16(dst, (u16)(2 * n));
    wr16(dst + 2, (u16)(src ? 2 * n + 2 : 0));
    wr32(dst + 4, src);
    return 0;
}

constexpr u32 STATUS_BUFFER_OVERFLOW = 0x80000005;

static u32 RtlAnsiStringToUnicodeString(Ctx*, KArgs a) {
    u32 dst = a[0], src = a[1];
    u32 n = rd16(src), sbuf = rd32(src + 4);
    u32 need = 2 * n + 2;
    if (a[2] & 0xFF) {
        wr32(dst + 4, pool_alloc(need));
        wr16(dst + 2, (u16)need);
    } else if (rd16(dst + 2) < need) {
        return STATUS_BUFFER_OVERFLOW;
    }
    u32 dbuf = rd32(dst + 4);
    for (u32 i = 0; i < n; i++) wr16(dbuf + 2 * i, rd8(sbuf + i));
    wr16(dbuf + 2 * n, 0);
    wr16(dst, (u16)(2 * n));
    return STATUS_SUCCESS;
}

static u32 RtlUnicodeStringToAnsiString(Ctx*, KArgs a) {
    u32 dst = a[0], src = a[1];
    u32 n = rd16(src) / 2, sbuf = rd32(src + 4);
    if (a[2] & 0xFF) {
        wr32(dst + 4, pool_alloc(n + 1));
        wr16(dst + 2, (u16)(n + 1));
    } else if (rd16(dst + 2) < n + 1) {
        return STATUS_BUFFER_OVERFLOW;
    }
    u32 dbuf = rd32(dst + 4);
    for (u32 i = 0; i < n; i++) {
        u16 ch = rd16(sbuf + 2 * i);
        wr8(dbuf + i, ch < 0x100 ? (u8)ch : '?');
    }
    wr8(dbuf + n, 0);
    wr16(dst, (u16)n);
    return STATUS_SUCCESS;
}

static u32 RtlFreeAnsiString(Ctx*, KArgs a) {
    if (u32 buf = rd32(a[0] + 4)) pool_free(buf);
    wr32(a[0] + 4, 0);
    wr16(a[0], 0);
    wr16(a[0] + 2, 0);
    return 0;
}

static u32 RtlEqualString(Ctx*, KArgs a) {
    std::string x = read_ansi_string(a[0]), y = read_ansi_string(a[1]);
    if (x.size() != y.size()) return 0;
    return (a[2] & 0xFF) ? _strnicmp(x.c_str(), y.c_str(), x.size()) == 0 : x == y;
}

static u32 RtlCompareMemoryUlong(Ctx*, KArgs a) {
    u32 n = 0;
    while (n + 4 <= a[1] && rd32(a[0] + n) == a[2]) n += 4;
    return n;
}

static u32 RtlNtStatusToDosError(Ctx*, KArgs a) {
    switch (a[0]) {
    case STATUS_SUCCESS: return 0;
    case STATUS_OBJECT_NAME_NOT_FOUND:
    case STATUS_NO_SUCH_FILE: return 2;
    case STATUS_OBJECT_PATH_NOT_FOUND: return 3;
    case STATUS_ACCESS_DENIED:
    case STATUS_CANNOT_DELETE:
    case STATUS_FILE_IS_A_DIRECTORY: return 5;
    case STATUS_INVALID_HANDLE: return 6;
    case STATUS_NO_MEMORY: return 8;
    case STATUS_NO_MORE_FILES: return 18;
    case STATUS_UNSUCCESSFUL: return 31;
    case STATUS_END_OF_FILE: return 38;
    case STATUS_INVALID_PARAMETER: return 87;
    case STATUS_DISK_FULL: return 112;
    case STATUS_NOT_IMPLEMENTED: return 120;
    case STATUS_BUFFER_TOO_SMALL: return 122;
    case STATUS_OBJECT_NAME_INVALID: return 123;
    case STATUS_OBJECT_NAME_COLLISION: return 183;
    case STATUS_TIMEOUT: return 258;
    case STATUS_NOT_A_DIRECTORY: return 267;
    case STATUS_MUTANT_NOT_OWNED: return 288;
    case STATUS_PENDING: return 997;
    case STATUS_INVALID_DEVICE_REQUEST: return 1;
    default: return 317;  // ERROR_MR_MID_NOT_FOUND
    }
}

// --------------------------------------------------------- critical sections
// Xbox RTL_CRITICAL_SECTION: a 16-byte dispatcher header (synchronization
// event), then LockCount, RecursionCount, OwningThread as on NT. The wait is
// a scheduler wait on OwningThread becoming free rather than on the event.
constexpr u32 CS_LOCKCOUNT = 0x10, CS_RECURSION = 0x14, CS_OWNER = 0x18;

static u32 RtlInitializeCriticalSection(Ctx*, KArgs a) {
    u32 cs = a[0];
    wr8(cs + DH_TYPE, EventSynchronizationObject);
    wr8(cs + 2, 4);
    wr32(cs + DH_SIGNAL, 0);
    wr32(cs + 8, cs + 8);
    wr32(cs + 12, cs + 8);
    wr32(cs + CS_LOCKCOUNT, 0xFFFFFFFFu);
    wr32(cs + CS_RECURSION, 0);
    wr32(cs + CS_OWNER, 0);
    return 0;
}

static u32 RtlEnterCriticalSection(Ctx*, KArgs a) {
    u32 cs = a[0], me = cur_thread()->kthread;
    wr32(cs + CS_LOCKCOUNT, rd32(cs + CS_LOCKCOUNT) + 1);
    if (rd32(cs + CS_OWNER) == me) {
        wr32(cs + CS_RECURSION, rd32(cs + CS_RECURSION) + 1);
        return 0;
    }
    sched_wait([&] { return rd32(cs + CS_OWNER) == 0; }, -1);
    wr32(cs + CS_OWNER, me);
    wr32(cs + CS_RECURSION, 1);
    return 0;
}

static u32 RtlLeaveCriticalSection(Ctx*, KArgs a) {
    u32 cs = a[0];
    u32 rec = rd32(cs + CS_RECURSION) - 1;
    wr32(cs + CS_RECURSION, rec);
    wr32(cs + CS_LOCKCOUNT, rd32(cs + CS_LOCKCOUNT) - 1);
    if (rec == 0) {
        wr32(cs + CS_OWNER, 0);
        sched_signal();
    }
    return 0;
}

// --------------------------------------------------------------------- time
static u32 RtlTimeToTimeFields(Ctx*, KArgs a) {
    u64 t = rd64(a[0]);
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    u32 f = a[1];
    wr16(f + 0, st.wYear);
    wr16(f + 2, st.wMonth);
    wr16(f + 4, st.wDay);
    wr16(f + 6, st.wHour);
    wr16(f + 8, st.wMinute);
    wr16(f + 10, st.wSecond);
    wr16(f + 12, st.wMilliseconds);
    wr16(f + 14, st.wDayOfWeek);
    return 0;
}

static u32 RtlTimeFieldsToTime(Ctx*, KArgs a) {
    u32 f = a[0];
    SYSTEMTIME st{};
    st.wYear = rd16(f + 0);
    st.wMonth = rd16(f + 2);
    st.wDay = rd16(f + 4);
    st.wHour = rd16(f + 6);
    st.wMinute = rd16(f + 8);
    st.wSecond = rd16(f + 10);
    st.wMilliseconds = rd16(f + 12);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    wr64(a[1], ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    return 1;
}

// -------------------------------------------------------------------- SHA-1
// State kept in the caller's context buffer: h[5], total length, block.
struct ShaCtx {
    u32 h[5];
    u64 len;
    u8 block[64];
};

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha_block(ShaCtx& s, const u8* p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    s.h[0] += a, s.h[1] += b, s.h[2] += c, s.h[3] += d, s.h[4] += e;
}

static void sha_update(ShaCtx& s, const u8* p, u32 n) {
    for (u32 i = 0; i < n; i++) {
        s.block[s.len++ % 64] = p[i];
        if (s.len % 64 == 0) sha_block(s, s.block);
    }
}

static u32 XcSHAInit(Ctx*, KArgs a) {
    ShaCtx s{{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0, {}};
    memcpy(gptr(a[0]), &s, sizeof(s));
    return 0;
}

static u32 XcSHAUpdate(Ctx*, KArgs a) {
    ShaCtx s;
    memcpy(&s, gptr(a[0]), sizeof(s));
    sha_update(s, gptr(a[1]), a[2]);
    memcpy(gptr(a[0]), &s, sizeof(s));
    return 0;
}

static u32 XcSHAFinal(Ctx*, KArgs a) {
    ShaCtx s;
    memcpy(&s, gptr(a[0]), sizeof(s));
    u64 bits = s.len * 8;
    u8 pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while (s.len % 64 != 56) sha_update(s, &pad, 1);
    u8 lenbe[8];
    for (int i = 0; i < 8; i++) lenbe[i] = (u8)(bits >> (56 - 8 * i));
    sha_update(s, lenbe, 8);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) wr8(a[1] + 4 * i + k, (u8)(s.h[i] >> (24 - 8 * k)));
    return 0;
}

// --------------------------------------------------------------- exceptions
// ------------------------------------------------------------- exceptions
// Structured exception dispatch as ntdll does it: the handlers on the fs:[0]
// chain are guest code (MSVC's __CxxFrameHandler among them) and do the real
// work. EXCEPTION_RECORD: code, flags, chained record, address, parameter
// count, parameters. Registration frame: next, handler.
constexpr u32 EXCEPTION_NONCONTINUABLE_ = 1, EXCEPTION_UNWINDING_ = 2, EXCEPTION_EXIT_UNWIND_ = 4, EXCEPTION_NESTED_CALL_ = 0x10;
enum { ContinueExecution, ContinueSearch, NestedException, CollidedUnwind };
constexpr u32 EH_CHAIN_END = 0xFFFFFFFFu;
constexpr u32 CONTEXT_BYTES = 0x2D0, DISPATCHER_CONTEXT_BYTES = 4, RECORD_BYTES = 0x50;
constexpr u32 CXX_EXCEPTION = 0xE06D7363, STATUS_UNWIND_ = 0xC0000027;
static bool g_trace_eh = getenv("NL_TRACE_EH") != nullptr;

static std::string exception_detail(u32 rec) {
    u32 code = rd32(rec), nparams = rd32(rec + 16);
    if (code != CXX_EXCEPTION || nparams < 3) return "";
    // MSVC C++ throw: params are magic, object, ThrowInfo. Name the type from
    // ThrowInfo -> CatchableTypeArray -> first CatchableType -> TypeDescriptor.
    // A rethrow has no ThrowInfo.
    u32 object = rd32(rec + 20 + 4), throw_info = rd32(rec + 20 + 8);
    if (!throw_info) return " C++ rethrow";
    u32 cta = rd32(throw_info + 12);
    u32 ct = rd32(cta + 4);
    u32 td = rd32(ct + 4);
    std::string name = read_cstr(td + 8), detail = " C++ exception of type " + name;
    if (name == ".H") detail += " value " + std::to_string((s32)rd32(object));
    if (name == ".PAG" || name == ".PBG") {
        // Unreal throws TCHAR strings (16-bit here).
        u32 text = rd32(object);
        detail += " \"";
        for (u32 i = 0; i < 200 && rd16(text + 2 * i); i++) detail += (char)rd16(text + 2 * i);
        detail += "\"";
    }
    return detail;
}

// Call an EXCEPTION_ROUTINE (cdecl).
static u32 call_handler(Ctx* c, u32 frame, u32 rec, u32 context, u32 dispatcher) {
    return guest_call(c, rd32(frame + 4), {rec, frame, context, dispatcher}, false);
}

static u32 RtlRaiseException(Ctx* c, KArgs a) {
    u32 rec = a[0];
    if (g_trace_eh)
        log_msg("[eh] raise %08X at %08X%s, return address %08X", rd32(rec), rd32(rec + 12), exception_detail(rec).c_str(),
                rd32(c->esp));
    // Handlers run below the raiser's stack; they get a blank context, which
    // MSVC's handlers do not look at.
    u32 saved_esp = c->esp;
    c->esp -= CONTEXT_BYTES + DISPATCHER_CONTEXT_BYTES;
    u32 context = c->esp, dispatcher = c->esp + CONTEXT_BYTES;
    memset(gptr(context), 0, CONTEXT_BYTES + DISPATCHER_CONTEXT_BYTES);
    u32 nested_end = 0;
    for (u32 frame = rd32(c->fs_base); frame != EH_CHAIN_END; frame = rd32(frame)) {
        u32 r = call_handler(c, frame, rec, context, dispatcher);
        if (frame == nested_end) {
            wr32(rec + 4, rd32(rec + 4) & ~EXCEPTION_NESTED_CALL_);
            nested_end = 0;
        }
        if (r == ContinueExecution) {
            if (rd32(rec + 4) & EXCEPTION_NONCONTINUABLE_) break;
            c->esp = saved_esp;
            return 0;
        }
        if (r == NestedException) {
            wr32(rec + 4, rd32(rec + 4) | EXCEPTION_NESTED_CALL_);
            nested_end = rd32(dispatcher);
        } else if (r != ContinueSearch) {
            break;
        }
    }
    c->esp = saved_esp;
    guest_backtrace(c);
    fatal("unhandled guest exception %08X at %08X%s (return address %08X)", rd32(rec), rd32(rec + 12),
          exception_detail(rec).c_str(), rd32(c->esp));
}

// (TargetFrame, TargetIp, ExceptionRecord, ReturnValue): call the handlers of
// the frames above the target with the unwinding flag and unlink them. Its
// only caller passes its own return address as TargetIp, so this just returns.
static u32 RtlUnwind(Ctx* c, KArgs a) {
    u32 target = a[0], rec = a[2], value = a[3];
    u32 saved_esp = c->esp;
    c->esp -= CONTEXT_BYTES + DISPATCHER_CONTEXT_BYTES + RECORD_BYTES;
    u32 context = c->esp, dispatcher = context + CONTEXT_BYTES;
    memset(gptr(context), 0, CONTEXT_BYTES + DISPATCHER_CONTEXT_BYTES + RECORD_BYTES);
    if (!rec) {
        rec = dispatcher + DISPATCHER_CONTEXT_BYTES;
        wr32(rec, STATUS_UNWIND_);
    }
    wr32(rec + 4, rd32(rec + 4) | EXCEPTION_UNWINDING_ | (target ? 0 : EXCEPTION_EXIT_UNWIND_));
    u32 frame = rd32(c->fs_base);
    while (frame != EH_CHAIN_END && frame != target) {
        u32 r = call_handler(c, frame, rec, context, dispatcher);
        if (r == CollidedUnwind) frame = rd32(dispatcher);
        else if (r != ContinueSearch) fatal("RtlUnwind: handler returned %u", r);
        frame = rd32(frame);
        wr32(c->fs_base, frame);
    }
    c->esp = saved_esp;
    return value;
}

void kernel_register_rtl() {
    kreg(289, RtlInitAnsiString);
    kreg(290, RtlInitUnicodeString);
    kreg(260, RtlAnsiStringToUnicodeString);
    kreg(308, RtlUnicodeStringToAnsiString);
    kreg(286, RtlFreeAnsiString);
    kreg(279, RtlEqualString);
    kreg(269, RtlCompareMemoryUlong);
    kreg(301, RtlNtStatusToDosError);
    kreg(291, RtlInitializeCriticalSection);
    kreg(277, RtlEnterCriticalSection);
    kreg(294, RtlLeaveCriticalSection);
    kreg(305, RtlTimeToTimeFields);
    kreg(304, RtlTimeFieldsToTime);
    kreg(335, XcSHAInit);
    kreg(336, XcSHAUpdate);
    kreg(337, XcSHAFinal);
    kreg(302, RtlRaiseException);
    kreg(312, RtlUnwind);
}
