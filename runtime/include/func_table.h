#pragma once
#include "x86_recomp.h"

struct FuncEntry {
    u32 addr;
    GuestFunc fn;
};

// Emitted by the recompiler into generated/func_table.cpp.
extern const FuncEntry g_func_table[];
extern const unsigned g_func_count;
extern const unsigned g_entry_point;
