// Game data setup: the version check, installing the game files from a disc
// image or a folder, the setup window and shortcuts.
#include "setup.h"

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <stdio.h>

#include <system_error>
#include <thread>
#include <vector>

#include "runtime.h"
#include "setup_ids.h"
#include "version.h"

namespace fs = std::filesystem;

// SHA-256 of the retail default.xbe the code was recompiled from
// (New Legends, USA, title id 54510001, version 1).
static const char* EXPECTED_XBE_SHA256 = "905f66ace66c4b315e13a6de374f89b1f5d46ee68bc5c25a6e7984343d3f58de";
static const char* GAME_DIR = "game";
static const char* PARTIAL_DIR = "game.partial";

// ------------------------------------------------------------------ hashing
namespace {
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    }
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void update(const void* p, size_t n) { BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::string hex() {
        unsigned char d[32];
        BCryptFinishHash(h, d, sizeof d, 0);
        std::string out;
        for (unsigned char b : d) {
            char t[3];
            snprintf(t, sizeof t, "%02x", b);
            out += t;
        }
        return out;
    }
};

std::string sha256_file(const fs::path& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    Sha256 sha;
    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) sha.update(buf.data(), n);
    fclose(f);
    return sha.hex();
}

const char* WRONG_VERSION =
    "is not the version this program was made for. Only the US release of New Legends for the "
    "original Xbox is supported, unmodified.";
}  // namespace

std::string game_data_problem(const std::string& xbe_path) {
    fs::path xbe = fs::u8path(xbe_path);
    std::error_code ec;
    if (!fs::exists(xbe, ec)) return "";  // nothing there yet: the setup window says what to do
    std::string hash = sha256_file(xbe);
    if (hash != EXPECTED_XBE_SHA256) {
        log_msg("%s has SHA-256 %s, expected %s", xbe_path.c_str(), hash.c_str(), EXPECTED_XBE_SHA256);
        return std::string("The default.xbe in the \"game\" folder ") + WRONG_VERSION;
    }
    if (!fs::exists(xbe.parent_path() / "entry.lin", ec))
        return "The \"game\" folder has default.xbe but not the level files (such as entry.lin). "
               "All files from the disc are needed.";
    return "";
}

// ------------------------------------------------------------------ sources
// A file to copy: from a disc image (offset) or from a folder (path).
namespace {
struct Entry {
    fs::path rel;
    bool dir;
    unsigned long long size;
    unsigned long long offset;  // disc images
    fs::path from;              // folders
};

constexpr unsigned SECTOR = 2048;
const char MAGIC[] = "MICROSOFT*XBOX*MEDIA";
// Redump-style full images carry the game partition at this offset; trimmed
// xiso images start at 0.
constexpr unsigned long long PARTITION_OFFSETS[] = {0, 0x18300000ull};

struct Image {
    FILE* f = nullptr;
    unsigned long long base = 0;
    ~Image() {
        if (f) fclose(f);
    }
    bool read(unsigned long long off, void* p, size_t n) {
        return _fseeki64(f, (long long)off, SEEK_SET) == 0 && fread(p, 1, n, f) == n;
    }
};

// XDVDFS directory tables are binary trees of entries: left, right (in
// dwords), sector, size, attributes, name length, name.
bool read_image_dir(Image& img, unsigned sector, unsigned size, const fs::path& prefix, std::vector<Entry>& out,
                    int depth) {
    if (size == 0) return true;
    if (depth > 32 || size > (64u << 20)) return false;
    std::vector<unsigned char> table(size);
    if (!img.read(img.base + (unsigned long long)sector * SECTOR, table.data(), size)) return false;
    std::vector<unsigned> stack{0};
    std::vector<bool> seen(size / 4 + 1);
    while (!stack.empty()) {
        unsigned off = stack.back();
        stack.pop_back();
        if (off + 14 > size || seen[off / 4]) continue;
        seen[off / 4] = true;
        const unsigned char* e = &table[off];
        unsigned left = e[0] | e[1] << 8, right = e[2] | e[3] << 8;
        if (left == 0xFFFF) continue;  // padding
        unsigned ent_sector = e[4] | e[5] << 8 | e[6] << 16 | (unsigned)e[7] << 24;
        unsigned ent_size = e[8] | e[9] << 8 | e[10] << 16 | (unsigned)e[11] << 24;
        unsigned attr = e[12], name_len = e[13];
        if (off + 14 + name_len > size) continue;
        std::wstring name;
        for (unsigned i = 0; i < name_len; i++) name += (wchar_t)e[14 + i];  // Latin-1
        if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos)
            return false;
        fs::path rel = prefix / name;
        bool dir = attr & 0x10;
        out.push_back({rel, dir, dir ? 0 : ent_size, img.base + (unsigned long long)ent_sector * SECTOR, {}});
        if (dir && !read_image_dir(img, ent_sector, ent_size, rel, out, depth + 1)) return false;
        if (left) stack.push_back(left * 4);
        if (right) stack.push_back(right * 4);
    }
    return true;
}

