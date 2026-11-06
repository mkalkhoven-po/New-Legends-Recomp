"""Boot each level directly and report crashes, hangs and engine errors.

Usage: python tools/smoke_levels.py [--seconds N] [--monkey SEED] [map ...]

The game switches levels by relaunching itself with "-relaunch <map>" in the
launch data page; this starts the runtime the same way, with random input
(NL_MONKEY). Maps default to every game/*.lin. Results go to smoke/: one log
and one screenshot per map, plus report.txt.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from bmp2png import convert

OUT = "smoke"
HANG_MARK = "no frames for"
TITLE_ID = 0x54510001
LAUNCH_PATH = b"\\Device\\CdRom0;default.xbe"
PAGE = 0x1000
LAUNCH_DATA_OFFSET = 0x400


def launch_page(name):
    """LAUNCH_DATA_PAGE: type, title id, launch path; then the data."""
    page = bytearray(PAGE)
    struct.pack_into("<II", page, 0, 0, TITLE_ID)
    page[8 : 8 + len(LAUNCH_PATH)] = LAUNCH_PATH
    cmd = f"-relaunch {name}".encode()
    page[LAUNCH_DATA_OFFSET : LAUNCH_DATA_OFFSET + len(cmd)] = cmd
    path = os.path.join(tempfile.gettempdir(), "newlegends_smoke.bin")
    with open(path, "wb") as f:
        f.write(page)
    return path


def scan_frame(path):
    """Share of pure black and of magenta (NaN colour, NL_DEBUG_NAN) pixels,
    leaving out the letterbox rows at the top and bottom."""
    data = open(path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    h = abs(h)
    black = magenta = total = 0
    for y in range(h // 8, h * 7 // 8, 4):
        row = data[off + y * w * 4 : off + (y + 1) * w * 4]
        for x in range(0, len(row), 16):
            b, g, r = row[x], row[x + 1], row[x + 2]
            total += 1
            black += r < 3 and g < 3 and b < 3
            magenta += r > 250 and g < 5 and b > 250
    return black / total, magenta / total


def run_map(name, seconds, monkey):
    log_path = os.path.join(OUT, name + ".log")
    shutil.rmtree("frames", ignore_errors=True)
    env = dict(os.environ, NL_WATCHDOG="20", NL_DUMP_FRAMES="120", NL_MONKEY=str(monkey), NL_DEBUG_NAN="1",
               NL_LAUNCH_DATA=launch_page(name))
    start = time.time()
    with open(log_path, "wb") as log:
        p = subprocess.Popen([os.path.abspath("build/newlegends.exe")], stdout=log, stderr=log, env=env)
        while p.poll() is None and time.time() - start < seconds:
            time.sleep(1)
        exited = p.poll()
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
        p.wait()
    text = open(log_path, errors="replace").read()
    frames = sorted(glob.glob("frames/*.bmp"), key=os.path.getmtime)
    frame = 0
    if frames:
        frame = int(os.path.basename(frames[-1])[:-4])
        shutil.copy(frames[-1], os.path.join(OUT, name + ".bmp"))
        convert(os.path.join(OUT, name + ".bmp"))
        os.remove(os.path.join(OUT, name + ".bmp"))
    # The frame with the most pure black, skipping loading screens (which are
    # nearly all black) and the first seconds.
    worst = (0, None)
    magenta = 0
    for f in frames[2:]:
        black, nan = scan_frame(f)
        magenta = max(magenta, nan)
        if worst[0] < black < 0.8:
            worst = (black, f)
    if worst[1]:
        shutil.copy(worst[1], os.path.join(OUT, name + "_black.bmp"))
        convert(os.path.join(OUT, name + "_black.bmp"))
        os.remove(os.path.join(OUT, name + "_black.bmp"))
    problems = []
    for pat in (r"^FATAL.*$", r"^\*\*\* host.*$", r"^\[unreal\] appError.*$", r"^\[texture\].*$", r"^\[shader\].*$",
                r"^unimplemented instruction.*$"):
        problems += sorted(set(re.findall(pat, text, flags=re.M)))
    hangs = text.count(HANG_MARK)
    status = "ok"
    if exited not in (None, 0) or any(x.startswith(("FATAL", "*** host")) for x in problems):
        status = "CRASH"
    elif hangs:
        status = f"HANG{hangs}"
    elif exited == 0:
        status = "exit"
    return status, time.time() - start, frame, worst[0], magenta, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=75)
    ap.add_argument("--monkey", type=int, default=1)
    ap.add_argument("maps", nargs="*")
    args = ap.parse_args()
    maps = args.maps or sorted(os.path.basename(p)[:-4] for p in glob.glob("game/*.lin"))
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "report.txt"), "a") as report:
        for m in maps:
            status, took, frame, black, nan, problems = run_map(m, args.seconds, args.monkey)
            line = f"{m:24} {status:6} {took:4.0f}s frame {frame:5} black {black:4.0%} nan {nan:4.0%}"
            print(line, flush=True)
            report.write(line + "\n")
            for p in problems:
                print("    " + p, flush=True)
                report.write("    " + p + "\n")
            report.flush()


if __name__ == "__main__":
    main()
