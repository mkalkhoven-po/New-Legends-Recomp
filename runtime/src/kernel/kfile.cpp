// File system: Xbox object paths -> host files.
//
//   \??\X:                      symbolic links (IoCreateSymbolicLink)
//   \Device\CdRom0              the extracted game disc (game/)
//   \Device\Harddisk0\PartitionN  hdd/PartitionN (1 = E:, 2 = C:, 3-5 = caches)
//
// All I/O completes synchronously; the GIL is dropped around host reads and
// writes so other guest threads keep running.
#include <windows.h>

#include <map>
#include <vector>

#include "kernel.h"

struct HostFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring host_path;
    std::string xbox_path;
    bool is_dir = false;
    bool read_only = false;  // on the disc
    HANDLE find = INVALID_HANDLE_VALUE;
    bool find_started = false;
    int raw_partition = -1;  // opened as a raw volume (no path inside it)
    u64 raw_size = 0;
};

// Retail hard disk layout: byte offset and length of each partition.
struct Partition {
    u64 offset, length;
};
static const Partition kPartitions[6] = {
    {0, 0x1DD156000ull},           // 0: whole disk
    {0xABE80000, 0x1312D6000ull},  // 1: E (data)
    {0x8CA80000, 0x1F400000},      // 2: C (system)
    {0x00080000, 0x2EE00000},      // 3: X (cache)
    {0x2EE80000, 0x2EE00000},      // 4: Y (cache)
    {0x5DC80000, 0x2EE00000},      // 5: Z (cache)
};

static std::wstring g_game_dir, g_hdd_dir;
static std::map<std::string, std::string> g_links;  // lower-case link -> target
static bool g_trace = getenv("NL_TRACE_FILES") != nullptr;

