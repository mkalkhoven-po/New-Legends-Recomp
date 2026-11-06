// Internal interface of the HLE xboxkrnl, shared by runtime/src/kernel/*.cpp.
#pragma once
#include <string>

#include "runtime.h"

// ------------------------------------------------------------------ NTSTATUS
// windows.h defines some of these as macros; ours are the Xbox values.
#undef STATUS_ABANDONED
#undef STATUS_USER_APC
#undef STATUS_ALERTED
#undef STATUS_TIMEOUT
#undef STATUS_PENDING
#undef STATUS_NO_MEMORY
#undef STATUS_INVALID_HANDLE
#undef STATUS_INVALID_PARAMETER
enum : u32 {
    STATUS_SUCCESS = 0,
    STATUS_ABANDONED = 0x80,
    STATUS_USER_APC = 0xC0,
    STATUS_ALERTED = 0x101,
    STATUS_TIMEOUT = 0x102,
    STATUS_PENDING = 0x103,
    STATUS_NO_MORE_FILES = 0x80000006,
    STATUS_UNSUCCESSFUL = 0xC0000001,
    STATUS_NOT_IMPLEMENTED = 0xC0000002,
    STATUS_INFO_LENGTH_MISMATCH = 0xC0000004,
    STATUS_INVALID_HANDLE = 0xC0000008,
    STATUS_INVALID_PARAMETER = 0xC000000D,
    STATUS_NO_SUCH_FILE = 0xC000000F,
    STATUS_INVALID_DEVICE_REQUEST = 0xC0000010,
    STATUS_END_OF_FILE = 0xC0000011,
    STATUS_NO_MEMORY = 0xC0000017,
    STATUS_CONFLICTING_ADDRESSES = 0xC0000018,
    STATUS_ACCESS_DENIED = 0xC0000022,
    STATUS_BUFFER_TOO_SMALL = 0xC0000023,
    STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024,
    STATUS_OBJECT_NAME_INVALID = 0xC0000033,
    STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
    STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
    STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
    STATUS_MUTANT_NOT_OWNED = 0xC0000046,
    STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
    STATUS_DISK_FULL = 0xC000007F,
    STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA,
    STATUS_NOT_A_DIRECTORY = 0xC0000103,
    STATUS_CANNOT_DELETE = 0xC0000121,
};

// ------------------------------------------------------------ call framework
// Arguments of a stdcall kernel function: return address at esp, arg i at
// esp + 4 + 4*i.
struct KArgs {
    Ctx* c;
    u32 operator[](int i) const { return rd32(c->esp + 4 + 4 * i); }
};

// ------------------------------------------------------- scheduler / threads
// Guest threads are host threads, but only one runs guest code at a time
// (the Xbox is a uniprocessor and its code relies on that). The lock is
// released while a thread blocks in the kernel and at yield points the
// recompiler places on loop back-edges.
void gil_lock();
void gil_unlock();

// Wake every thread blocked in a kernel wait so it re-checks its condition.
// Call after any change that could satisfy a wait.
void sched_signal();
// Block (GIL released) until `ready` holds or the deadline passes. `ready` is
// evaluated with the GIL held. deadline_ms < 0 means forever. Returns false
// on timeout.
template <class F>
bool sched_wait(F ready, s64 deadline_ms);
s64 now_ms();

struct GuestThread {
    Ctx ctx;
    u32 kthread;  // guest ETHREAD/KTHREAD address
    u32 kpcr;
    u32 id;
};
GuestThread* cur_thread();
// Bring up a guest thread (KPCR, KTHREAD, TLS, stack) without starting it.
GuestThread* thread_create(u32 stack_size, u32 tls_size);
void thread_bind(GuestThread* t);  // make t the current thread of this host thread

// Call a guest function from the kernel: push args right to left, push a
// return address, run, return eax. Callee pops args only if stdcall.
u32 guest_call(Ctx* c, u32 fn, std::initializer_list<u32> args, bool stdcall);

