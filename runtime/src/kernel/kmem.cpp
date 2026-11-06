// Kernel memory services: virtual memory (NtAllocateVirtualMemory),
// contiguous/system memory (Mm*) and the executive pool (Ex*).
//
// Each kind of memory gets its own guest address range so allocations never
// collide; the backing host memory is committed up front, except the
// virtual range, which is committed page by page like on the console.
#include <windows.h>

#include <map>
#include <mutex>

#include "kernel.h"

constexpr u32 PAGE = 0x1000, ALLOC_GRAN = 0x10000;
constexpr u32 VIRT_LO = 0x10000000u, VIRT_HI = 0x7FFF0000u;
constexpr u32 CONTIG_LO = 0x80000000u, CONTIG_SIZE = 64u << 20;  // phys | 0x80000000
constexpr u32 SYSTEM_LO = 0xD0000000u, SYSTEM_SIZE = 32u << 20;
constexpr u32 POOL_LO = 0x8E000000u, POOL_SIZE = 16u << 20;

constexpr u32 MEM_COMMIT_ = 0x1000, MEM_RESERVE_ = 0x2000, MEM_DECOMMIT_ = 0x4000, MEM_RELEASE_ = 0x8000;
constexpr u32 MEM_FREE_ = 0x10000, MEM_PRIVATE_ = 0x20000, MEM_TOP_DOWN_ = 0x100000;
constexpr u32 PAGE_READWRITE_ = 4;

static u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

// First-fit allocator over a guest address range.
class RangeAllocator {
public:
    void init(u32 lo, u32 size) {
        free_[lo] = size;
    }
    u32 alloc(u32 size, u32 align, bool top_down = false) {
        size = align_up(size ? size : 1, 16);
        auto try_block = [&](u32 base, u32 len) -> u32 {
            u32 a = top_down ? ((base + len - size) & ~(align - 1)) : align_up(base, align);
            if (a < base || (u64)a + size > (u64)base + len) return 0;
            take(base, len, a, size);
            return a;
        };
        if (top_down) {
            for (auto it = free_.rbegin(); it != free_.rend(); ++it)
                if (u32 a = try_block(it->first, it->second)) return a;
        } else {
            for (auto& [base, len] : free_)
                if (u32 a = try_block(base, len)) return a;
        }
        return 0;
    }
    // Claim a specific range; fails if any of it is in use.
    bool claim(u32 a, u32 size) {
        auto it = free_.upper_bound(a);
        if (it == free_.begin()) return false;
        --it;
        if ((u64)it->first + it->second < (u64)a + size) return false;
        take(it->first, it->second, a, size);
        return true;
    }
    u32 release(u32 a) {
        auto it = used_.find(a);
        if (it == used_.end()) return 0;
        u32 size = it->second;
        used_.erase(it);
        auto [ins, ok] = free_.emplace(a, size);
        // Merge with neighbours.
        auto next = std::next(ins);
        if (next != free_.end() && ins->first + ins->second == next->first) {
            ins->second += next->second;
            free_.erase(next);
        }
        if (ins != free_.begin()) {
            auto prev = std::prev(ins);
            if (prev->first + prev->second == ins->first) {
                prev->second += ins->second;
                free_.erase(ins);
            }
        }
        return size;
    }
    u32 size_of(u32 a) const {
        auto it = used_.find(a);
        return it == used_.end() ? 0 : it->second;
    }
    // Allocation containing a (base, size), or (0, 0).
    std::pair<u32, u32> containing(u32 a) const {
        auto it = used_.upper_bound(a);
        if (it == used_.begin()) return {0, 0};
        --it;
        if (a >= it->first + it->second) return {0, 0};
        return {it->first, it->second};
    }
    u64 used_bytes() const {
        u64 n = 0;
        for (auto& [a, s] : used_) n += s;
        return n;
    }

private:
    void take(u32 base, u32 len, u32 a, u32 size) {
        free_.erase(base);
        if (a > base) free_[base] = a - base;
        if ((u64)a + size < (u64)base + len) free_[a + size] = (u32)((u64)base + len - a - size);
        used_[a] = size;
    }
    std::map<u32, u32> free_, used_;
};

static std::mutex g_mem_m;
static RangeAllocator g_virt, g_contig, g_system, g_pool;
static u64 g_virt_committed;
static bool g_trace_vmem = getenv("NL_TRACE_VMEM") != nullptr;

static void host_commit(u32 a, u32 size) {
    if (!VirtualAlloc(gptr(a), size, MEM_COMMIT, PAGE_READWRITE)) fatal("commit %08X+%X failed", a, size);
}

