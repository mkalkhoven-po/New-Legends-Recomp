// Scheduler, guest threads, dispatcher objects (events, mutants, timers),
// waits, DPCs, IRQL, handles and time.
#include <windows.h>

#include <atomic>
#include <deque>
#include <unordered_map>
#include <vector>

#include "kernel.h"

// ------------------------------------------------------------------ the GIL
// A ticket lock, so a thread that yields really lets the others run before
// it gets the CPU back.
static std::mutex g_gil_m;
static std::condition_variable g_gil_cv;
static u64 g_gil_next, g_gil_serving;

void gil_lock() {
    std::unique_lock<std::mutex> lk(g_gil_m);
    u64 t = g_gil_next++;
    g_gil_cv.wait(lk, [&] { return g_gil_serving == t; });
}

void gil_unlock() {
    {
        std::lock_guard<std::mutex> lk(g_gil_m);
        g_gil_serving++;
    }
    g_gil_cv.notify_all();
}

std::mutex g_sig_mutex;
std::condition_variable g_sig_cv;
u64 g_sig_gen;

void sched_signal() {
    {
        std::lock_guard<std::mutex> lk(g_sig_mutex);
        g_sig_gen++;
    }
    g_sig_cv.notify_all();
}

static LARGE_INTEGER g_qpf, g_qpc0;
static u64 qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)(t.QuadPart - g_qpc0.QuadPart);
}
s64 now_ms() { return (s64)(qpc_now() * 1000 / (u64)g_qpf.QuadPart); }

static u64 system_time_100ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

s64 nt_deadline(u32 timeout_ptr) {
    if (!timeout_ptr) return -1;
    s64 t = (s64)rd64(timeout_ptr);
    if (t < 0) return now_ms() + (-t + 9999) / 10000;
    s64 rel = (t - (s64)system_time_100ns()) / 10000;
    return now_ms() + (rel > 0 ? rel : 0);
}

// ----------------------------------------------------- yield points / ticks
volatile u32 g_yield;
extern u32 g_ke_tick_count_addr;

static u8 cur_irql() { return cur_thread() ? rd8(cur_thread()->kpcr + 0x24) : 0; }

void rt_yield(Ctx*) {
    if (cur_irql() >= 2) return;  // DISPATCH_LEVEL: not preemptible
    g_yield = 0;
    gil_unlock();
    gil_lock();
}

static DWORD WINAPI tick_thread(void*) {
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        wr32(g_ke_tick_count_addr, (u32)now_ms());
        g_yield = 1;
    }
}

// ------------------------------------------------------------------ threads
static thread_local GuestThread* t_cur;
static std::atomic<u32> g_next_thread_id{1};
static std::mutex g_stack_m;
static u32 g_stack_next = RAM_SIZE;

GuestThread* cur_thread() { return t_cur; }
void crash_register_thread(u32 id);
void thread_bind(GuestThread* t) {
    t_cur = t;
    crash_register_thread(t->id);
}

constexpr u32 KPCR_SIZE = 0x300, KPCR_IRQL = 0x24, KPCR_PRCB = 0x20, KPCR_PRCBDATA = 0x28;
const XbeInfo& kernel_xbe();