static std::string lower(std::string s) {
    for (char& ch : s) ch = (char)tolower((u8)ch);
    return s;
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static bool prefix_match(const std::string& path_lc, const std::string& prefix_lc) {
    return path_lc.compare(0, prefix_lc.size(), prefix_lc) == 0 &&
           (path_lc.size() == prefix_lc.size() || path_lc[prefix_lc.size()] == '\\');
}

// Resolve an Xbox object path to a host path. Returns false if the path is
// not on a device we provide.
static bool resolve(std::string path, std::wstring& host, bool& read_only, int* raw_partition = nullptr) {
    for (int depth = 0; depth < 8; depth++) {
        std::string lc = lower(path);
        bool replaced = false;
        for (auto& [link, target] : g_links)
            if (prefix_match(lc, link)) {
                path = target + path.substr(link.size());
                replaced = true;
                break;
            }
        if (!replaced) break;
    }
    std::string lc = lower(path);
    std::string rest;
    if (prefix_match(lc, "\\device\\cdrom0")) {
        host = g_game_dir;
        rest = path.substr(14);
        read_only = true;
    } else if (lc.compare(0, 27, "\\device\\harddisk0\\partition") == 0 && lc.size() >= 28 && isdigit((u8)lc[27]) &&
               (lc.size() == 28 || lc[28] == '\\')) {
        host = g_hdd_dir + L"\\Partition" + (wchar_t)lc[27];
        rest = path.substr(28);
        read_only = false;
        // "\Device\Harddisk0\PartitionN" without a trailing separator is the
        // volume itself; it is backed by a sparse image file.
        if (raw_partition && lc.size() == 28 && lc[27] <= '5') {
            *raw_partition = lc[27] - '0';
            host += L".img";
            return true;
        }
    } else {
        return false;
    }
    // Drop "." components and a trailing separator.
    std::string clean;
    size_t i = 0;
    while (i < rest.size()) {
        size_t j = rest.find('\\', i);
        if (j == std::string::npos) j = rest.size();
        std::string comp = rest.substr(i, j - i);
        if (!comp.empty() && comp != ".") clean += "\\" + comp;
        i = j + 1;
    }
    host += widen(clean);
    return true;
}

// Full object path for a name relative to an optional root directory handle.
static std::string object_path(u32 root, std::string name) {
    if (root) {
        HandleEntry* e = handle_get(root);
        if (e && e->type == HType::File) {
            std::string base = e->file->xbox_path;
            if (!name.empty() && name[0] != '\\') base += "\\";
            return base + name;
        }
    }
    // XAPI opens "d:\file" relative to the DOS devices directory.
    if (name.size() >= 2 && name[1] == ':' && isalpha((u8)name[0])) return "\\??\\" + name;
    return name;
}

static std::string object_path(u32 obj_attr) {
    return object_path(rd32(obj_attr + 0), read_ansi_string(rd32(obj_attr + 4)));
}

static u64 ft64(const FILETIME& f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void io_status(u32 iosb, u32 status, u32 info) {
    if (!iosb) return;
    wr32(iosb, status);
    wr32(iosb + 4, info);
}

static u32 xbox_attributes(DWORD a) {
    return a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY |
                FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL);
}

void file_close(HostFile* f) {
    if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
    delete f;
}

// ------------------------------------------------------------------ open
enum { FILE_SUPERSEDE, FILE_OPEN, FILE_CREATE, FILE_OPEN_IF, FILE_OVERWRITE, FILE_OVERWRITE_IF };
enum { FILE_SUPERSEDED, FILE_OPENED, FILE_CREATED, FILE_OVERWRITTEN, FILE_EXISTS, FILE_DOES_NOT_EXIST };
constexpr u32 FILE_DIRECTORY_FILE = 0x1, FILE_NON_DIRECTORY_FILE = 0x40, FILE_DELETE_ON_CLOSE = 0x1000;
constexpr u32 WRITE_ACCESS = 0x2 | 0x4 | 0x100 | 0x10000 | 0x40000000 | 0x10000000;

static u32 open_file(u32 handle_ptr, u32 access, u32 obj_attr, u32 iosb, u32 attributes, u32 disposition,
                     u32 options) {
    std::string path = object_path(obj_attr);
    std::wstring host;
    bool read_only;
    int raw = -1;
    if (!resolve(path, host, read_only, &raw)) {
        if (g_trace) log_msg("open %s: no such device", path.c_str());
        io_status(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (raw >= 0) {
        auto* f = new HostFile;
        f->xbox_path = path;
        f->host_path = host;
        f->raw_partition = raw;
        f->raw_size = kPartitions[raw].length;
        f->h = CreateFileW(host.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f->h == INVALID_HANDLE_VALUE) {
            delete f;
            io_status(iosb, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        DWORD bytes;
        DeviceIoControl(f->h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr);
        u32 h = handle_create({HType::File, 0, f});
        wr32(handle_ptr, h);
        io_status(iosb, STATUS_SUCCESS, FILE_OPENED);
        if (g_trace) log_msg("open %s -> handle %X (raw volume)", path.c_str(), h);
        return STATUS_SUCCESS;
    }
    DWORD attr = GetFileAttributesW(host.c_str());
    bool exists = attr != INVALID_FILE_ATTRIBUTES;
    bool is_dir = exists && (attr & FILE_ATTRIBUTE_DIRECTORY);
    bool want_write = (access & WRITE_ACCESS) != 0 || (disposition != FILE_OPEN && disposition != FILE_OPEN_IF);

    auto fail = [&](u32 status) {
        if (g_trace) log_msg("open %s -> %08X", path.c_str(), status);
        io_status(iosb, status, 0);
        return status;
    };
    if (exists && (options & FILE_NON_DIRECTORY_FILE) && is_dir) return fail(STATUS_FILE_IS_A_DIRECTORY);
    if (exists && (options & FILE_DIRECTORY_FILE) && !is_dir) return fail(STATUS_NOT_A_DIRECTORY);
    if (exists && disposition == FILE_CREATE) return fail(STATUS_OBJECT_NAME_COLLISION);
    if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE)) {
        std::wstring parent = host.substr(0, host.find_last_of(L'\\'));
        DWORD pa = GetFileAttributesW(parent.c_str());
        return fail(pa == INVALID_FILE_ATTRIBUTES ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND);
    }
    if (read_only && (!exists || (want_write && !is_dir && disposition != FILE_OPEN && disposition != FILE_OPEN_IF)))
        return fail(STATUS_ACCESS_DENIED);

    u32 info = FILE_OPENED;
    auto* f = new HostFile;
    f->xbox_path = path;
    f->host_path = host;
    f->read_only = read_only;
    if ((options & FILE_DIRECTORY_FILE) || is_dir) {
        if (!exists) {
            if (!CreateDirectoryW(host.c_str(), nullptr)) {
                delete f;
                return fail(STATUS_OBJECT_PATH_NOT_FOUND);
            }
            info = FILE_CREATED;
        }
        f->is_dir = true;
        f->h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
        DWORD create;
        switch (disposition) {
        case FILE_SUPERSEDE:
        case FILE_OVERWRITE_IF: create = CREATE_ALWAYS; info = exists ? FILE_OVERWRITTEN : FILE_CREATED; break;
        case FILE_CREATE: create = CREATE_NEW; info = FILE_CREATED; break;
        case FILE_OPEN_IF: create = OPEN_ALWAYS; info = exists ? FILE_OPENED : FILE_CREATED; break;
        case FILE_OVERWRITE: create = TRUNCATE_EXISTING; info = FILE_OVERWRITTEN; break;
        default: create = OPEN_EXISTING; break;
        }
        DWORD host_access = GENERIC_READ | (read_only ? 0 : GENERIC_WRITE | DELETE);
        DWORD flags = FILE_ATTRIBUTE_NORMAL | ((options & FILE_DELETE_ON_CLOSE) ? FILE_FLAG_DELETE_ON_CLOSE : 0);
        if (attributes & 0x27) flags = (attributes & 0x27) | (flags & ~FILE_ATTRIBUTE_NORMAL);
        f->h = CreateFileW(host.c_str(), host_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           create, flags, nullptr);
    }
    if (f->h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        delete f;
        return fail(err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ? STATUS_ACCESS_DENIED
                                                                                  : STATUS_OBJECT_NAME_NOT_FOUND);
    }
    u32 h = handle_create({HType::File, 0, f});
    wr32(handle_ptr, h);
    io_status(iosb, STATUS_SUCCESS, info);
    if (g_trace) log_msg("open %s -> handle %X (%s)", path.c_str(), h, narrow(host).c_str());
    return STATUS_SUCCESS;
}

static u32 NtCreateFile(Ctx*, KArgs a) { return open_file(a[0], a[1], a[2], a[3], a[5], a[7], a[8]); }
static u32 NtOpenFile(Ctx*, KArgs a) { return open_file(a[0], a[1], a[2], a[3], 0, FILE_OPEN, a[5]); }

static HostFile* get_file(u32 h) {
    HandleEntry* e = handle_get(h);
    return e && e->type == HType::File ? e->file : nullptr;
}

// ------------------------------------------------------------- read/write
static void seek_for_io(HostFile* f, u32 offset_ptr) {
    if (!offset_ptr) return;
    u64 off = rd64(offset_ptr);
    // FILE_USE_FILE_POINTER_POSITION / FILE_WRITE_TO_END_OF_FILE
    if (off == 0xFFFFFFFFFFFFFFFEull) return;
    LARGE_INTEGER li;
    if (off == 0xFFFFFFFFFFFFFFFFull) {
        li.QuadPart = 0;
        SetFilePointerEx(f->h, li, nullptr, FILE_END);
        return;
    }
    li.QuadPart = (LONGLONG)off;
    SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN);
}

static void complete_io(u32 event_handle, u32 apc) {
    if (event_handle) {
        if (u32 obj = handle_object(event_handle)) {
            wr32(obj + DH_SIGNAL, 1);
            sched_signal();
        }
    }
    if (apc) {
        static bool warned;
        if (!warned) log_msg("warning: I/O completion APCs are not delivered");
        warned = true;
    }
}

static u32 NtReadFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f) {
        if (g_trace) log_msg("read handle %X: invalid handle", a[0]);
        return STATUS_INVALID_HANDLE;
    }
    u32 iosb = a[4], buf = a[5], len = a[6];
    seek_for_io(f, a[7]);
    DWORD got = 0;
    gil_unlock();
    BOOL ok = ReadFile(f->h, gptr(buf), len, &got, nullptr);
    gil_lock();
    if (ok && f->raw_partition >= 0 && got < len) {
        // Unwritten parts of a volume image read as zeros.
        LARGE_INTEGER zero{}, pos{};
        SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        u64 at = (u64)pos.QuadPart;
        u64 left = f->raw_size > at ? f->raw_size - at : 0;
        u32 extra = (u32)((u64)(len - got) < left ? len - got : left);
        memset(gptr(buf + got), 0, extra);
        LARGE_INTEGER adv;
        adv.QuadPart = extra;
        SetFilePointerEx(f->h, adv, nullptr, FILE_CURRENT);
        got += extra;
    }
    u32 status = !ok ? STATUS_UNSUCCESSFUL : (got == 0 && len > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    // NL_WATCH_READ=addr (hex): log reads whose buffer covers addr.
    static u32 watch = getenv("NL_WATCH_READ") ? (u32)strtoul(getenv("NL_WATCH_READ"), nullptr, 16) : 0;
    if (g_trace || (watch && watch >= buf && watch < buf + len)) {
        LARGE_INTEGER zero{}, pos{};
        SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        log_msg("read handle %X -> %08X+%X, file offset %llX, got %X", a[0], buf, len, (u64)pos.QuadPart - got, got);
    }
    io_status(iosb, status, got);
    complete_io(a[1], a[2]);
    return status;
}

static u32 NtWriteFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    if (f->read_only) return STATUS_ACCESS_DENIED;
    u32 iosb = a[4], buf = a[5], len = a[6];
    seek_for_io(f, a[7]);
    DWORD put = 0;
    gil_unlock();
    BOOL ok = WriteFile(f->h, gptr(buf), len, &put, nullptr);
    gil_lock();
    u32 status = ok ? STATUS_SUCCESS : STATUS_DISK_FULL;
    io_status(iosb, status, put);
    complete_io(a[1], a[2]);
    return status;
}

