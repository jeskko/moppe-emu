"""
NMT FFSK at audio level: 1200 bit/s, a 1 is one cycle of 1200 Hz, a 0
one and a half cycles of 1800 Hz, phase continuous.

    from ffsk import Modulator, Demodulator
    mod = Modulator(48000)
    samples = mod.bits([1, 0, 1, ...])      # float samples, amplitude 1
    samples = mod.silence(n)                # carrier without modulation

    dem = Demodulator(48000)
    bits = dem.feed(samples)                # bits found so far, in order

The demodulator is non-coherent: for each bit period it compares the
energy at 1200 and 1800 Hz, sampled at the bit phase that gives the
clearest decisions; the phase is re-estimated on every block, so slow
clock drift is followed.  Needs numpy.
"""
import math

import numpy as np

BAUD = 1200.0


class Modulator:
    def __init__(self, rate=48000):
        self.rate = rate
        self.phase = 0.0                # cycles
        self.t = 0.0                    # samples owed (fractional)

    def _tone(self, hz, n):
        k = np.arange(n)
        out = np.sin(2 * math.pi * (self.phase + hz * k / self.rate))
        self.phase = (self.phase + hz * n / self.rate) % 1.0
        return out

    def bits(self, bits):
        out = []
        for b in bits:
            self.t += self.rate / BAUD
            n = int(self.t)
            self.t -= n
            out.append(self._tone(1200.0 if b else 1800.0, n))
        return np.concatenate(out) if out else np.zeros(0)

    def silence(self, nbits):
        """nbits bit periods of carrier without FFSK (zeros); the phase
        restarts afterwards"""
        n = 0
        for _ in range(nbits):
            self.t += self.rate / BAUD
            k = int(self.t)
            self.t -= k
            n += k
        return np.zeros(n)


class Demodulator:
    """Streaming: feed() any block sizes; returns decided bits (1, 0, or
    -1 for a bit period without signal)."""

    def __init__(self, rate=48000, level=0.05):
        self.rate = rate
        self.spb = rate / BAUD           # samples per bit
        self.n = int(round(self.spb))
        k = np.arange(self.n)
        self.w1 = np.exp(-2j * math.pi * 1200.0 * k / rate)
        self.w0 = np.exp(-2j * math.pi * 1800.0 * k / rate)
        self.level = level * self.n / 2  # tone magnitude for "carrier"
        self.buf = np.zeros(0)
        self.base = 0                    # absolute index of buf[0]
        self.next = None                 # absolute start of the next bit window

    def _metric(self, x):
        """for each start sample: (|1200| - |1800|, |1200| + |1800|) over
        one bit"""
        c1 = np.abs(np.convolve(x, self.w1[::-1], mode="valid"))
        c0 = np.abs(np.convolve(x, self.w0[::-1], mode="valid"))
        return c1 - c0, c1 + c0

    def _phase(self, d):
        nb = int((len(d) - self.n) // self.spb)
        if nb < 8:
            return None
        idx = (np.arange(nb)[:, None] * self.spb
               + np.arange(self.n)[None, :]).astype(int)
        return int(np.argmax(np.abs(d[idx]).mean(axis=0)))

    def feed(self, samples):
        self.buf = np.concatenate([self.buf, np.asarray(samples, float)])
        if len(self.buf) < 2 * self.n:
            return []
        d, tot = self._metric(self.buf)          # d[i]: window buf[i:i+n]
        if self.next is None:
            ph = self._phase(d)
            if ph is None:
                return []
            self.next = self.base + ph
        out = []
        while True:
            i = int(round(self.next - self.base))
            if i + 2 >= len(d):
                break
            if i < 1:
                self.next += self.spb
                continue
            if tot[i] < self.level:
                out.append(-1)
            else:
                out.append(1 if d[i] > 0 else 0)
                # timing: move toward the neighbour with the clearer decision
                a, b = abs(d[i - 1]), abs(d[i + 1])
                if a > abs(d[i]) and a > b:
                    self.next -= 0.25
                elif b > abs(d[i]) and b > a:
                    self.next += 0.25
            self.next += self.spb
        # drop samples that no future window needs
        keep_from = int(self.next - self.base) - 2
        if keep_from > 0:
            self.buf = self.buf[keep_from:]
            self.base += keep_from
        return out