GuestThread* thread_create(u32 stack_size, u32 tls_size) {
    GuestThread* t = new GuestThread{};
    Ctx* c = &t->ctx;
    c->fcw = 0x037F;
    c->mxcsr = 0x1F80;

    // Stacks are carved from the top of guest RAM downwards, with a gap
    // between them.
    stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
    u32 top;
    {
        std::lock_guard<std::mutex> lk(g_stack_m);
        top = g_stack_next;
        g_stack_next -= stack_size + 0x10000;
        if (g_stack_next < 0x01000000u) fatal("out of guest stack space");
    }

    // TLS: a slot array whose slot 0 points at the module's TLS block right
    // after it. Both KPCR.NtTib.StackBase (fs:[4]) and KTHREAD.TlsData point
    // at the array, which is how XAPI and the compiler find it.
    const XbeInfo& xbe = kernel_xbe();
    u32 tls = 0;
    if (xbe.tls_dir) {
        u32 start = rd32(xbe.tls_dir + 0), end = rd32(xbe.tls_dir + 4);
        u32 zero_fill = rd32(xbe.tls_dir + 16), index_addr = rd32(xbe.tls_dir + 8);
        u32 need = 4 + (end - start) + zero_fill;
        if (tls_size < need) tls_size = need;
        top -= (tls_size + 15) & ~15u;
        tls = top;
        memset(gptr(tls), 0, tls_size);
        wr32(tls, tls + 4);
        memcpy(gptr(tls + 4), gptr(start), end - start);
        if (index_addr) wr32(index_addr, 0);
    }

    t->kpcr = kernel_alloc(KPCR_SIZE);
    t->kthread = kernel_alloc(ETHREAD_SIZE);
    t->id = g_next_thread_id++;
    u32 kpcr = t->kpcr;
    wr32(kpcr + 0x00, 0xFFFFFFFFu);  // empty SEH chain
    wr32(kpcr + 0x04, tls);          // NtTib.StackBase doubles as TLS pointer
    wr32(kpcr + 0x08, top - stack_size);
    wr32(kpcr + 0x18, kpcr);         // NtTib.Self
    wr32(kpcr + 0x1C, kpcr);         // SelfPcr
    wr32(kpcr + KPCR_PRCB, kpcr + KPCR_PRCBDATA);
    wr32(kpcr + KPCR_PRCBDATA, t->kthread);  // Prcb.CurrentThread
    wr8(t->kthread + DH_TYPE, ThreadObject);
    wr8(t->kthread + 2, (u8)(ETHREAD_SIZE / 4));
    wr32(t->kthread + KTHREAD_TLSDATA, tls);
    wr32(t->kthread + ETHREAD_UNIQUETHREAD, t->id);

    c->fs_base = kpcr;
    c->esp = top - 16;
    return t;
}

constexpr u32 THREAD_RETURN = 0xFE00FFF4u;

u32 guest_call(Ctx* c, u32 fn, std::initializer_list<u32> args, bool stdcall) {
    u32 n = (u32)args.size();
    const u32* a = args.begin();
    for (u32 i = n; i-- > 0;) {
        c->esp -= 4;
        wr32(c->esp, a[i]);
    }
    c->esp -= 4;
    wr32(c->esp, THREAD_RETURN);
    rt_icall(c, fn);
    if (!stdcall) c->esp += 4 * n;
    return c->eax;
}

[[noreturn]] static void thread_exit(GuestThread* t, u32 status) {
    wr32(t->kthread + DH_SIGNAL, 1);
    sched_signal();
    gil_unlock();
    ExitThread(status);
}

struct ThreadStart {
    GuestThread* t;
    u32 start_routine, start_context, system_routine;
    bool suspended;
};
static std::unordered_map<u32, bool> g_suspended;  // kthread -> suspended (GIL)

static DWORD WINAPI thread_main(void* p) {
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    gil_lock();
    thread_bind(s.t);
    sched_wait([&] { return !g_suspended[s.t->kthread]; }, -1);
    Ctx* c = &s.t->ctx;
    if (s.system_routine)
        guest_call(c, s.system_routine, {s.start_routine, s.start_context}, true);
    else
        guest_call(c, s.start_routine, {s.start_context}, true);
    thread_exit(s.t, 0);
}

static u32 PsCreateSystemThreadEx(Ctx*, KArgs a) {
    u32 stack = a[2] < 0x10000 ? 0x10000 : a[2];
    GuestThread* t = thread_create(stack, a[3]);
    auto* s = new ThreadStart{t, a[5], a[6], a[9], (a[7] & 0xFF) != 0};
    g_suspended[t->kthread] = s->suspended;
    if (a[0]) wr32(a[0], handle_create({HType::Object, t->kthread}));
    if (a[4]) wr32(a[4], t->id);
    HANDLE h = CreateThread(nullptr, 64u << 20, thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) fatal("CreateThread failed");
    CloseHandle(h);
    log_msg("thread %u created: start %08X context %08X system routine %08X", t->id, a[5], a[6], a[9]);
    return STATUS_SUCCESS;
}

