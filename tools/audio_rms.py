"""Print per-second RMS and peak of an NL_AUDIO_DUMP capture (48 kHz stereo s16)."""
import array
import math
import sys

d = open(sys.argv[1], "rb").read()
a = array.array("h")
a.frombytes(d[: len(d) // 4 * 4])
sec = 48000 * 2
print(f"{len(a) / sec:.1f} s")
for s in range(len(a) // sec):
    b = a[s * sec : (s + 1) * sec]
    l, r = b[0::2], b[1::2]
    rms = lambda x: round(math.sqrt(sum(v * v for v in x) / len(x)))
    print(s, "rms L", rms(l), "R", rms(r), "peak", max(abs(v) for v in b))