void kmem_init() {
    g_virt.init(VIRT_LO, VIRT_HI - VIRT_LO);
    // The top MB is GPU instance memory (PRAMIN, see gpu.h).
    g_contig.init(CONTIG_LO + 0x00100000u, CONTIG_SIZE - 0x00200000u);
    g_system.init(SYSTEM_LO, SYSTEM_SIZE);
    g_pool.init(POOL_LO, POOL_SIZE);
    host_commit(SYSTEM_LO, SYSTEM_SIZE);
    host_commit(POOL_LO, POOL_SIZE);
}

u32 pool_alloc(u32 size) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    u32 a = g_pool.alloc(size, 16);
    if (a) memset(gptr(a), 0, size);
    return a;
}
void pool_free(u32 a) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    g_pool.release(a);
}
u32 pool_size(u32 a) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    return g_pool.size_of(a);
}

// ------------------------------------------------------------- virtual
static u32 NtAllocateVirtualMemory(Ctx*, KArgs a) {
    u32 base_ptr = a[0], size_ptr = a[2], type = a[3];
    u32 base = rd32(base_ptr), size = rd32(size_ptr);
    if (!size) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lk(g_mem_m);
    // Committing at no particular address reserves the region as well.
    if (!base && (type & MEM_COMMIT_)) type |= MEM_RESERVE_;
    if (type & MEM_RESERVE_) {
        u32 rsize = align_up(size, PAGE);
        if (base) {
            base &= ~(ALLOC_GRAN - 1);
            if (base < VIRT_LO || !g_virt.claim(base, rsize)) return STATUS_CONFLICTING_ADDRESSES;
        } else {
            base = g_virt.alloc(rsize, ALLOC_GRAN, (type & MEM_TOP_DOWN_) != 0);
            if (!base) return STATUS_NO_MEMORY;
        }
        size = rsize;
    }
    if (type & MEM_COMMIT_) {
        u32 lo = base & ~(PAGE - 1), hi = align_up(base + size, PAGE);
        auto r = g_virt.containing(lo);
        if (!r.first || hi > r.first + r.second) return STATUS_CONFLICTING_ADDRESSES;
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(gptr(lo), &mbi, sizeof(mbi));
        host_commit(lo, hi - lo);
        g_virt_committed += hi - lo;
        base = lo;
        size = hi - lo;
    }
    if (g_trace_vmem) log_msg("[vmem] alloc %08X+%X type %X", base, size, type);
    wr32(base_ptr, base);
    wr32(size_ptr, size);
    return STATUS_SUCCESS;
}

static u32 NtFreeVirtualMemory(Ctx*, KArgs a) {
    u32 base_ptr = a[0], size_ptr = a[1], type = a[2];
    u32 base = rd32(base_ptr), size = rd32(size_ptr);
    std::lock_guard<std::mutex> lk(g_mem_m);
    auto r = g_virt.containing(base);
    if (!r.first) return STATUS_INVALID_PARAMETER;
    if (type & MEM_RELEASE_) {
        VirtualFree(gptr(r.first), r.second, MEM_DECOMMIT);
        g_virt.release(r.first);
        base = r.first;
        size = r.second;
    } else if (type & MEM_DECOMMIT_) {
        u32 lo = base & ~(PAGE - 1);
        u32 hi = size ? align_up(base + size, PAGE) : r.first + r.second;
        VirtualFree(gptr(lo), hi - lo, MEM_DECOMMIT);
        base = lo;
        size = hi - lo;
    }
    if (g_trace_vmem) log_msg("[vmem] free %08X+%X type %X", base, size, type);
    wr32(base_ptr, base);
    wr32(size_ptr, size);
    return STATUS_SUCCESS;
}

static u32 NtQueryVirtualMemory(Ctx*, KArgs a) {
    u32 addr = a[0] & ~(PAGE - 1), out = a[1];
    std::lock_guard<std::mutex> lk(g_mem_m);
    auto r = g_virt.containing(addr);
    u32 state = MEM_FREE_, alloc_base = 0, region = PAGE, protect = 0;
    if (r.first) {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(gptr(addr), &mbi, sizeof(mbi));
        bool committed = mbi.State == MEM_COMMIT;
        u32 end = (u32)((u8*)mbi.BaseAddress + mbi.RegionSize - GUEST_BASE);
        if (end > r.first + r.second) end = r.first + r.second;
        state = committed ? MEM_COMMIT_ : MEM_RESERVE_;
        alloc_base = r.first;
        region = end - addr;
        protect = committed ? PAGE_READWRITE_ : 0;
    } else if (addr < RAM_SIZE) {
        state = MEM_COMMIT_;
        protect = PAGE_READWRITE_;
    }
    wr32(out + 0, addr);
    wr32(out + 4, alloc_base);
    wr32(out + 8, alloc_base ? PAGE_READWRITE_ : 0);
    wr32(out + 12, region);
    wr32(out + 16, state);
    wr32(out + 20, protect);
    wr32(out + 24, state == MEM_FREE_ ? 0 : MEM_PRIVATE_);
    return STATUS_SUCCESS;
}