static u32 NtFlushBuffersFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    if (!f->read_only && !f->is_dir) FlushFileBuffers(f->h);
    io_status(a[1], STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------- information
static void write_network_open_info(u32 out, const WIN32_FILE_ATTRIBUTE_DATA& d) {
    u64 size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    wr64(out + 0, ft64(d.ftCreationTime));
    wr64(out + 8, ft64(d.ftLastAccessTime));
    wr64(out + 16, ft64(d.ftLastWriteTime));
    wr64(out + 24, ft64(d.ftLastWriteTime));
    wr64(out + 32, (size + 0x3FFF) & ~0x3FFFull);
    wr64(out + 40, size);
    wr32(out + 48, xbox_attributes(d.dwFileAttributes));
    wr32(out + 52, 0);
}

static u32 NtQueryInformationFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    u32 iosb = a[1], out = a[2], len = a[3], cls = a[4];
    WIN32_FILE_ATTRIBUTE_DATA d{};
    GetFileAttributesExW(f->host_path.c_str(), GetFileExInfoStandard, &d);
    u64 size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (f->raw_partition >= 0) {
        size = f->raw_size;
    } else if (!f->is_dir) {
        LARGE_INTEGER li;
        if (GetFileSizeEx(f->h, &li)) size = (u64)li.QuadPart;
    }
    u32 n;
    switch (cls) {
    case 4:  // FileBasicInformation
        n = 0x28;
        if (len < n) return STATUS_INFO_LENGTH_MISMATCH;
        wr64(out + 0, ft64(d.ftCreationTime));
        wr64(out + 8, ft64(d.ftLastAccessTime));
        wr64(out + 16, ft64(d.ftLastWriteTime));
        wr64(out + 24, ft64(d.ftLastWriteTime));
        wr32(out + 32, xbox_attributes(d.dwFileAttributes));
        wr32(out + 36, 0);
        break;
    case 5:  // FileStandardInformation
        n = 0x18;
        if (len < n) return STATUS_INFO_LENGTH_MISMATCH;
        wr64(out + 0, (size + 0x3FFF) & ~0x3FFFull);
        wr64(out + 8, size);
        wr32(out + 16, 1);
        wr8(out + 20, 0);
        wr8(out + 21, f->is_dir);
        wr16(out + 22, 0);
        break;
    case 6:  // FileInternalInformation
        n = 8;
        wr64(out, (u64)(uintptr_t)f);
        break;
    case 7:  // FileEaInformation
        n = 4;
        wr32(out, 0);
        break;
    case 14: {  // FilePositionInformation
        n = 8;
        LARGE_INTEGER zero{}, pos{};
        SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        wr64(out, (u64)pos.QuadPart);
        break;
    }
    case 16:  // FileModeInformation
    case 17:  // FileAlignmentInformation
        n = 4;
        wr32(out, 0);
        break;
    case 34:  // FileNetworkOpenInformation
        n = 0x38;
        if (len < n) return STATUS_INFO_LENGTH_MISMATCH;
        write_network_open_info(out, d);
        wr64(out + 40, size);
        break;
    default:
        log_msg("NtQueryInformationFile: class %u not supported (%s)", cls, f->xbox_path.c_str());
        return STATUS_INVALID_PARAMETER;
    }
    io_status(iosb, STATUS_SUCCESS, n);
    return STATUS_SUCCESS;
}

