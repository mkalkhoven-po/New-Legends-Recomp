// HLE xboxkrnl: export table, call dispatch, import thunk patching and
// exported kernel data.
#include "kernel.h"

enum Conv { STDCALL, FASTCALL, CDECL };
struct ExportInfo {
    u32 ordinal;
    const char* name;
    u32 argbytes;  // stdcall: bytes the callee pops
    Conv conv;
};

// Ordinals, names and argument sizes from nxdk's xboxkrnl.exe.def, limited
// to the functions this game imports.
static const ExportInfo kExports[] = {
    {1, "AvGetSavedDataAddress", 0, STDCALL},
    {2, "AvSendTVEncoderOption", 16, STDCALL},
    {3, "AvSetDisplayMode", 24, STDCALL},
    {4, "AvSetSavedDataAddress", 4, STDCALL},
    {8, "DbgPrint", 0, CDECL},
    {14, "ExAllocatePool", 4, STDCALL},
    {15, "ExAllocatePoolWithTag", 8, STDCALL},
    {17, "ExFreePool", 4, STDCALL},
    {23, "ExQueryPoolBlockSize", 4, STDCALL},
    {24, "ExQueryNonVolatileSetting", 20, STDCALL},
    {44, "HalGetInterruptVector", 8, STDCALL},
    {46, "HalReadWritePCISpace", 24, STDCALL},
    {47, "HalRegisterShutdownNotification", 8, STDCALL},
    {49, "HalReturnToFirmware", 4, STDCALL},
    {65, "IoCreateDevice", 24, STDCALL},
    {67, "IoCreateSymbolicLink", 8, STDCALL},
    {69, "IoDeleteSymbolicLink", 4, STDCALL},
    {74, "IoInvalidDeviceRequest", 8, STDCALL},
    {81, "IoStartNextPacket", 4, STDCALL},
    {83, "IoStartPacket", 12, STDCALL},
    {87, "IofCompleteRequest", 8, FASTCALL},
    {91, "IoDismountVolumeByName", 4, STDCALL},
    {95, "KeBugCheck", 4, STDCALL},
    {97, "KeCancelTimer", 4, STDCALL},
    {98, "KeConnectInterrupt", 4, STDCALL},
    {99, "KeDelayExecutionThread", 12, STDCALL},
    {100, "KeDisconnectInterrupt", 4, STDCALL},
    {107, "KeInitializeDpc", 12, STDCALL},
    {109, "KeInitializeInterrupt", 28, STDCALL},
    {113, "KeInitializeTimerEx", 8, STDCALL},
    {119, "KeInsertQueueDpc", 12, STDCALL},
    {125, "KeQueryInterruptTime", 0, STDCALL},
    {126, "KeQueryPerformanceCounter", 0, STDCALL},
    {127, "KeQueryPerformanceFrequency", 0, STDCALL},
    {128, "KeQuerySystemTime", 4, STDCALL},
    {129, "KeRaiseIrqlToDpcLevel", 0, STDCALL},
    {137, "KeRemoveQueueDpc", 4, STDCALL},
    {139, "KeRestoreFloatingPointState", 4, STDCALL},
    {142, "KeSaveFloatingPointState", 4, STDCALL},
    {143, "KeSetBasePriorityThread", 8, STDCALL},
    {145, "KeSetEvent", 12, STDCALL},
    {149, "KeSetTimer", 16, STDCALL},
    {151, "KeStallExecutionProcessor", 4, STDCALL},
    {153, "KeSynchronizeExecution", 12, STDCALL},
    {159, "KeWaitForSingleObject", 20, STDCALL},
    {160, "KfRaiseIrql", 4, FASTCALL},
    {161, "KfLowerIrql", 4, FASTCALL},
    {165, "MmAllocateContiguousMemory", 4, STDCALL},
    {166, "MmAllocateContiguousMemoryEx", 20, STDCALL},
    {167, "MmAllocateSystemMemory", 8, STDCALL},
    {168, "MmClaimGpuInstanceMemory", 8, STDCALL},
    {171, "MmFreeContiguousMemory", 4, STDCALL},
    {172, "MmFreeSystemMemory", 8, STDCALL},
    {173, "MmGetPhysicalAddress", 4, STDCALL},
    {175, "MmLockUnlockBufferPages", 12, STDCALL},
    {176, "MmLockUnlockPhysicalPage", 8, STDCALL},
    {178, "MmPersistContiguousMemory", 12, STDCALL},
    {179, "MmQueryAddressProtect", 4, STDCALL},
    {180, "MmQueryAllocationSize", 4, STDCALL},
    {181, "MmQueryStatistics", 4, STDCALL},
    {182, "MmSetAddressProtect", 12, STDCALL},
    {184, "NtAllocateVirtualMemory", 20, STDCALL},
    {187, "NtClose", 4, STDCALL},
    {189, "NtCreateEvent", 16, STDCALL},
    {190, "NtCreateFile", 36, STDCALL},
    {192, "NtCreateMutant", 12, STDCALL},
    {196, "NtDeviceIoControlFile", 40, STDCALL},
    {198, "NtFlushBuffersFile", 8, STDCALL},
    {199, "NtFreeVirtualMemory", 12, STDCALL},
    {200, "NtFsControlFile", 40, STDCALL},
    {202, "NtOpenFile", 24, STDCALL},
    {203, "NtOpenSymbolicLinkObject", 8, STDCALL},
    {207, "NtQueryDirectoryFile", 40, STDCALL},
    {210, "NtQueryFullAttributesFile", 8, STDCALL},
    {211, "NtQueryInformationFile", 20, STDCALL},
    {215, "NtQuerySymbolicLinkObject", 12, STDCALL},
    {217, "NtQueryVirtualMemory", 8, STDCALL},
    {218, "NtQueryVolumeInformationFile", 20, STDCALL},
    {219, "NtReadFile", 32, STDCALL},
    {221, "NtReleaseMutant", 8, STDCALL},
    {224, "NtResumeThread", 8, STDCALL},
    {225, "NtSetEvent", 8, STDCALL},
    {226, "NtSetInformationFile", 20, STDCALL},
    {233, "NtWaitForSingleObject", 12, STDCALL},
    {234, "NtWaitForSingleObjectEx", 16, STDCALL},
    {235, "NtWaitForMultipleObjectsEx", 24, STDCALL},
    {236, "NtWriteFile", 32, STDCALL},
    {238, "NtYieldExecution", 0, STDCALL},
    {246, "ObReferenceObjectByHandle", 12, STDCALL},
    {250, "ObfDereferenceObject", 4, FASTCALL},
    {255, "PsCreateSystemThreadEx", 40, STDCALL},
    {258, "PsTerminateSystemThread", 4, STDCALL},
    {260, "RtlAnsiStringToUnicodeString", 12, STDCALL},
    {269, "RtlCompareMemoryUlong", 12, STDCALL},
    {277, "RtlEnterCriticalSection", 4, STDCALL},
    {279, "RtlEqualString", 12, STDCALL},
    {286, "RtlFreeAnsiString", 4, STDCALL},
    {289, "RtlInitAnsiString", 8, STDCALL},
    {290, "RtlInitUnicodeString", 8, STDCALL},
    {291, "RtlInitializeCriticalSection", 4, STDCALL},
    {294, "RtlLeaveCriticalSection", 4, STDCALL},
    {301, "RtlNtStatusToDosError", 4, STDCALL},
    {302, "RtlRaiseException", 4, STDCALL},
    {304, "RtlTimeFieldsToTime", 8, STDCALL},
    {305, "RtlTimeToTimeFields", 8, STDCALL},
    {308, "RtlUnicodeStringToAnsiString", 12, STDCALL},
    {312, "RtlUnwind", 16, STDCALL},
    {327, "XeLoadSection", 4, STDCALL},
    {328, "XeUnloadSection", 4, STDCALL},
    {335, "XcSHAInit", 4, STDCALL},
    {336, "XcSHAUpdate", 12, STDCALL},
    {337, "XcSHAFinal", 8, STDCALL},
    {358, "HalIsResetOrShutdownPending", 0, STDCALL},
    {359, "IoMarkIrpMustComplete", 4, STDCALL},
    {360, "HalInitiateShutdown", 0, STDCALL},
};

