"""
Build a working NV RAM image for the Nokia R40 emulator by running the
service-mode procedure on the emulated radio itself, as a technician
with a CU43PROG would (PE1BVU's RD40 70 cm conversion, the R40 service
manual): the firmware writes its own checksums and copies.

    python3 r40nv.py -o r40.nv                 # 70 cm defaults below
    python3 r40nv.py -o r40.nv --ch 433.500 --ch 438.775/431.175 --own 325555

    from r40nv import default_nv
    nv = default_nv(rom)                       # bytes for Radio(rom, nv=nv)

Defaults: band D, 12.5 kHz raster, simplex, calibration 430 / 435 /
440 MHz, D-band tuning defaults from the ROM (tests 172, 190002), own
number 325555, simplex channels in parameter records 030-034 on 433.500,
433.450, 433.475, 433.525, 433.550 MHz with `st` 008 (bit 3: usable for
simplex; dial `*55*30#` .. `*55*34#` in normal mode, `#55#` to leave), squelch levels 121 / 118
(PE1BVU; tests 33, 34), no transmit time limit (759), no telephone
limits (702).  The squelch delays (31, 32) are factory tests that are
not stored ("doesn't affect the radio in the system mode", service
manual), so PE1BVU's 00 settings are left out.  The result boots without error messages into "Number
unobtainable" (there is no trunking network).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from r40emu import Radio   # noqa: E402

BASE_HZ = 400_000_000      # 0-channel of the RD40 (test 18: 64000 x 6.25 kHz)
STEP_HZ = 6_250

DEFAULT_CHANNELS = [(433.500, 433.500), (433.450, 433.450), (433.475, 433.475),
                    (433.525, 433.525), (433.550, 433.550)]


def channel(mhz):
    """physical channel number of a frequency: (f - 400 MHz) / 6.25 kHz"""
    hz = round(mhz * 1e6)
    n, rem = divmod(hz - BASE_HZ, STEP_HZ)
    if rem or not 0 < n < 65536:
        raise ValueError("%.4f MHz is not on the 6.25 kHz raster above 400 MHz" % mhz)
    return n


class Session:
    """the CU43PROG keypad, one test at a time"""

    def __init__(self, rom, nv=None, log=None):
        self.r = Radio(rom, nv=nv, service_head=True, power=False)
        self.log = log
        d = self.r.service_mode()
        if d[1] != "rsl":
            raise RuntimeError("no service mode: %r" % d)

    def say(self, what):
        if self.log:
            self.log("%-28s %s" % (what, " | ".join(self.r.display())))

    def test(self, keys, wait=1.5):
        self.r.type(keys)
        self.r.press("OK", hold=0.3, gap=wait)
        self.say(keys + " OK")

    def store(self):
        self.r.press("FNC")
        self.r.press("STO", hold=0.3, gap=1.0)

    def leave(self):
        self.r.press("*", hold=0.3, gap=1.5)

    def value(self):
        """the setting: the last number on the bottom line"""
        return int(self.r.display()[2].split()[-1])

    def adjust(self, test, target, limit=300):
        """open a test, step its value with UP/DOWN, store, leave"""
        self.test(test, wait=1.0)
        last = None
        for _ in range(limit):
            v = self.value()
            if v == target:
                break
            if v == last:          # no further steps that way: closest value
                break
            last = v
            self.r.press("UP" if v < target else "DOWN", hold=0.2, gap=0.4)
        self.store()
        self.say("%s -> %d" % (test, self.value()))
        self.leave()

    def parameters(self, code="1234"):
        """70 OK code FNC STO: parameter programming"""
        self.test("70", wait=1.0)
        self.r.type(code)
        self.store()
        self.say("parameters")

    def parameter(self, number, *values):
        self.test("%03d" % number, wait=1.0)
        for v in values:
            self.r.type(str(v))
            self.store()
        self.say("parameter %03d" % number)


SIMPLEX_ST = "008"     # parameter record st byte, bit 3: simplex channel (0x34B34)


def default_nv(rom, channels=DEFAULT_CHANNELS, own="325555",
               band=(430.0, 435.0, 440.0), squelch=(121, 118), log=None):
    """run the procedure on an empty NV RAM; returns the 32 KB image"""
    s = Session(rom, log=log)
    # RF set-up, in the order the firmware needs: 0-channels (18), simplex
    # (16), raster and band (15), D-band defaults read from ROM (172,
    # 190002, which overwrites the calibration words), then 10-12
    for t in ("18164000", "18271200", "16200000", "151", "155", "172", "190002"):
        s.test(t, wait=2.0)
    for k, mhz in enumerate(band):
        s.test("1%d%08d" % (k, round(mhz * 1e5)))
    # squelch opening / closing level (33, 34)
    for t, v in zip(("33", "34"), squelch):
        s.adjust(t, v)
    # parameters: own number, simplex channels 1-5, limits off
    s.parameters()
    s.parameter(800, own)
    for k, (rx, tx) in enumerate(channels[:5]):
        s.parameter(30 + k, channel(rx), channel(tx), SIMPLEX_ST)
    s.parameter(759, "000")
    s.parameter(702, "000")
    s.leave()
    s.say("done")
    return s.r.nv()


def boot_messages(rom, nv, seconds=14.0):
    """the distinct displays of a normal power-on, for checking an image"""
    r = Radio(rom, nv=nv)
    seen = []
    while r.time() < seconds:
        r.run(0.2)
        d = r.display()
        if not seen or seen[-1] != d:
            seen.append(d)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-o", "--out", default="r40.nv", help="output file (32 KB)")
    ap.add_argument("--rom", help="R40 EPROM image (default: tests/r40/roms.py)")
    ap.add_argument("--own", default="325555", help="own subscriber number (parameter 800)")
    ap.add_argument("--ch", action="append", metavar="RX[/TX]",
                    help="simplex channel in MHz, up to 5 (parameters 030-034)")
    ap.add_argument("-v", "--verbose", action="store_true", help="show each step")
    a = ap.parse_args()
    rom = a.rom
    if rom is None:
        sys.path.insert(0, os.path.join(HERE, "..", "tests", "r40"))
        import roms
        rom = roms.rom()
    chans = DEFAULT_CHANNELS
    if a.ch:
        chans = []
        for c in a.ch:
            rx, _, tx = c.partition("/")
            chans.append((float(rx), float(tx or rx)))
    nv = default_nv(rom, chans, a.own, log=print if a.verbose else None)
    with open(a.out, "wb") as f:
        f.write(nv)
    last = boot_messages(rom, nv)[-1]
    print("%s written; power-on ends with %s" % (a.out, " | ".join(last)))


if __name__ == "__main__":
    main()