static u32 NtSetInformationFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    u32 iosb = a[1], in = a[2], cls = a[4];
    u32 status = STATUS_SUCCESS;
    switch (cls) {
    case 14: {  // FilePositionInformation
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)rd64(in);
        SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN);
        break;
    }
    case 20: {  // FileEndOfFileInformation
        if (f->read_only) return STATUS_ACCESS_DENIED;
        LARGE_INTEGER zero{}, pos{}, end;
        SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        end.QuadPart = (LONGLONG)rd64(in);
        SetFilePointerEx(f->h, end, nullptr, FILE_BEGIN);
        SetEndOfFile(f->h);
        SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
        break;
    }
    case 19:  // FileAllocationInformation
        break;
    case 13: {  // FileDispositionInformation
        if (f->read_only) return STATUS_CANNOT_DELETE;
        if (f->is_dir) {
            // Directory handles lack DELETE access; remove by path on request.
            if (rd8(in) && !RemoveDirectoryW(f->host_path.c_str())) status = STATUS_CANNOT_DELETE;
        } else {
            FILE_DISPOSITION_INFO di{rd8(in) != 0};
            if (!SetFileInformationByHandle(f->h, FileDispositionInfo, &di, sizeof(di))) status = STATUS_CANNOT_DELETE;
        }
        break;
    }
    case 4: {  // FileBasicInformation
        FILETIME times[3];
        u64 v[3] = {rd64(in + 0), rd64(in + 8), rd64(in + 16)};
        for (int i = 0; i < 3; i++) times[i] = {(DWORD)v[i], (DWORD)(v[i] >> 32)};
        if (!f->read_only && !f->is_dir)
            SetFileTime(f->h, v[0] ? &times[0] : nullptr, v[1] ? &times[1] : nullptr, v[2] ? &times[2] : nullptr);
        break;
    }
    case 10: {  // FileRenameInformation: ReplaceIfExists, RootDirectory, OBJECT_STRING FileName
        if (f->read_only) return STATUS_ACCESS_DENIED;
        std::string name = object_path(rd32(in + 4), read_ansi_string(in + 8));
        std::wstring target;
        bool ro;
        if (!resolve(name, target, ro) || ro) return STATUS_OBJECT_PATH_NOT_FOUND;
        std::vector<u8> buf(sizeof(FILE_RENAME_INFO) + target.size() * 2);
        auto* ri = (FILE_RENAME_INFO*)buf.data();
        ri->ReplaceIfExists = rd8(in) != 0;
        ri->RootDirectory = nullptr;
        ri->FileNameLength = (DWORD)(target.size() * 2);
        memcpy(ri->FileName, target.c_str(), target.size() * 2);
        if (!SetFileInformationByHandle(f->h, FileRenameInfo, ri, (DWORD)buf.size())) {
            status = STATUS_OBJECT_NAME_COLLISION;
        } else {
            f->host_path = target;
            f->xbox_path = name;
        }
        break;
    }
    default:
        log_msg("NtSetInformationFile: class %u not supported (%s)", cls, f->xbox_path.c_str());
        return STATUS_INVALID_PARAMETER;
    }
    io_status(iosb, status, 0);
    return status;
}