static u32 PsTerminateSystemThread(Ctx*, KArgs a) { thread_exit(cur_thread(), a[0]); }

static u32 NtResumeThread(Ctx*, KArgs a) {
    u32 obj = handle_object(a[0]);
    if (!obj) return STATUS_INVALID_HANDLE;
    if (a[1]) wr32(a[1], g_suspended[obj] ? 1 : 0);
    g_suspended[obj] = false;
    sched_signal();
    return STATUS_SUCCESS;
}

static u32 KeSetBasePriorityThread(Ctx*, KArgs) { return 0; }

// ------------------------------------------------------------------ handles
static std::unordered_map<u32, HandleEntry> g_handles;
static u32 g_next_handle = 0x1000;

u32 handle_create(const HandleEntry& e) {
    u32 h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = e;
    return h;
}

HandleEntry* handle_get(u32 h) {
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : &it->second;
}

bool handle_close(u32 h) {
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return false;
    if (it->second.file) file_close(it->second.file);
    g_handles.erase(it);
    return true;
}

u32 handle_object(u32 h) {
    if (h == 0xFFFFFFFEu) return cur_thread()->kthread;  // NtCurrentThread()
    HandleEntry* e = handle_get(h);
    return e && e->type == HType::Object ? e->obj : 0;
}

static u32 NtClose(Ctx*, KArgs a) { return handle_close(a[0]) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE; }

static u32 ObReferenceObjectByHandle(Ctx*, KArgs a) {
    u32 obj = handle_object(a[0]);
    if (!obj) {
        if (a[2]) wr32(a[2], 0);
        return STATUS_INVALID_HANDLE;
    }
    if (a[2]) wr32(a[2], obj);
    return STATUS_SUCCESS;
}

static u32 ObfDereferenceObject(Ctx*, KArgs) { return 0; }

// ------------------------------------------------------- dispatcher objects
static bool obj_signaled(u32 obj) {
    u8 type = rd8(obj + DH_TYPE);
    s32 sig = (s32)rd32(obj + DH_SIGNAL);
    if (type == MutantObject) return sig > 0 || rd32(obj + KMUTANT_OWNER) == cur_thread()->kthread;
    return sig > 0;
}

static void obj_consume(u32 obj) {
    switch (rd8(obj + DH_TYPE)) {
    case EventSynchronizationObject:
    case TimerSynchronizationObject:
        wr32(obj + DH_SIGNAL, 0);
        break;
    case SemaphoreObject:
        wr32(obj + DH_SIGNAL, rd32(obj + DH_SIGNAL) - 1);
        break;
    case MutantObject:
        wr32(obj + DH_SIGNAL, rd32(obj + DH_SIGNAL) - 1);
        wr32(obj + KMUTANT_OWNER, cur_thread()->kthread);
        break;
    default:
        break;
    }
}

bool obj_try_acquire(u32 obj) {
    if (!obj_signaled(obj)) return false;
    obj_consume(obj);
    return true;
}

static u32 wait_single(u32 obj, u32 timeout_ptr) {
    s64 deadline = nt_deadline(timeout_ptr);
    return sched_wait([&] { return obj_try_acquire(obj); }, deadline) ? STATUS_SUCCESS : STATUS_TIMEOUT;
}

// File handles count as signaled: all I/O completes synchronously.
static u32 wait_handle(u32 h, u32 timeout_ptr) {
    HandleEntry* e = handle_get(h);
    if (e && e->type == HType::File) return STATUS_SUCCESS;
    u32 obj = handle_object(h);
    if (!obj) return STATUS_INVALID_HANDLE;
    return wait_single(obj, timeout_ptr);
}

static u32 KeWaitForSingleObject(Ctx*, KArgs a) { return wait_single(a[0], a[4]); }
static u32 NtWaitForSingleObject(Ctx*, KArgs a) { return wait_handle(a[0], a[2]); }
static u32 NtWaitForSingleObjectEx(Ctx*, KArgs a) { return wait_handle(a[0], a[3]); }