bool list_image(Image& img, const fs::path& path, std::vector<Entry>& out, std::string* error) {
    img.f = _wfopen(path.c_str(), L"rb");
    if (!img.f) return *error = "The disc image could not be opened.", false;
    for (unsigned long long base : PARTITION_OFFSETS) {
        char magic[20];
        if (img.read(base + 32ull * SECTOR, magic, sizeof magic) && !memcmp(magic, MAGIC, sizeof magic)) {
            img.base = base;
            unsigned char root[8];
            if (!img.read(base + 32ull * SECTOR + 20, root, 8)) break;
            unsigned rs = root[0] | root[1] << 8 | root[2] << 16 | (unsigned)root[3] << 24;
            unsigned rz = root[4] | root[5] << 8 | root[6] << 16 | (unsigned)root[7] << 24;
            if (!read_image_dir(img, rs, rz, {}, out, 0)) return *error = "The disc image is damaged.", false;
            return true;
        }
    }
    return *error = "This file is not an Xbox disc image.", false;
}

bool list_folder(const fs::path& dir, std::vector<Entry>& out, std::string* error) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        fs::path rel = fs::relative(it->path(), dir, ec);
        if (ec) break;
        bool is_dir = it->is_directory(ec);
        out.push_back({rel, is_dir, is_dir ? 0 : (unsigned long long)it->file_size(ec), 0, it->path()});
    }
    if (ec) return *error = "The folder could not be read: " + ec.message(), false;
    return true;
}

// Hash of default.xbe and whether entry.lin is there, before copying gigabytes.
bool check_source(const std::vector<Entry>& files, Image* img, std::string* error, const char* what) {
    std::string What = what;
    What[0] = (char)toupper((unsigned char)What[0]);
    const Entry *xbe = nullptr, *entry = nullptr;
    for (auto& e : files) {
        if (e.dir) continue;
        if (!_wcsicmp(e.rel.c_str(), L"default.xbe")) xbe = &e;
        if (!_wcsicmp(e.rel.c_str(), L"entry.lin")) entry = &e;
    }
    if (!xbe) return *error = What + " has no default.xbe, so it holds no Xbox game.", false;
    std::string hash;
    if (img) {
        Sha256 sha;
        std::vector<unsigned char> buf(1 << 16);
        for (unsigned long long done = 0; done < xbe->size;) {
            size_t n = (size_t)std::min<unsigned long long>(buf.size(), xbe->size - done);
            if (!img->read(xbe->offset + done, buf.data(), n)) return *error = "The disc image is damaged.", false;
            sha.update(buf.data(), n);
            done += n;
        }
        hash = sha.hex();
    } else {
        hash = sha256_file(xbe->from);
    }
    if (hash != EXPECTED_XBE_SHA256) {
        log_msg("setup: source default.xbe has SHA-256 %s", hash.c_str());
        return *error = std::string("The game in ") + what + " " + WRONG_VERSION, false;
    }
    if (!entry) return *error = What + " has the game but not its level files (such as entry.lin).", false;
    return true;
}