// -------------------------------------------------------- contiguous/system
static u32 contig_alloc(u32 size, u32 align) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    if (align < PAGE) align = PAGE;
    u32 a = g_contig.alloc(align_up(size, PAGE), align);
    if (a) memset(gptr(a), 0, size);
    return a;
}

u32 kernel_contig_alloc(u32 size) { return contig_alloc(size, PAGE); }
void kernel_contig_free(u32 addr) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    g_contig.release(addr);
}

static u32 MmAllocateContiguousMemory(Ctx*, KArgs a) { return contig_alloc(a[0], PAGE); }
static u32 MmAllocateContiguousMemoryEx(Ctx*, KArgs a) { return contig_alloc(a[0], a[3]); }

static u32 MmFreeContiguousMemory(Ctx*, KArgs a) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    g_contig.release(a[0]);
    return 0;
}

static u32 MmAllocateSystemMemory(Ctx*, KArgs a) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    u32 p = g_system.alloc(align_up(a[0], PAGE), PAGE);
    if (p) memset(gptr(p), 0, a[0]);
    return p;
}

static u32 MmFreeSystemMemory(Ctx*, KArgs a) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    return g_system.release(a[0]) / PAGE;
}

static u32 MmQueryAllocationSize(Ctx*, KArgs a) {
    std::lock_guard<std::mutex> lk(g_mem_m);
    for (RangeAllocator* r : {&g_contig, &g_system, &g_virt}) {
        auto c = r->containing(a[0]);
        if (c.first) return c.second;
    }
    return 0;
}

static u32 MmGetPhysicalAddress(Ctx*, KArgs a) {
    u32 v = a[0];
    return v >= CONTIG_LO && v < CONTIG_LO + CONTIG_SIZE ? v - CONTIG_LO : v;
}

static u32 MmClaimGpuInstanceMemory(Ctx*, KArgs a) {
    if (a[1]) wr32(a[1], 0);
    return CONTIG_LO + CONTIG_SIZE;
}

static u32 MmQueryStatistics(Ctx*, KArgs a) {
    u32 s = a[0];
    if (rd32(s) != 0x24) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lk(g_mem_m);
    u64 used = g_virt_committed + g_contig.used_bytes() + g_system.used_bytes() + (8u << 20);
    u32 total = RAM_SIZE / PAGE;
    u32 avail = used >= RAM_SIZE ? 256 : (u32)((RAM_SIZE - used) / PAGE);
    wr32(s + 4, total);
    wr32(s + 8, avail);
    wr32(s + 12, (u32)g_virt_committed);
    wr32(s + 16, (u32)g_virt.used_bytes());
    wr32(s + 20, 0);
    wr32(s + 24, (u32)(g_pool.used_bytes() / PAGE));
    wr32(s + 28, 64);
    wr32(s + 32, 0x4A0000 / PAGE);
    return STATUS_SUCCESS;
}

static u32 nop0(Ctx*, KArgs) { return 0; }
static u32 MmQueryAddressProtect(Ctx*, KArgs) { return PAGE_READWRITE_; }

// --------------------------------------------------------------------- pool
static u32 ExAllocatePool(Ctx*, KArgs a) { return pool_alloc(a[0]); }
static u32 ExFreePool(Ctx*, KArgs a) {
    pool_free(a[0]);
    return 0;
}
static u32 ExQueryPoolBlockSize(Ctx*, KArgs a) { return pool_size(a[0]); }

void kernel_register_memory() {
    kmem_init();
    kreg(184, NtAllocateVirtualMemory);
    kreg(199, NtFreeVirtualMemory);
    kreg(217, NtQueryVirtualMemory);
    kreg(165, MmAllocateContiguousMemory);
    kreg(166, MmAllocateContiguousMemoryEx);
    kreg(171, MmFreeContiguousMemory);
    kreg(167, MmAllocateSystemMemory);
    kreg(172, MmFreeSystemMemory);
    kreg(180, MmQueryAllocationSize);
    kreg(173, MmGetPhysicalAddress);
    kreg(168, MmClaimGpuInstanceMemory);
    kreg(181, MmQueryStatistics);
    kreg(178, nop0);  // MmPersistContiguousMemory
    kreg(175, nop0);  // MmLockUnlockBufferPages
    kreg(176, nop0);  // MmLockUnlockPhysicalPage
    kreg(182, nop0);  // MmSetAddressProtect
    kreg(179, MmQueryAddressProtect);
    kreg(14, ExAllocatePool);
    kreg(15, ExAllocatePool);  // ExAllocatePoolWithTag: tag ignored
    kreg(17, ExFreePool);
    kreg(23, ExQueryPoolBlockSize);
}