static u32 NtWaitForMultipleObjectsEx(Ctx*, KArgs a) {
    u32 count = a[0], handles = a[1], wait_any = a[2];
    std::vector<u32> objs(count);
    for (u32 i = 0; i < count; i++) {
        u32 h = rd32(handles + 4 * i);
        HandleEntry* e = handle_get(h);
        objs[i] = e && e->type == HType::File ? 0 : handle_object(h);
        if (!objs[i] && !(e && e->type == HType::File)) return STATUS_INVALID_HANDLE;
    }
    s64 deadline = nt_deadline(a[5]);
    u32 result = STATUS_TIMEOUT;
    sched_wait(
        [&] {
            if (wait_any) {
                for (u32 i = 0; i < count; i++)
                    if (!objs[i] || obj_try_acquire(objs[i])) {
                        result = i;  // STATUS_WAIT_0 + i
                        return true;
                    }
                return false;
            }
            for (u32 o : objs)
                if (o && !obj_signaled(o)) return false;
            for (u32 o : objs)
                if (o) obj_consume(o);
            result = STATUS_SUCCESS;
            return true;
        },
        deadline);
    return result;
}

static u32 new_object(u8 type, u32 size, s32 signal) {
    u32 obj = kernel_alloc(size);
    wr8(obj + DH_TYPE, type);
    wr8(obj + 2, (u8)(size / 4));
    wr32(obj + DH_SIGNAL, (u32)signal);
    wr32(obj + 8, obj + 8);  // empty wait list
    wr32(obj + 12, obj + 8);
    return obj;
}

static u32 NtCreateEvent(Ctx*, KArgs a) {
    // EventType: NotificationEvent = 0, SynchronizationEvent = 1.
    u32 obj = new_object(a[2] ? EventSynchronizationObject : EventNotificationObject, 0x10, (a[3] & 0xFF) ? 1 : 0);
    wr32(a[0], handle_create({HType::Object, obj}));
    return STATUS_SUCCESS;
}

static u32 set_event(u32 obj) {
    u32 prev = rd32(obj + DH_SIGNAL);
    wr32(obj + DH_SIGNAL, 1);
    sched_signal();
    return prev;
}

static u32 NtSetEvent(Ctx*, KArgs a) {
    u32 obj = handle_object(a[0]);
    if (!obj) return STATUS_INVALID_HANDLE;
    u32 prev = set_event(obj);
    if (a[1]) wr32(a[1], prev);
    return STATUS_SUCCESS;
}

static u32 KeSetEvent(Ctx*, KArgs a) { return set_event(a[0]); }

static u32 NtCreateMutant(Ctx*, KArgs a) {
    bool owned = (a[2] & 0xFF) != 0;
    u32 obj = new_object(MutantObject, 0x20, owned ? 0 : 1);
    if (owned) wr32(obj + KMUTANT_OWNER, cur_thread()->kthread);
    wr32(a[0], handle_create({HType::Object, obj}));
    return STATUS_SUCCESS;
}