static const ExportInfo* g_info[512];
static KFunc g_funcs[512];
static bool g_trace = getenv("NL_TRACE_KERNEL") != nullptr;

void kreg(u32 ordinal, KFunc fn) { g_funcs[ordinal] = fn; }

// Kernel exports that are variables rather than functions. The thunk slot
// must hold the address of guest memory with the object in it.
struct DataExport {
    u32 ordinal;
    u32 size;
};
static const DataExport kDataExports[] = {
    {16, 0x40},  {18, 0x40}, {30, 0x40},  {31, 0x40},  {40, 4},   {71, 0x40},  {156, 4},
    {164, 4},    {259, 0x40}, {322, 8},   {323, 16},   {324, 8},  {325, 16},   {326, 8},
    {353, 16},   {354, 256}, {355, 284},  {356, 4},
};
static u32 g_data_addr[512];
u32 g_ke_tick_count_addr;

u32 kernel_data_addr(u32 ordinal) { return g_data_addr[ordinal & 511]; }

static XbeInfo g_xbe;
const XbeInfo& kernel_xbe() { return g_xbe; }

void kernel_init(const XbeInfo& xbe) {
    g_xbe = xbe;
    for (const ExportInfo& e : kExports) g_info[e.ordinal] = &e;
    kernel_register_misc();
    kernel_register_memory();
    kernel_register_threads();
    kernel_register_files();
    kernel_register_rtl();

    for (const DataExport& d : kDataExports) g_data_addr[d.ordinal] = kernel_alloc(d.size);
    g_ke_tick_count_addr = g_data_addr[156];
    // Values matching a retail console.
    wr32(g_data_addr[16], EventNotificationObject);
    wr32(g_data_addr[40], 3);                // HalDiskCachePartitionCount
    wr32(g_data_addr[322] + 0, 0x00000020);  // XboxHardwareInfo.Flags
    wr16(g_data_addr[324] + 0, 1);           // XboxKrnlVersion 1.0.5838.1
    wr16(g_data_addr[324] + 4, 5838);
    wr16(g_data_addr[324] + 6, 1);
    for (int i = 0; i < 16; i++) wr8(g_data_addr[325] + i, (u8)(0x5A ^ (i * 17)));  // XboxSignatureKey
    launch_data_restore();

    int n = 0;
    for (u32 slot = xbe.kernel_thunk;; slot += 4) {
        u32 v = rd32(slot);
        if (!v) break;
        u32 ord = v & 0x1FF;
        if (g_data_addr[ord]) {
            wr32(slot, g_data_addr[ord]);
        } else {
            if (!g_info[ord]) fatal("game imports unknown kernel ordinal %u", ord);
            wr32(slot, KERNEL_THUNK_BASE | ord);
        }
        n++;
    }
    int missing = 0;
    for (const ExportInfo& e : kExports)
        if (!g_funcs[e.ordinal]) missing++;
    log_msg("kernel: patched %d import thunks, %d functions not implemented yet", n, missing);
}

void kernel_call(Ctx* c, u32 ordinal) {
    const ExportInfo* info = ordinal < 512 ? g_info[ordinal] : nullptr;
    if (!info) fatal("call to unknown kernel ordinal %u", ordinal);
    KFunc fn = g_funcs[ordinal];
    u32 ret_addr = rd32(c->esp);
    if (!fn)
        fatal("kernel function %s (#%u) not implemented (called from %08X, args %08X %08X %08X %08X)", info->name,
              ordinal, ret_addr, rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12), rd32(c->esp + 16));
    if (g_trace)
        log_msg("[%u] %s(%08X %08X %08X %08X) from %08X", cur_thread() ? cur_thread()->id : 0, info->name,
                info->conv == FASTCALL ? c->ecx : rd32(c->esp + 4), info->conv == FASTCALL ? c->edx : rd32(c->esp + 8),
                rd32(c->esp + 12), rd32(c->esp + 16), ret_addr);
    c->eax = fn(c, KArgs{c});
    // stdcall pops the arguments; fastcall's (up to two) came in ecx/edx.
    c->esp += 4 + (info->conv == STDCALL ? info->argbytes : 0);
}