std::string gb(unsigned long long bytes) {
    char t[32];
    snprintf(t, sizeof t, "%.1f GB", bytes / 1e9);
    return t;
}
}  // namespace

bool install_game_files(const fs::path& source, InstallProgress& progress, std::string* error) {
    progress.stage = 0;
    std::error_code ec;
    std::vector<Entry> files;
    Image img;
    bool from_image = !fs::is_directory(source, ec);
    const char* what = from_image ? "this disc image" : "this folder";
    if (from_image) {
        if (!list_image(img, source, files, error)) return false;
    } else {
        fs::path dir = source;
        // Accept the folder above "game" too.
        if (!fs::exists(dir / "default.xbe", ec) && fs::exists(dir / GAME_DIR / "default.xbe", ec)) dir /= GAME_DIR;
        if (fs::exists(GAME_DIR, ec) && fs::equivalent(dir, GAME_DIR, ec))
            return *error = "This is the program's own \"game\" folder. Choose the disc image or the folder you "
                            "copied the files from.",
                   false;
        if (!list_folder(dir, files, error)) return false;
    }
    if (!check_source(files, from_image ? &img : nullptr, error, what)) return false;

    unsigned long long total = 0;
    for (auto& e : files) total += e.size;
    ULARGE_INTEGER free_bytes;
    if (GetDiskFreeSpaceExW(L".", &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < total + (256ull << 20))
        return *error = "There is not enough free disk space: the game files need " + gb(total) + ", and " +
                        gb(free_bytes.QuadPart) + " is free on this drive.",
               false;

    // Copy into a folder of our own first, so a stopped or failed copy never
    // leaves a half "game" folder behind.
    fs::remove_all(PARTIAL_DIR, ec);
    fs::create_directory(PARTIAL_DIR, ec);
    if (ec) return *error = "The \"" + std::string(PARTIAL_DIR) + "\" folder could not be made: " + ec.message(), false;
    progress.total = total;
    progress.done = 0;
    progress.stage = 1;
    std::vector<unsigned char> buf(4 << 20);
    auto fail = [&](const std::string& msg) {
        fs::remove_all(PARTIAL_DIR, ec);
        *error = msg;
        return false;
    };
    for (auto& e : files) {
        fs::path dest = fs::path(PARTIAL_DIR) / e.rel;
        if (e.dir) {
            fs::create_directories(dest, ec);
            continue;
        }
        fs::create_directories(dest.parent_path(), ec);
        FILE* out = _wfopen(dest.c_str(), L"wb");
        if (!out) return fail("Could not write " + dest.u8string() + ".");
        FILE* in = from_image ? nullptr : _wfopen(e.from.c_str(), L"rb");
        if (!from_image && !in) {
            fclose(out);
            return fail("Could not read " + e.from.u8string() + ".");
        }
        for (unsigned long long done = 0; done < e.size;) {
            if (progress.cancel) {
                fclose(out);
                if (in) fclose(in);
                return fail("Stopped.");
            }
            size_t n = (size_t)std::min<unsigned long long>(buf.size(), e.size - done);
            bool ok = from_image ? img.read(e.offset + done, buf.data(), n) : fread(buf.data(), 1, n, in) == n;
            if (!ok || fwrite(buf.data(), 1, n, out) != n) {
                fclose(out);
                if (in) fclose(in);
                return fail(ok ? "Writing the game files failed (is the disk full?)." : "Reading " + e.rel.u8string() +
                                                                                            " failed.");
            }
            done += n;
            progress.done += n;
        }
        if (in) fclose(in);
        if (fclose(out) != 0) return fail("Writing the game files failed (is the disk full?).");
    }

    progress.stage = 2;
    if (fs::exists(fs::symlink_status(GAME_DIR, ec))) {
        // Keep whatever was there: it may be the player's own copy.
        std::string old = "game.old";
        for (int i = 2; fs::exists(fs::symlink_status(old, ec)); i++) old = "game.old" + std::to_string(i);
        fs::rename(GAME_DIR, old, ec);
        if (ec) return fail("The old \"game\" folder could not be renamed: " + ec.message());
        log_msg("setup: kept the old game folder as %s", old.c_str());
    }
    fs::rename(PARTIAL_DIR, GAME_DIR, ec);
    if (ec) return fail("The \"game\" folder could not be made: " + ec.message());
    log_msg("setup: installed %llu bytes from %s", total, source.u8string().c_str());
    return true;
}

// ------------------------------------------------------------------ shortcuts
static bool make_link(REFKNOWNFOLDERID folder, std::string* error) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path exe_path(exe);
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &dir))) return *error = "No such folder.", false;
    fs::path link = fs::path(dir) / L"" NL_PRODUCT_NAME ".lnk";
    CoTaskMemFree(dir);
    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl));
    if (SUCCEEDED(hr)) {
        sl->SetPath(exe_path.c_str());
        sl->SetWorkingDirectory(exe_path.parent_path().c_str());
        sl->SetIconLocation(exe_path.c_str(), 0);
        sl->SetDescription(L"" NL_PRODUCT_NAME);
        hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
    }
    if (SUCCEEDED(hr)) hr = pf->Save(link.c_str(), TRUE);
    if (pf) pf->Release();
    if (sl) sl->Release();
    if (FAILED(hr)) return *error = "The shortcut could not be made.", false;
    log_msg("setup: shortcut %s", link.u8string().c_str());
    return true;
}

