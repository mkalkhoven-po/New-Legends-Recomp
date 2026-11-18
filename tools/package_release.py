"""Build the release zip from the current build.

Usage: python tools/package_release.py [--allow-no-license] [--out dist]

Refuses when:
  - the build is not optimised (a failed CMake configure can silently empty the flags);
  - the exe needs a DLL that is neither part of Windows / the graphics driver nor shipped;
  - LICENSE is missing (unless --allow-no-license, for test packages only);
  - anything that looks like game data would end up in the package.

Writes dist/NewLegendsRecomp-<version>-win64.zip.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
OBJDUMP = r"C:\Program Files\LLVM\bin\llvm-objdump.exe"
VC_REDIST = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"
SHIPPED_DLLS = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]
# Part of Windows, or installed with the graphics driver (vulkan-1.dll).
SYSTEM_DLLS = {"kernel32.dll", "user32.dll", "ole32.dll", "winmm.dll", "xinput1_4.dll", "dbghelp.dll",
               "bcrypt.dll", "vulkan-1.dll", "shell32.dll", "comctl32.dll"}
GAME_DATA = re.compile(r"\.(xbe|lin|u|utx|uax|umx|usx|iso|xiso|xmv|wma|bik|sav)$", re.I)


def fail(msg):
    sys.exit("package_release: " + msg)


def version():
    text = open(os.path.join(ROOT, "runtime", "include", "version.h")).read()
    parts = [re.search(r"#define NL_VERSION_%s (\d+)" % k, text).group(1) for k in ("MAJOR", "MINOR", "PATCH")]
    return ".".join(parts)


def check_optimised():
    cache = open(os.path.join(BUILD, "CMakeCache.txt")).read()
    build_type = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.M).group(1).strip()
    if build_type not in ("Release", "RelWithDebInfo"):
        fail("build type is %r, not Release or RelWithDebInfo" % build_type)
    flags = re.search(r"^CMAKE_CXX_FLAGS_%s:STRING=(.*)$" % build_type.upper(), cache, re.M).group(1)
    if not re.search(r"[/-]O[23]", flags):
        fail("CMAKE_CXX_FLAGS_%s has no /O2 or -O3 (%r): reconfigure with LLVM first on PATH"
             % (build_type.upper(), flags))
    ninja = open(os.path.join(BUILD, "build.ninja")).read()
    if "-O2" not in ninja:
        fail("the recompiled game code is not built with -O2")


def check_dlls(exe):
    out = subprocess.run([OBJDUMP, "-p", exe], capture_output=True, text=True, check=True).stdout
    dlls = {m.lower() for m in re.findall(r"DLL Name: (\S+)", out)}
    unknown = sorted(d for d in dlls if d not in SYSTEM_DLLS and d not in SHIPPED_DLLS
                     and not d.startswith("api-ms-win-"))
    if unknown:
        fail("the exe needs DLLs that are neither part of Windows nor shipped: " + ", ".join(unknown))
    return dlls


def vc_runtime_dir():
    dirs = sorted(glob.glob(os.path.join(VC_REDIST, "*", "x64", "Microsoft.VC14*.CRT")))
    if not dirs:
        fail("no Visual C++ redistributable DLLs under " + VC_REDIST)
    return dirs[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--allow-no-license", action="store_true", help="test package without LICENSE")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = ap.parse_args()

    ver = version()
    exe = os.path.join(BUILD, "newlegends.exe")
    if not os.path.exists(exe):
        fail("build/newlegends.exe not found: run build.bat")
    check_optimised()
    check_dlls(exe)
    license_path = os.path.join(ROOT, "LICENSE")
    if not os.path.exists(license_path) and not args.allow_no_license:
        fail("LICENSE is missing (use --allow-no-license only for a test package)")

    name = "NewLegendsRecomp-%s-win64" % ver
    stage = os.path.join(args.out, name)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(os.path.join(stage, "licenses"))
    files = {
        exe: "newlegends.exe",
        os.path.join(ROOT, "README.md"): "README.md",
        os.path.join(ROOT, "THIRD_PARTY_NOTICES.md"): "THIRD_PARTY_NOTICES.md",
        os.path.join(ROOT, "docs", "release-notes-v%s.md" % ver): "RELEASE_NOTES.md",
    }
    if os.path.exists(license_path):
        files[license_path] = "LICENSE.txt"
    crt = vc_runtime_dir()
    for dll in SHIPPED_DLLS:
        files[os.path.join(crt, dll)] = dll
    for lic in glob.glob(os.path.join(ROOT, "licenses", "*.txt")):
        files[lic] = os.path.join("licenses", os.path.basename(lic))
    for src, dst in files.items():
        if not os.path.exists(src):
            fail("missing " + src)
        shutil.copy2(src, os.path.join(stage, dst))
    # No "game" folder: the program sets it up on the first start (it would
    # keep an existing one aside as game.old).

    # Last line of defence: nothing from the game may be in the package.
    for dirpath, _, names in os.walk(stage):
        for n in names:
            if GAME_DATA.search(n):
                fail("game data in the package: " + os.path.join(dirpath, n))

    zip_path = os.path.join(args.out, name + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, _, names in os.walk(stage):
            for n in sorted(names):
                full = os.path.join(dirpath, n)
                z.write(full, os.path.join(name, os.path.relpath(full, stage)))
    print("wrote %s (%.1f MB)" % (zip_path, os.path.getsize(zip_path) / 1e6))


if __name__ == "__main__":
    main()