static u32 NtQueryFullAttributesFile(Ctx*, KArgs a) {
    std::string path = object_path(a[0]);
    std::wstring host;
    bool ro;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!resolve(path, host, ro) || !GetFileAttributesExW(host.c_str(), GetFileExInfoStandard, &d)) {
        if (g_trace) log_msg("attributes %s: not found", path.c_str());
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    write_network_open_info(a[1], d);
    return STATUS_SUCCESS;
}

static u32 NtQueryVolumeInformationFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f) return STATUS_INVALID_HANDLE;
    u32 iosb = a[1], out = a[2], len = a[3], cls = a[4];
    constexpr u32 SECTOR = 512, SECTORS_PER_CLUSTER = 32;
    const u64 cluster = SECTOR * SECTORS_PER_CLUSTER;
    u64 total = f->read_only ? 0x1C000000ull : 0x80000000ull, avail = f->read_only ? 0 : 0x60000000ull;
    u32 n;
    switch (cls) {
    case 1:  // FileFsVolumeInformation
        n = 0x18;
        if (len < n) return STATUS_INFO_LENGTH_MISMATCH;
        memset(gptr(out), 0, n);
        wr32(out + 8, 0x4E4C3031);  // serial
        break;
    case 3:  // FileFsSizeInformation
        n = 0x18;
        wr64(out + 0, total / cluster);
        wr64(out + 8, avail / cluster);
        wr32(out + 16, SECTORS_PER_CLUSTER);
        wr32(out + 20, SECTOR);
        break;
    case 4:  // FileFsDeviceInformation
        n = 8;
        wr32(out + 0, f->read_only ? 2 /* CD_ROM */ : 7 /* DISK */);
        wr32(out + 4, 0);
        break;
    case 5:  // FileFsAttributeInformation
        n = 16;
        if (len < n) return STATUS_INFO_LENGTH_MISMATCH;
        wr32(out + 0, 0);
        wr32(out + 4, 42);  // FATX name length limit
        wr32(out + 8, 4);
        memcpy(gptr(out + 12), "FATX", 4);
        break;
    case 7:  // FileFsFullSizeInformation
        n = 0x20;
        wr64(out + 0, total / cluster);
        wr64(out + 8, avail / cluster);
        wr64(out + 16, avail / cluster);
        wr32(out + 24, SECTORS_PER_CLUSTER);
        wr32(out + 28, SECTOR);
        break;
    default:
        log_msg("NtQueryVolumeInformationFile: class %u not supported", cls);
        return STATUS_INVALID_PARAMETER;
    }
    io_status(iosb, STATUS_SUCCESS, n);
    return STATUS_SUCCESS;
}

