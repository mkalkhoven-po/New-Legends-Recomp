#include <windows.h>
#include <bcrypt.h>

#include <string>
#include <vector>

#include "func_table.h"
#include "gpu.h"
#include "kernel.h"
#include "version.h"

// Return address pushed for the boot thread; reaching it means the XBE entry
// point returned to the "kernel".
constexpr u32 BOOT_RETURN = 0xFE00FFF0u;

// SHA-256 of the retail default.xbe the code was recompiled from
// (New Legends, USA, title id 54510001, version 1).
static const char* EXPECTED_XBE_SHA256 = "905f66ace66c4b315e13a6de374f89b1f5d46ee68bc5c25a6e7984343d3f58de";

void crash_init();
[[noreturn]] void show_user_error(const char* text);

static std::string sha256_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
    fclose(f);
    unsigned char digest[32];
    BCryptFinishHash(h, digest, sizeof digest, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    for (unsigned char b : digest) {
        char t[3];
        snprintf(t, sizeof t, "%02x", b);
        hex += t;
    }
    return hex;
}

static bool file_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The game files live in a "game" folder next to the exe (or in the current
// folder, the developer layout). Settings, saves and caches go there too.
static void find_home() {
    if (file_exists("game/default.xbe")) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    std::string dir(exe);
    dir.resize(dir.find_last_of("\\/") + 1);
    SetCurrentDirectoryA(dir.c_str());
}

static void check_game_data(const std::string& xbe_path) {
    if (!file_exists(xbe_path))
        show_user_error(
            "The game files were not found.\n\n"
            "Copy all files from your New Legends disc (the US release) into the \"game\" folder next to "
            "newlegends.exe, so that game\\default.xbe exists. See README.md for how to do that.");
    std::string hash = sha256_file(xbe_path.c_str());
    if (hash != EXPECTED_XBE_SHA256) {
        log_msg("%s has SHA-256 %s, expected %s", xbe_path.c_str(), hash.c_str(), EXPECTED_XBE_SHA256);
        show_user_error(
            "The default.xbe in the \"game\" folder is not the version this program was made for.\n\n"
            "Only the US release of New Legends for the original Xbox is supported. Make sure the files "
            "come from that disc and are not modified.");
    }
    if (!file_exists(xbe_path.substr(0, xbe_path.find_last_of("/\\") + 1) + "entry.lin"))
        show_user_error(
            "The \"game\" folder has default.xbe but not the level files (such as entry.lin).\n\n"
            "Copy all files from the disc, not just default.xbe.");
}

int main(int argc, char** argv) {
    if (argc <= 1) find_home();
    std::string xbe_path = argc > 1 ? argv[1] : "game/default.xbe";
    std::string game_dir = xbe_path.substr(0, xbe_path.find_last_of("/\\") + 1);
    if (game_dir.empty()) game_dir = ".";

    crash_init();
    log_msg("%s %s", NL_PRODUCT_NAME, NL_VERSION_STRING);
    check_game_data(xbe_path);
    mem_init();
    XbeInfo xbe = xbe_load(xbe_path.c_str());
    if (xbe.entry != g_entry_point)
        fatal("%s has entry %08X but the recompiled code expects %08X", xbe_path.c_str(), xbe.entry, g_entry_point);
    dispatch_init();
    kernel_init(xbe);
    file_init(game_dir.c_str());

    // The boot thread runs the XBE entry point the way the kernel does after
    // loading a title; XAPI then starts the game's main thread from there.
    GuestThread* boot = thread_create(0x10000, 0);
    gil_lock();
    thread_bind(boot);
    kernel_start_services();
    video_init();

    GuestFunc entry = dispatch_lookup(xbe.entry);
    if (!entry) fatal("no recompiled function at entry %08X", xbe.entry);
    log_msg("starting guest at %08X", xbe.entry);
    Ctx* c = &boot->ctx;
    GUEST_CALL(entry, BOOT_RETURN);
    // Only the boot thread ends here; the game runs on the threads it made.
    log_msg("boot thread finished (eax %08X)", c->eax);
    gil_unlock();
    ExitThread(0);
}