// Upper bound in milliseconds for a 100ns-unit NT timeout (negative =
// relative, positive = absolute system time, null pointer = infinite).
s64 nt_deadline(u32 timeout_ptr);

// ---------------------------------------------------------------- objects
// Dispatcher header layout (DISPATCHER_HEADER)
constexpr u32 DH_TYPE = 0, DH_SIGNAL = 4;
enum : u8 {
    EventNotificationObject = 0,
    EventSynchronizationObject = 1,
    MutantObject = 2,
    SemaphoreObject = 5,
    ThreadObject = 6,
    TimerNotificationObject = 8,
    TimerSynchronizationObject = 9,
};
constexpr u32 KMUTANT_OWNER = 0x18, KSEMAPHORE_LIMIT = 0x10;
constexpr u32 KTHREAD_TLSDATA = 0x28, ETHREAD_UNIQUETHREAD = 0x12C, ETHREAD_SIZE = 0x200;

// Is the object signaled for the current thread? Consume the signal if so.
bool obj_try_acquire(u32 obj);

// Handles. Kernel objects are guest memory (so the game can read them);
// files and symbolic links carry host-side state.
enum class HType { Object, File, Symlink };
struct HostFile;
struct HandleEntry {
    HType type;
    u32 obj = 0;  // guest object address for Object handles
    HostFile* file = nullptr;
    std::string link;  // target of a symbolic link
};
u32 handle_create(const HandleEntry& e);
HandleEntry* handle_get(u32 h);
bool handle_close(u32 h);
// Guest object address for a handle, including the pseudo handles.
u32 handle_object(u32 h);

// ---------------------------------------------------------------- memory
u32 pool_alloc(u32 size);
void pool_free(u32 addr);
u32 pool_size(u32 addr);

// ---------------------------------------------------------------- strings
std::string read_ansi_string(u32 string_ptr);  // ANSI_STRING / OBJECT_STRING
std::string read_cstr(u32 addr);

// ---------------------------------------------------------------- files
void file_init(const char* game_dir);
void file_close(HostFile* f);

// Kernel-side handlers per area; each registers its ordinals.
using KFunc = u32 (*)(Ctx* c, KArgs a);
void kreg(u32 ordinal, KFunc fn);
void kernel_register_misc();
// Contiguous (0x80000000 | physical) memory for kernel-owned buffers.
u32 kernel_contig_alloc(u32 size);
void kernel_contig_free(u32 addr);
// Guest address of a kernel data export (LaunchDataPage, KeTickCount, ...).
u32 kernel_data_addr(u32 ordinal);
// XLaunchNewImage support: LaunchDataPage survives a quick reboot.
void launch_data_restore();
void kernel_register_memory();
void kernel_register_threads();
void kernel_register_files();
void kernel_register_rtl();

// --------------------------------------------------------- implementation
#include <condition_variable>
#include <mutex>

extern std::mutex g_sig_mutex;
extern std::condition_variable g_sig_cv;
extern u64 g_sig_gen;

template <class F>
bool sched_wait(F ready, s64 deadline_ms) {
    for (;;) {
        // Read the generation before testing: device threads signal without
        // the GIL, and a signal between test and read would be lost.
        u64 gen;
        {
            std::lock_guard<std::mutex> lk(g_sig_mutex);
            gen = g_sig_gen;
        }
        if (ready()) return true;
        if (deadline_ms >= 0 && now_ms() >= deadline_ms) return false;
        gil_unlock();
        {
            std::unique_lock<std::mutex> lk(g_sig_mutex);
            auto pred = [&] { return g_sig_gen != gen; };
            if (deadline_ms < 0) {
                g_sig_cv.wait(lk, pred);
            } else {
                s64 left = deadline_ms - now_ms();
                if (left > 0) g_sig_cv.wait_for(lk, std::chrono::milliseconds(left), pred);
            }
        }
        gil_lock();
    }
}
