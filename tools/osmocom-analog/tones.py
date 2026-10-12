#!/usr/bin/env python3
"""
Tone levels in a recording (raw s16le mono 48 kHz), per second:

    tones.py ear.raw [FROM TO] [HZ ...]       (default 425 440 700 2000 3955)

rms of the second and the level of each tone (dB, a full-scale sine is
about 0 dB).  For soundtest.sh's ear.raw and net.raw.
"""
import sys

import numpy as np

x = np.frombuffer(open(sys.argv[1], "rb").read(), "<i2") / 32768.0
args = sys.argv[2:]
a, b = (int(args[0]), int(args[1])) if len(args) >= 2 else (0, len(x) // 48000)
tones = [float(f) for f in args[2:]] or [425, 440, 700, 2000, 3955]
for t in range(a, min(b, len(x) // 48000)):
    seg = x[t * 48000:(t + 1) * 48000]
    f = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    fr = np.fft.rfftfreq(len(seg), 1 / 48000)

    def lv(hz):
        return 20 * np.log10(f[np.abs(fr - hz) < 3].max() / len(seg) * 4 + 1e-9)
    print("%4d s  rms %.3f  " % (t, np.sqrt(np.mean(seg ** 2))) +
          "  ".join("%g:%6.1f" % (hz, lv(hz)) for hz in tones))
