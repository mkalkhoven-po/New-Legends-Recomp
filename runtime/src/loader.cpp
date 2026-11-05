// Maps default.xbe into guest memory the way the Xbox kernel does: headers at
// the base address, then every section at its virtual address.
#include <vector>

#include "runtime.h"

static const u32 ENTRY_XOR_RETAIL = 0xA8FC57ABu, ENTRY_XOR_DEBUG = 0x94859D4Bu;
static const u32 THUNK_XOR_RETAIL = 0x5B6D40B6u, THUNK_XOR_DEBUG = 0xEFB1F152u;

static std::vector<u8> g_file;  // the whole XBE, for XeLoadSection
constexpr u32 SECTION_PRELOAD = 2, SECTION_REFCOUNT = 0x18;

static u32 le32(const std::vector<u8>& d, size_t off) {
    u32 v;
    memcpy(&v, &d[off], 4);
    return v;
}

XbeInfo xbe_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    std::vector<u8> d(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) fatal("cannot read %s", path);
    fclose(f);
    if (d.size() < 0x180 || memcmp(d.data(), "XBEH", 4) != 0) fatal("%s is not an XBE", path);

    XbeInfo x{};
    x.base = le32(d, 0x104);
    u32 size_headers = le32(d, 0x108);
    u32 size_image = le32(d, 0x10C);
    u32 nsections = le32(d, 0x11C);
    u32 sect_hdrs = le32(d, 0x120);
    u32 entry_enc = le32(d, 0x128);
    x.tls_dir = le32(d, 0x12C);
    x.stack_commit = le32(d, 0x130);
    u32 thunk_enc = le32(d, 0x158);

    bool debug = false;
    x.entry = entry_enc ^ ENTRY_XOR_RETAIL;
    if (x.entry < x.base || x.entry >= x.base + size_image) {
        x.entry = entry_enc ^ ENTRY_XOR_DEBUG;
        debug = true;
    }
    x.kernel_thunk = thunk_enc ^ (debug ? THUNK_XOR_DEBUG : THUNK_XOR_RETAIL);
    if (x.base + size_image > RAM_SIZE) fatal("XBE image does not fit in guest RAM");

    memcpy(gptr(x.base), d.data(), size_headers);
    for (u32 i = 0; i < nsections; i++) {
        size_t h = sect_hdrs - x.base + i * 0x38;
        u32 vaddr = le32(d, h + 4), vsize = le32(d, h + 8), raw = le32(d, h + 12), rawsize = le32(d, h + 16);
        memset(gptr(vaddr), 0, vsize);
        memcpy(gptr(vaddr), &d[raw], rawsize < vsize ? rawsize : vsize);
        // The kernel loads preload sections at boot, counting one reference.
        if (le32(d, h) & SECTION_PRELOAD) wr32(sect_hdrs + i * 0x38 + SECTION_REFCOUNT, 1);
    }
    g_file = std::move(d);
    log_msg("loaded %s: base %08X entry %08X kernel thunk %08X, %u sections", path, x.base, x.entry, x.kernel_thunk,
            nsections);
    return x;
}

void xbe_reload_section(u32 hdr) {
    u32 vaddr = rd32(hdr + 4), vsize = rd32(hdr + 8), raw = rd32(hdr + 12), rawsize = rd32(hdr + 16);
    if ((size_t)raw + rawsize > g_file.size()) fatal("section header %08X points outside the XBE", hdr);
    memset(gptr(vaddr), 0, vsize);
    memcpy(gptr(vaddr), &g_file[raw], rawsize < vsize ? rawsize : vsize);
}