// One FILE_DIRECTORY_INFORMATION entry (with an ANSI name) per call.
static u32 NtQueryDirectoryFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    if (!f || !f->is_dir) return STATUS_INVALID_HANDLE;
    u32 iosb = a[4], out = a[5], len = a[6], cls = a[7], mask_ptr = a[8], restart = a[9] & 0xFF;
    if (cls != 1) {
        log_msg("NtQueryDirectoryFile: class %u not supported", cls);
        return STATUS_INVALID_PARAMETER;
    }
    WIN32_FIND_DATAW fd;
    bool first = !f->find_started || restart;
    for (;;) {
        BOOL ok;
        if (first) {
            if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
            std::string mask = mask_ptr ? read_ansi_string(mask_ptr) : "*";
            if (mask.empty()) mask = "*";
            f->find = FindFirstFileW((f->host_path + L"\\" + widen(mask)).c_str(), &fd);
            f->find_started = true;
            ok = f->find != INVALID_HANDLE_VALUE;
        } else {
            ok = f->find != INVALID_HANDLE_VALUE && FindNextFileW(f->find, &fd);
        }
        if (!ok) {
            u32 status = first ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
            io_status(iosb, status, 0);
            return status;
        }
        first = false;
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) break;
    }
    std::string name = narrow(fd.cFileName);
    u32 need = 0x40 + (u32)name.size();
    if (len < need) return STATUS_BUFFER_TOO_SMALL;
    u64 size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    wr32(out + 0x00, 0);
    wr32(out + 0x04, 0);
    wr64(out + 0x08, ft64(fd.ftCreationTime));
    wr64(out + 0x10, ft64(fd.ftLastAccessTime));
    wr64(out + 0x18, ft64(fd.ftLastWriteTime));
    wr64(out + 0x20, ft64(fd.ftLastWriteTime));
    wr64(out + 0x28, size);
    wr64(out + 0x30, (size + 0x3FFF) & ~0x3FFFull);
    wr32(out + 0x38, xbox_attributes(fd.dwFileAttributes));
    wr32(out + 0x3C, (u32)name.size());
    memcpy(gptr(out + 0x40), name.data(), name.size());
    io_status(iosb, STATUS_SUCCESS, need);
    complete_io(a[1], a[2]);
    return STATUS_SUCCESS;
}