bool create_shortcuts(bool desktop, bool start_menu, std::string* error) {
    bool ok = true;
    if (desktop) ok &= make_link(FOLDERID_Desktop, error);
    if (start_menu) ok &= make_link(FOLDERID_Programs, error);
    return ok;
}

// ------------------------------------------------------------------ window
namespace {
constexpr UINT WM_SETUP_DONE = WM_APP + 1;
constexpr UINT_PTR PROGRESS_TIMER = 1;

struct SetupState {
    std::string problem;
    bool files_ok;
    bool working = false;
    bool quit_after = false;  // closed while copying: quit when the copy has stopped
    InstallProgress progress;
    std::string error;
    std::thread worker;
};

void set_text(HWND dlg, int id, const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
    SetDlgItemTextW(dlg, id, w.c_str());
}

void set_working(HWND dlg, SetupState& s, bool on) {
    s.working = on;
    EnableWindow(GetDlgItem(dlg, IDC_PICK_IMAGE), !on);
    EnableWindow(GetDlgItem(dlg, IDC_PICK_FOLDER), !on);
    EnableWindow(GetDlgItem(dlg, IDOK), !on && s.files_ok);
    SetDlgItemTextW(dlg, IDCANCEL, on ? L"Stop" : L"Quit");
}

bool pick(HWND dlg, bool folder, fs::path* out) {
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return false;
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (folder) {
        fd->SetTitle(L"Choose the folder with the New Legends game files");
    } else {
        COMDLG_FILTERSPEC types[] = {{L"Xbox disc images (*.iso, *.xiso)", L"*.iso;*.xiso"}, {L"All files", L"*.*"}};
        fd->SetFileTypes(2, types);
        fd->SetTitle(L"Choose your New Legends disc image");
    }
    bool ok = false;
    IShellItem* item = nullptr;
    if (SUCCEEDED(fd->Show(dlg)) && SUCCEEDED(fd->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            *out = p;
            CoTaskMemFree(p);
            ok = true;
        }
        item->Release();
    }
    fd->Release();
    return ok;
}

void start_install(HWND dlg, SetupState& s, const fs::path& source) {
    if (s.worker.joinable()) s.worker.join();
    s.progress.cancel = false;
    s.progress.done = 0;
    s.progress.total = 0;
    s.error.clear();
    set_working(dlg, s, true);
    set_text(dlg, IDC_STATUS, "Checking " + source.filename().u8string() + "...");
    SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
    SetTimer(dlg, PROGRESS_TIMER, 100, nullptr);
    s.worker = std::thread([dlg, &s, source] {
        bool ok = install_game_files(source, s.progress, &s.error);
        PostMessageW(dlg, WM_SETUP_DONE, ok, 0);
    });
}

// Shortcuts, then start the game.
void finish(HWND dlg) {
    std::string err;
    bool desktop = IsDlgButtonChecked(dlg, IDC_DESKTOP) == BST_CHECKED;
    bool start_menu = IsDlgButtonChecked(dlg, IDC_STARTMENU) == BST_CHECKED;
    if ((desktop || start_menu) && !create_shortcuts(desktop, start_menu, &err))
        MessageBoxW(dlg, L"The shortcut could not be made. The game will start anyway.", L"" NL_PRODUCT_NAME,
                    MB_OK | MB_ICONWARNING);
    EndDialog(dlg, 1);
}

INT_PTR CALLBACK setup_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    SetupState* s = (SetupState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        s = (SetupState*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)s);
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        std::string intro;
        if (s->files_ok)
            intro = "The game files are set up. You can install them again from a disc image or a folder, "
                    "or only make shortcuts and start the game.";
        else
            intro = "To play, this program needs the files from your own New Legends disc (the US release "
                    "for the original Xbox). Choose a disc image of it, or a folder with the files copied "
                    "from it. They are copied into the \"game\" folder next to this program.";
        set_text(dlg, IDC_INTRO, intro);
        if (!s->problem.empty()) set_text(dlg, IDC_STATUS, s->problem);
        CheckDlgButton(dlg, IDC_DESKTOP, BST_CHECKED);
        CheckDlgButton(dlg, IDC_STARTMENU, BST_CHECKED);
        SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETRANGE32, 0, 1000);
        ShowWindow(GetDlgItem(dlg, IDOK), s->files_ok ? SW_SHOW : SW_HIDE);  // nothing to start yet
        set_working(dlg, *s, false);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PICK_IMAGE:
        case IDC_PICK_FOLDER: {
            fs::path p;
            if (!s->working && pick(dlg, LOWORD(wp) == IDC_PICK_FOLDER, &p)) start_install(dlg, *s, p);
            return TRUE;
        }
        case IDOK:
            if (!s->working && s->files_ok) finish(dlg);
            return TRUE;
        case IDCANCEL:
            if (s->working) {
                s->progress.cancel = true;
                s->quit_after = false;
                set_text(dlg, IDC_STATUS, "Stopping...");
            } else {
                EndDialog(dlg, 0);
            }
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (s->working) {
            s->progress.cancel = true;
            s->quit_after = true;
        } else {
            EndDialog(dlg, 0);
        }
        return TRUE;
    case WM_TIMER: {
        unsigned long long done = s->progress.done, total = s->progress.total;
        if (s->progress.stage == 1 && total) {
            SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, (WPARAM)(done * 1000 / total), 0);
            if (!s->progress.cancel) set_text(dlg, IDC_STATUS, "Copying the game files: " + gb(done) + " of " + gb(total));
        } else if (s->progress.stage == 2) {
            set_text(dlg, IDC_STATUS, "Finishing...");
        }
        return TRUE;
    }
    case WM_SETUP_DONE:
        KillTimer(dlg, PROGRESS_TIMER);
        s->worker.join();
        set_working(dlg, *s, false);
        if (wp) {
            SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 1000, 0);
            set_text(dlg, IDC_STATUS, "The game files are set up.");
            finish(dlg);
        } else {
            SendDlgItemMessageW(dlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
            set_text(dlg, IDC_STATUS, s->error);
            if (s->quit_after) EndDialog(dlg, 0);
        }
        return TRUE;
    }
    return FALSE;
}
}  // namespace

bool run_setup_window(const std::string& problem, bool files_ok) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetupState s;
    s.problem = problem;
    s.files_ok = files_ok;
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETUP), nullptr, setup_proc, (LPARAM)&s);
    if (s.worker.joinable()) s.worker.join();
    if (SUCCEEDED(co)) CoUninitialize();
    if (r == -1) log_msg("setup: the window could not be opened (%lu)", GetLastError());
    return r == 1;
}