static u32 NtReleaseMutant(Ctx*, KArgs a) {
    u32 obj = handle_object(a[0]);
    if (!obj) return STATUS_INVALID_HANDLE;
    if (rd32(obj + KMUTANT_OWNER) != cur_thread()->kthread) return STATUS_MUTANT_NOT_OWNED;
    s32 prev = (s32)rd32(obj + DH_SIGNAL);
    if (a[1]) wr32(a[1], (u32)prev);
    wr32(obj + DH_SIGNAL, (u32)(prev + 1));
    if (prev + 1 > 0) wr32(obj + KMUTANT_OWNER, 0);
    sched_signal();
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------- DPCs/timers
constexpr u32 KDPC_INSERTED = 2, KDPC_ROUTINE = 0xC, KDPC_CONTEXT = 0x10, KDPC_ARG1 = 0x14, KDPC_ARG2 = 0x18;
constexpr u32 KTIMER_DUE = 0x10, KTIMER_DPC = 0x20, KTIMER_PERIOD = 0x24;

static std::deque<u32> g_dpc_queue;  // guarded by the GIL
static std::unordered_map<u32, s64> g_timers;  // timer -> due (ms), GIL
static u64 g_timer_gen;  // bumped whenever the timer set changes, GIL

static u32 KeInitializeDpc(Ctx*, KArgs a) {
    u32 dpc = a[0];
    wr16(dpc, 19);  // DpcObject
    wr8(dpc + KDPC_INSERTED, 0);
    wr32(dpc + KDPC_ROUTINE, a[1]);
    wr32(dpc + KDPC_CONTEXT, a[2]);
    return 0;
}

static bool queue_dpc(u32 dpc, u32 arg1, u32 arg2) {
    if (rd8(dpc + KDPC_INSERTED)) return false;
    wr8(dpc + KDPC_INSERTED, 1);
    wr32(dpc + KDPC_ARG1, arg1);
    wr32(dpc + KDPC_ARG2, arg2);
    g_dpc_queue.push_back(dpc);
    sched_signal();
    return true;
}

static u32 KeInsertQueueDpc(Ctx*, KArgs a) { return queue_dpc(a[0], a[1], a[2]); }

static u32 KeRemoveQueueDpc(Ctx*, KArgs a) {
    for (auto it = g_dpc_queue.begin(); it != g_dpc_queue.end(); ++it)
        if (*it == a[0]) {
            g_dpc_queue.erase(it);
            wr8(a[0] + KDPC_INSERTED, 0);
            return 1;
        }
    return 0;
}

static u32 KeInitializeTimerEx(Ctx*, KArgs a) {
    u32 t = a[0];
    wr8(t + DH_TYPE, a[1] ? TimerSynchronizationObject : TimerNotificationObject);
    wr8(t + 2, 0x28 / 4);
    wr32(t + DH_SIGNAL, 0);
    wr32(t + 8, t + 8);
    wr32(t + 12, t + 8);
    wr64(t + KTIMER_DUE, 0);
    wr32(t + KTIMER_PERIOD, 0);
    return 0;
}

static u32 KeSetTimer(Ctx*, KArgs a) {
    u32 t = a[0];
    bool was_set = g_timers.count(t) != 0;
    u32 due_ptr = t + KTIMER_DUE;
    wr32(due_ptr, a[1]);
    wr32(due_ptr + 4, a[2]);
    g_timers[t] = nt_deadline(due_ptr);
    wr32(t + DH_SIGNAL, 0);
    wr32(t + KTIMER_DPC, a[3]);
    wr32(t + KTIMER_PERIOD, 0);
    g_timer_gen++;
    sched_signal();
    return was_set;
}

static u32 KeCancelTimer(Ctx*, KArgs a) {
    g_timer_gen++;
    return g_timers.erase(a[0]) != 0;
}

// Fires due timers and runs queued DPCs on its own guest thread at
// DISPATCH_LEVEL.
static s64 fire_due_timers() {
    s64 now = now_ms(), next = -1;
    for (auto it = g_timers.begin(); it != g_timers.end();) {
        if (it->second <= now) {
            u32 timer = it->first;
            it = g_timers.erase(it);
            wr32(timer + DH_SIGNAL, 1);
            if (u32 dpc = rd32(timer + KTIMER_DPC)) queue_dpc(dpc, (u32)system_time_100ns(), 0);
            if (u32 period = rd32(timer + KTIMER_PERIOD)) g_timers[timer] = now + period;
            sched_signal();
        } else {
            if (next < 0 || it->second < next) next = it->second;
            ++it;
        }
    }
    return next;
}

// ---------------------------------------------------------------- interrupts
// Bus interrupt lines are level-triggered by the device models; connected
// service routines run on the DPC thread at the interrupt's IRQL.
constexpr u32 KINT_ROUTINE = 0x00, KINT_CONTEXT = 0x04, KINT_LEVEL = 0x08, KINT_IRQL = 0x0C, KINT_CONNECTED = 0x10;
constexpr u32 IRQ_VECTOR_BASE = 0x30;
static u32 g_irq_obj[16];
static std::atomic<u32> g_irq_lines;

void kernel_set_irq(u32 line, bool level) {
    u32 bit = 1u << line;
    u32 old = level ? g_irq_lines.fetch_or(bit) : g_irq_lines.fetch_and(~bit);
    if (level && !(old & bit)) sched_signal();
}

static u32 irq_ready() {
    u32 lines = g_irq_lines, ready = 0;
    for (u32 i = 0; i < 16; i++)
        if ((lines >> i & 1) && g_irq_obj[i]) ready |= 1u << i;
    return ready;
}

static u32 KeInitializeInterrupt(Ctx*, KArgs a) {
    u32 k = a[0];
    wr32(k + KINT_ROUTINE, a[1]);
    wr32(k + KINT_CONTEXT, a[2]);
    wr32(k + KINT_LEVEL, a[3] - IRQ_VECTOR_BASE);
    wr32(k + KINT_IRQL, a[4]);
    wr8(k + KINT_CONNECTED, 0);
    return 0;
}
static u32 KeConnectInterrupt(Ctx*, KArgs a) {
    u32 k = a[0], level = rd32(k + KINT_LEVEL) & 15;
    if (rd8(k + KINT_CONNECTED) || g_irq_obj[level]) return 0;
    wr8(k + KINT_CONNECTED, 1);
    g_irq_obj[level] = k;
    sched_signal();
    return 1;
}
static u32 KeDisconnectInterrupt(Ctx*, KArgs a) {
    u32 k = a[0], level = rd32(k + KINT_LEVEL) & 15;
    if (!rd8(k + KINT_CONNECTED)) return 0;
    wr8(k + KINT_CONNECTED, 0);
    g_irq_obj[level] = 0;
    return 1;
}
static u32 HalGetInterruptVector(Ctx*, KArgs a) {
    if (a[1]) wr8(a[1], (u8)(26 - a[0]));  // device IRQL, higher for lower lines
    return IRQ_VECTOR_BASE + a[0];
}
static DWORD WINAPI dpc_thread(void* p) {
    GuestThread* t = (GuestThread*)p;
    gil_lock();
    thread_bind(t);
    wr8(t->kpcr + KPCR_IRQL, 2);
    for (;;) {
        if (u32 ready = irq_ready()) {
            u32 k = g_irq_obj[__builtin_ctz(ready)];
            wr8(t->kpcr + KPCR_IRQL, (u8)rd32(k + KINT_IRQL));
            guest_call(&t->ctx, rd32(k + KINT_ROUTINE), {k, rd32(k + KINT_CONTEXT)}, true);
            wr8(t->kpcr + KPCR_IRQL, 2);
            continue;
        }
        s64 next = fire_due_timers();
        if (!g_dpc_queue.empty()) {
            u32 dpc = g_dpc_queue.front();
            g_dpc_queue.pop_front();
            wr8(dpc + KDPC_INSERTED, 0);
            guest_call(&t->ctx, rd32(dpc + KDPC_ROUTINE),
                       {dpc, rd32(dpc + KDPC_CONTEXT), rd32(dpc + KDPC_ARG1), rd32(dpc + KDPC_ARG2)}, true);
            continue;
        }
        u64 gen = g_timer_gen;
        sched_wait([&] { return !g_dpc_queue.empty() || g_timer_gen != gen || irq_ready(); }, next);
    }
}

static u32 KeSynchronizeExecution(Ctx* c, KArgs a) { return guest_call(c, a[1], {a[2]}, true) & 0xFF; }

// --------------------------------------------------------------------- IRQL
static u32 KeRaiseIrqlToDpcLevel(Ctx*, KArgs) {
    u32 kpcr = cur_thread()->kpcr;
    u8 old = rd8(kpcr + KPCR_IRQL);
    wr8(kpcr + KPCR_IRQL, 2);
    return old;
}
static u32 KfRaiseIrql(Ctx* c, KArgs) {
    u32 kpcr = cur_thread()->kpcr;
    u8 old = rd8(kpcr + KPCR_IRQL);
    wr8(kpcr + KPCR_IRQL, (u8)c->ecx);
    return old;
}
static u32 KfLowerIrql(Ctx* c, KArgs) {
    wr8(cur_thread()->kpcr + KPCR_IRQL, (u8)c->ecx);
    return 0;
}

// --------------------------------------------------------------------- time
static u32 KeQuerySystemTime(Ctx*, KArgs a) {
    wr64(a[0], system_time_100ns());
    return 0;
}
static u32 KeQueryInterruptTime(Ctx* c, KArgs) {
    u64 t = qpc_now() * 10000000ull / (u64)g_qpf.QuadPart;
    c->edx = (u32)(t >> 32);
    return (u32)t;
}
// The Xbox performance counter runs at the CPU clock, same as rdtsc.
static u32 KeQueryPerformanceCounter(Ctx* c, KArgs) {
    u64 t = rt_rdtsc(c);
    c->edx = (u32)(t >> 32);
    return (u32)t;
}
static u32 KeQueryPerformanceFrequency(Ctx* c, KArgs) {
    c->edx = 0;
    return 733333333u;
}

static u32 KeDelayExecutionThread(Ctx*, KArgs a) {
    s64 deadline = nt_deadline(a[2]);
    if (deadline <= now_ms()) {
        gil_unlock();
        SwitchToThread();
        gil_lock();
    } else {
        sched_wait([] { return false; }, deadline);
    }
    return STATUS_SUCCESS;
}

static u32 NtYieldExecution(Ctx*, KArgs) {
    gil_unlock();
    SwitchToThread();
    gil_lock();
    return STATUS_SUCCESS;
}

static u32 KeStallExecutionProcessor(Ctx*, KArgs a) {
    u64 end = qpc_now() + (u64)a[0] * (u64)g_qpf.QuadPart / 1000000ull;
    while (qpc_now() < end) YieldProcessor();
    return 0;
}

void kernel_register_threads() {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);

    kreg(255, PsCreateSystemThreadEx);
    kreg(258, PsTerminateSystemThread);
    kreg(224, NtResumeThread);
    kreg(143, KeSetBasePriorityThread);
    kreg(187, NtClose);
    kreg(246, ObReferenceObjectByHandle);
    kreg(250, ObfDereferenceObject);
    kreg(159, KeWaitForSingleObject);
    kreg(233, NtWaitForSingleObject);
    kreg(234, NtWaitForSingleObjectEx);
    kreg(235, NtWaitForMultipleObjectsEx);
    kreg(189, NtCreateEvent);
    kreg(225, NtSetEvent);
    kreg(145, KeSetEvent);
    kreg(192, NtCreateMutant);
    kreg(221, NtReleaseMutant);
    kreg(107, KeInitializeDpc);
    kreg(119, KeInsertQueueDpc);
    kreg(137, KeRemoveQueueDpc);
    kreg(113, KeInitializeTimerEx);
    kreg(149, KeSetTimer);
    kreg(97, KeCancelTimer);
    kreg(109, KeInitializeInterrupt);
    kreg(98, KeConnectInterrupt);
    kreg(100, KeDisconnectInterrupt);
    kreg(44, HalGetInterruptVector);
    kreg(153, KeSynchronizeExecution);
    kreg(129, KeRaiseIrqlToDpcLevel);
    kreg(160, KfRaiseIrql);
    kreg(161, KfLowerIrql);
    kreg(128, KeQuerySystemTime);
    kreg(125, KeQueryInterruptTime);
    kreg(126, KeQueryPerformanceCounter);
    kreg(127, KeQueryPerformanceFrequency);
    kreg(99, KeDelayExecutionThread);
    kreg(238, NtYieldExecution);
    kreg(151, KeStallExecutionProcessor);
}

// Background services: tick counter / preemption, timers, DPCs.
void kernel_start_services() {
    CloseHandle(CreateThread(nullptr, 0, tick_thread, nullptr, 0, nullptr));
    GuestThread* dpc = thread_create(0x10000, 0);
    CloseHandle(CreateThread(nullptr, 64u << 20, dpc_thread, dpc, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
}