static u32 NtDeviceIoControlFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    u32 code = a[5], out = a[8], out_len = a[9], iosb = a[4];
    constexpr u32 IOCTL_GET_DRIVE_GEOMETRY = 0x00070000, IOCTL_GET_PARTITION_INFO = 0x00074004;
    if (code == IOCTL_GET_PARTITION_INFO && out_len >= 32 && f && f->raw_partition >= 0) {
        const Partition& p = kPartitions[f->raw_partition];
        memset(gptr(out), 0, 32);
        wr64(out + 0, p.offset);
        wr64(out + 8, p.length);
        wr32(out + 16, (u32)(p.offset / 512));
        wr32(out + 20, (u32)f->raw_partition);
        wr8(out + 24, 0x0C);  // PartitionType
        wr8(out + 26, 1);     // RecognizedPartition
        io_status(iosb, STATUS_SUCCESS, 32);
        return STATUS_SUCCESS;
    }
    if (code == IOCTL_GET_DRIVE_GEOMETRY && out_len >= 24) {
        wr64(out + 0, kPartitions[0].length / (255 * 63 * 512));
        wr32(out + 8, 12);   // FixedMedia
        wr32(out + 12, 255);
        wr32(out + 16, 63);
        wr32(out + 20, 512);
        io_status(iosb, STATUS_SUCCESS, 24);
        return STATUS_SUCCESS;
    }
    log_msg("NtDeviceIoControlFile(%s, code %08X) not supported", f ? f->xbox_path.c_str() : "?", code);
    io_status(iosb, STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static u32 NtFsControlFile(Ctx*, KArgs a) {
    HostFile* f = get_file(a[0]);
    constexpr u32 FSCTL_LOCK = 0x00090018, FSCTL_UNLOCK = 0x0009001C, FSCTL_DISMOUNT = 0x00090020;
    if (a[5] == FSCTL_LOCK || a[5] == FSCTL_UNLOCK || a[5] == FSCTL_DISMOUNT) {
        io_status(a[4], STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    log_msg("NtFsControlFile(%s, code %08X) not supported", f ? f->xbox_path.c_str() : "?", a[5]);
    io_status(a[4], STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

// ------------------------------------------------------------ symbolic links
static u32 IoCreateSymbolicLink(Ctx*, KArgs a) {
    std::string link = read_ansi_string(a[0]), target = read_ansi_string(a[1]);
    g_links[lower(link)] = target;
    log_msg("symlink %s -> %s", link.c_str(), target.c_str());
    // Make sure the host directory behind a new drive letter exists.
    std::wstring host;
    bool ro;
    if (resolve(target, host, ro) && !ro) {
        for (size_t i = g_hdd_dir.size() + 1; i <= host.size(); i++)
            if (i == host.size() || host[i] == L'\\') CreateDirectoryW(host.substr(0, i).c_str(), nullptr);
    }
    return STATUS_SUCCESS;
}

static u32 IoDeleteSymbolicLink(Ctx*, KArgs a) {
    return g_links.erase(lower(read_ansi_string(a[0]))) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

static u32 IoDismountVolumeByName(Ctx*, KArgs) { return STATUS_SUCCESS; }

static u32 NtOpenSymbolicLinkObject(Ctx*, KArgs a) {
    std::string name = lower(read_ansi_string(rd32(a[1] + 4)));
    auto it = g_links.find(name);
    if (it == g_links.end()) return STATUS_OBJECT_NAME_NOT_FOUND;
    HandleEntry e{HType::Symlink};
    e.link = it->second;
    wr32(a[0], handle_create(e));
    return STATUS_SUCCESS;
}

static u32 NtQuerySymbolicLinkObject(Ctx*, KArgs a) {
    HandleEntry* e = handle_get(a[0]);
    if (!e || e->type != HType::Symlink) return STATUS_INVALID_HANDLE;
    u32 str = a[1];
    u32 max = rd16(str + 2), buf = rd32(str + 4);
    u32 n = (u32)e->link.size();
    if (a[2]) wr32(a[2], n);
    if (n > max) return STATUS_BUFFER_TOO_SMALL;
    memcpy(gptr(buf), e->link.data(), n);
    wr16(str, (u16)n);
    return STATUS_SUCCESS;
}

void file_init(const char* game_dir) {
    wchar_t full[MAX_PATH];
    GetFullPathNameW(widen(game_dir).c_str(), MAX_PATH, full, nullptr);
    g_game_dir = full;
    GetFullPathNameW(L"hdd", MAX_PATH, full, nullptr);
    g_hdd_dir = full;
    CreateDirectoryW(g_hdd_dir.c_str(), nullptr);
    for (wchar_t p = L'1'; p <= L'5'; p++) CreateDirectoryW((g_hdd_dir + L"\\Partition" + p).c_str(), nullptr);
    g_links["\\??\\d:"] = "\\Device\\CdRom0";
    g_links["\\??\\c:"] = "\\Device\\Harddisk0\\Partition2";
    g_links["\\??\\e:"] = "\\Device\\Harddisk0\\Partition1";
    log_msg("files: disc = %s, hdd = %s", narrow(g_game_dir).c_str(), narrow(g_hdd_dir).c_str());
}

void kernel_register_files() {
    kreg(190, NtCreateFile);
    kreg(202, NtOpenFile);
    kreg(219, NtReadFile);
    kreg(236, NtWriteFile);
    kreg(198, NtFlushBuffersFile);
    kreg(211, NtQueryInformationFile);
    kreg(226, NtSetInformationFile);
    kreg(210, NtQueryFullAttributesFile);
    kreg(218, NtQueryVolumeInformationFile);
    kreg(207, NtQueryDirectoryFile);
    kreg(196, NtDeviceIoControlFile);
    kreg(200, NtFsControlFile);
    kreg(67, IoCreateSymbolicLink);
    kreg(69, IoDeleteSymbolicLink);
    kreg(91, IoDismountVolumeByName);
    kreg(203, NtOpenSymbolicLinkObject);
    kreg(215, NtQuerySymbolicLinkObject);
}
