"""XBE (Xbox executable) inspector.

Usage:
    python tools/xbe.py info <default.xbe>
"""
import struct
import sys

ENTRY_XOR = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_XOR = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}

SECTION_FLAGS = {1: "W", 2: "PRELOAD", 4: "X", 8: "INSERTED", 0x10: "HEAD_RO", 0x20: "TAIL_RO"}


class Xbe:
    def __init__(self, data):
        if data[:4] != b"XBEH":
            raise ValueError("not an XBE")
        self.data = data
        h = struct.unpack_from("<I", data, 0x104)[0]
        self.base = h
        (
            self.size_headers,
            self.size_image,
            self.size_image_header,
            self.timedate,
            self.cert_addr,
            self.num_sections,
            self.section_hdrs_addr,
            self.init_flags,
            entry_enc,
            self.tls_addr,
            self.stack_commit,
            self.heap_reserve,
            self.heap_commit,
            self.pe_base,
            self.pe_size,
            self.pe_checksum,
            self.pe_timedate,
            self.debug_path_addr,
            self.debug_file_addr,
            self.debug_ufile_addr,
            thunk_enc,
            self.nonkernel_import_addr,
            self.num_libs,
            self.libs_addr,
            self.kernel_lib_addr,
            self.xapi_lib_addr,
        ) = struct.unpack_from("<26I", data, 0x108)

        # Pick whichever key lands the entry point inside the image.
        for kind, key in ENTRY_XOR.items():
            ep = entry_enc ^ key
            if self.base <= ep < self.base + self.size_image:
                self.kind, self.entry = kind, ep
                break
        else:
            raise ValueError("could not decode entry point")
        self.kernel_thunk = thunk_enc ^ THUNK_XOR[self.kind]

        self.sections = []
        for i in range(self.num_sections):
            off = self.hdr_off(self.section_hdrs_addr) + i * 0x38
            flags, vaddr, vsize, raw, rawsize, name_addr = struct.unpack_from("<6I", self.data, off)
            self.sections.append(
                dict(name=self.cstr(name_addr), flags=flags, vaddr=vaddr, vsize=vsize, raw=raw, rawsize=rawsize)
            )

    def hdr_off(self, va):
        return va - self.base

    def cstr(self, va):
        off = self.hdr_off(va)
        end = self.data.index(b"\0", off)
        return self.data[off:end].decode("latin-1")

    def va_to_off(self, va):
        if va < self.base + self.size_headers:
            return va - self.base
        for s in self.sections:
            if s["vaddr"] <= va < s["vaddr"] + s["rawsize"]:
                return s["raw"] + va - s["vaddr"]
        return None

    def u32(self, va):
        return struct.unpack_from("<I", self.data, self.va_to_off(va))[0]

    def title(self):
        off = self.hdr_off(self.cert_addr)
        title_id = struct.unpack_from("<I", self.data, off + 8)[0]
        name = self.data[off + 0xC : off + 0xC + 80].decode("utf-16-le").split("\0")[0]
        return title_id, name

    def libraries(self):
        libs = []
        for i in range(self.num_libs):
            off = self.hdr_off(self.libs_addr) + i * 16
            name = self.data[off : off + 8].rstrip(b"\0").decode("latin-1")
            major, minor, build, flags = struct.unpack_from("<4H", self.data, off + 8)
            libs.append((name, f"{major}.{minor}.{build}", flags))
        return libs

    def kernel_imports(self):
        imports = []
        va = self.kernel_thunk
        while True:
            v = self.u32(va)
            if v == 0:
                break
            imports.append((va, v & 0x7FFFFFFF))
            va += 4
        return imports


def flag_str(flags):
    return "|".join(n for b, n in SECTION_FLAGS.items() if flags & b) or "-"


def cmd_info(path):
    x = Xbe(open(path, "rb").read())
    tid, name = x.title()
    print(f"Title      : {name} (title id {tid:08X})")
    print(f"Build kind : {x.kind}")
    print(f"Base       : {x.base:08X}  image size {x.size_image:08X}")
    print(f"Entry      : {x.entry:08X}")
    print(f"Kernel thk : {x.kernel_thunk:08X}")
    print(f"TLS        : {x.tls_addr:08X}")
    print(f"Debug path : {x.cstr(x.debug_path_addr) if x.debug_path_addr else '-'}")
    print("\nSections:")
    for s in x.sections:
        print(
            f"  {s['name']:<10} va {s['vaddr']:08X} vsize {s['vsize']:08X} "
            f"raw {s['raw']:08X} rawsize {s['rawsize']:08X}  {flag_str(s['flags'])}"
        )
    print("\nLibraries (XDK):")
    for lname, ver, flags in x.libraries():
        print(f"  {lname:<8} {ver}  flags {flags:04X}")
    imps = x.kernel_imports()
    print(f"\nKernel imports: {len(imps)}")
    print("  " + " ".join(str(o) for _, o in imps))


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "info":
        cmd_info(sys.argv[2])
    else:
        print(__doc__)
        sys.exit(1)
