"""
Python harness for the Talkman MD50/MD59/ME59 emulator (libmd5x.so).

    from md5xemu import Radio
    r = Radio("md59.bin", listing="md59.lst", model="MD59", cu="CU59")
    r.run(3.0)
    print(r.display())
    r.type("433500")

Listings are as06/as1806 output (OH3NWQ mx5x.asm, OH1E md50.asm); their
symbol table is the "# name l 0xADDR dec size" block at the end.
"""
import ctypes as C
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.environ.get("MD5X_LIB", os.path.join(HERE, "..", "libmd5x.so"))

MODELS = {"MD50": 0, "MD59": 1, "ME59": 2}
HANDSETS = {"CU53": 0, "CU59": 1}
INPUTS = ["poweroff", "toff", "offhook", "lowbatt", "ptt", "local",
          "portable", "coldstart", "ign", "ac"]
EVENTS = {1: "WDRESET", 2: "POWEROFF", 3: "POWERON", 4: "TX_ON", 5: "TX_OFF",
          6: "SYNTH", 7: "MODEM_TX", 8: "LCD", 9: "DAC", 10: "ILLEGAL"}
STOP = {0: "time", 1: "break", 2: "watch", 3: "off"}

# raw handset key codes (MM74C923 code; 0x20 = the extra D5 bit), named
# by the labels both firmwares' key tables give them (Finnish layout)
KEYS_COMMON = {"3": 0, "2": 1, "1": 2, "C": 3, "6": 4, "5": 5, "4": 6,
               "S": 7, "9": 8, "8": 9, "7": 10, "R": 11, "#": 12, "0": 13,
               "*": 14, "E": 15, "P": 16, "-": 18, "+": 19}
KEYS = {
    "CU53": dict(KEYS_COMMON, B=17, X=0x28, D=0x29),
    "CU59": dict(KEYS_COMMON, D=17, X=0x20, B=0x21),
}

# 7-segment digit positions (segment bit numbers), as both firmwares'
# tables give them: glyph bits 0..3 at base+0..3, bits 4..6 at base+64..66
UPPER = [32, 36, 40, 44, 48, 52]                    # u5 .. u0
LOWER = [56, 60, 0, 4, 8, 12, 16, 20, 24, 28]       # dl1 dl0 dr7 .. dr0

# icon segments (mx5x.asm SEG_* defines; OH1E's ICO* agree)
ICONS = {
    "CU53": {127: "KEY", 123: "T", 67: "C", 103: "COLON_L", 71: "COLON_R",
             79: "BAR_L", 91: "BAR_R", 115: "S", 119: "F", 75: "D", 83: "I",
             87: "LESS", 95: "N"},
    "CU59": {111: "ARROW", 127: "KEY", 123: "r", 107: "s", 67: "t",
             103: "HANDSET", 71: "COLON_R", 79: "CH", 91: "A", 115: "D",
             119: "I", 75: "LESS", 83: "S", 87: "F", 95: "N"},
}
# handset indicator latch bits (mx5x BIT_*, OH1E LED_*)
LEDS = ["AVAIL", "CALL", "SERV", "KEYLIGHT", "ROAM", "HF", "LCDLIGHT", "BRIGHT"]

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB)
        vp = C.c_void_p
        ip = C.POINTER(C.c_int)
        sig = {
            "md5xapi_new": (vp, [C.c_int, C.c_int, C.c_int]),
            "md5xapi_free": (None, [vp]),
            "md5xapi_load_rom": (C.c_int, [vp, C.c_char_p]),
            "md5xapi_power": (None, [vp, C.c_int]),
            "md5xapi_powered": (C.c_int, [vp]),
            "md5xapi_run": (C.c_int, [vp, C.c_double]),
            "md5xapi_step": (C.c_int, [vp]),
            "md5xapi_time": (C.c_double, [vp]),
            "md5xapi_cpu_hz": (C.c_double, [vp]),
            "md5xapi_set_input": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_key": (None, [vp, C.c_int]),
            "md5xapi_set_squelch": (None, [vp, C.c_int]),
            "md5xapi_set_rx_tone": (None, [vp, C.c_double]),
            "md5xapi_set_adc": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_dac": (C.c_int, [vp]),
            "md5xapi_set_wd_timeout": (None, [vp, C.c_double]),
            "md5xapi_modem_rx": (None, [vp, C.c_int]),
            "md5xapi_peek": (C.c_int, [vp, C.c_int]),
            "md5xapi_poke": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_breakpoint": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_watch": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_watch_addr": (C.c_int, [vp]),
            "md5xapi_latch": (C.c_int, [vp, C.c_int]),
            "md5xapi_leds": (C.c_int, [vp]),
            "md5xapi_lcd_frames": (C.c_uint, [vp]),
            "md5xapi_modem_ticks": (C.c_int, [vp]),
            "md5xapi_read": (None, [vp, C.c_int, C.c_int, C.c_char_p]),
            "md5xapi_write": (None, [vp, C.c_int, C.c_int, C.c_char_p]),
            "md5xapi_ram": (C.c_int, [vp, C.c_char_p, C.c_char_p]),
            "md5xapi_segments": (None, [vp, C.c_char_p]),
            "md5xapi_cpu": (None, [vp, ip]),
            "md5xapi_set_reg": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_synth": (None, [vp, C.POINTER(C.c_uint)]),
            "md5xapi_event": (C.c_int, [vp, C.POINTER(C.c_double), ip, ip]),
            "md5xapi_trace": (C.c_int, [vp, C.c_int, ip]),
            "md5xapi_audio_capture": (None, [vp, C.c_int, C.c_int]),
            "md5xapi_audio": (C.c_int, [vp, C.c_int, C.POINTER(C.c_double),
                                        C.c_char_p]),
            "md5xapi_audio_count": (C.c_int, [vp]),
        }
        for name, (res, args) in sig.items():
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
        _lib = L
    return _lib


def load_symbols(listing):
    """{name: value} from an as06/as1806 listing's symbol table."""
    syms = {}
    rx = re.compile(r"^# (\S+)\s+[a-z]\s+0x([0-9A-F]{4})\s")
    with open(listing, errors="replace") as f:
        for line in f:
            m = rx.match(line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    return syms


class Radio:
    """One emulated radio.  model MD50/MD59/ME59, cu CU53/CU59."""

    def __init__(self, rom, listing=None, model="MD59", cu="CU59", ram=None,
                 ram_size=0, power=True):
        self.model, self.cu = model, cu
        self.L = lib()
        self.m = self.L.md5xapi_new(MODELS[model], HANDSETS[cu], ram_size)
        if self.L.md5xapi_load_rom(self.m, rom.encode()) < 0:
            raise OSError("cannot load " + rom)
        self.syms = load_symbols(listing) if listing else {}
        self.addr2sym = sorted((v, k) for k, v in self.syms.items()
                               if v < 0x8000)
        self.events = []
        self.hz = self.L.md5xapi_cpu_hz(self.m)
        self._font = self._read_font()
        if ram is not None:
            self.ram(ram)
        if power:
            self.power(True)

    def __del__(self):
        if getattr(self, "m", None):
            self.L.md5xapi_free(self.m)
            self.m = None

    # ---- time and power

    def run(self, seconds):
        why = STOP[self.L.md5xapi_run(self.m, seconds)]
        self._collect()
        return why

    def step(self, n=1):
        for _ in range(n):
            self.L.md5xapi_step(self.m)
        self._collect()

    def time(self):
        return self.L.md5xapi_time(self.m)

    def power(self, on=True):
        self.L.md5xapi_power(self.m, 1 if on else 0)

    def powered(self):
        return bool(self.L.md5xapi_powered(self.m))

    def _collect(self):
        at, t, a = C.c_double(), C.c_int(), C.c_int()
        while self.L.md5xapi_event(self.m, C.byref(at), C.byref(t), C.byref(a)):
            self.events.append((at.value, EVENTS.get(t.value, t.value), a.value))

    def take_events(self, kind=None):
        ev = [e for e in self.events if kind is None or e[1] == kind]
        self.events = [e for e in self.events
                       if not (kind is None or e[1] == kind)]
        return ev

    # ---- inputs

    def set_input(self, name, v):
        self.L.md5xapi_set_input(self.m, INPUTS.index(name), 1 if v else 0)

    def set_ptt(self, v):
        self.set_input("ptt", v)

    def set_hook(self, offhook):
        self.set_input("offhook", offhook)

    def set_squelch(self, open_):
        self.L.md5xapi_set_squelch(self.m, 1 if open_ else 0)

    def set_rx_tone(self, hz):
        self.L.md5xapi_set_rx_tone(self.m, float(hz))

    def set_adc(self, ch, v):
        self.L.md5xapi_set_adc(self.m, ch, v)

    def modem_rx(self, byte):
        self.L.md5xapi_modem_rx(self.m, byte)

    def key(self, name):
        """Hold a key (label, see KEYS) or release with None."""
        code = -1 if name is None else KEYS[self.cu][name]
        self.L.md5xapi_key(self.m, code)

    def press(self, name, hold=0.15, gap=0.15):
        self.key(name)
        self.run(hold)
        self.key(None)
        self.run(gap)

    def type(self, keys, hold=0.15, gap=0.15):
        for k in keys:
            self.press(k, hold, gap)

    # ---- memory and symbols

    def sym(self, name):
        return self.syms[name] if isinstance(name, str) else name

    def symbolize(self, addr):
        import bisect
        i = bisect.bisect_right(self.addr2sym, (addr, "\x7f")) - 1
        if i < 0:
            return "%04X" % addr
        v, k = self.addr2sym[i]
        return "%s+%d" % (k, addr - v)

    def peek(self, a):
        return self.L.md5xapi_peek(self.m, self.sym(a))

    def poke(self, a, v):
        self.L.md5xapi_poke(self.m, self.sym(a), v)

    def read(self, a, n):
        buf = C.create_string_buffer(n)
        self.L.md5xapi_read(self.m, self.sym(a), n, buf)
        return buf.raw

    def write(self, a, data):
        self.L.md5xapi_write(self.m, self.sym(a), len(data), bytes(data))

    def ram(self, data=None):
        """Battery RAM image (get, or set when data is given)."""
        buf = C.create_string_buffer(8192)
        n = self.L.md5xapi_ram(self.m, buf, None)
        if data is not None:
            self.L.md5xapi_ram(self.m, None, bytes(data[:n]).ljust(n, b"\0"))
            return None
        return buf.raw[:n]

    def cpu(self):
        out = (C.c_int * 24)()
        self.L.md5xapi_cpu(self.m, out)
        d = {"r": list(out[:16])}
        for i, k in enumerate(["p", "x", "d", "df", "t", "ie", "q", "idle"]):
            d[k] = out[16 + i]
        d["pc"] = d["r"][d["p"]]
        return d

    def breakpoint(self, a, on=True):
        self.L.md5xapi_breakpoint(self.m, self.sym(a), 1 if on else 0)

    def watchpoint(self, a, n=1):
        a = self.sym(a)
        self.L.md5xapi_watch(self.m, a, a + n - 1)

    def trace(self, n=32):
        out = (C.c_int * n)()
        k = self.L.md5xapi_trace(self.m, n, out)
        return list(out[:k])

    # ---- outputs

    def latch(self, n):
        return self.L.md5xapi_latch(self.m, n)

    def leds(self):
        """Handset indicator latch, bit k = the k-th bit shifted out (the
        firmwares' 'indicators' / 'leds' byte, bit 0 first)."""
        v = self.L.md5xapi_leds(self.m)
        return int("{:08b}".format(v)[::-1], 2)

    def tx(self):
        return bool(self.latch(7) & 1)

    def segments(self):
        buf = C.create_string_buffer(16)
        self.L.md5xapi_segments(self.m, buf)
        return buf.raw

    def segment(self, pos):
        return (self.segments()[pos >> 3] >> (pos & 7)) & 1

    def _read_font(self):
        """glyph -> char from the ROM's 'font' table (mx5x/OH1E: 128
        entries by ASCII, bits 0..6 = digit positions)."""
        rev = {}
        base = self.syms.get("font")
        if base is not None:
            tbl = self.read(base, 128)
            # OH1E packs glyph bits 6..4 one position up ("654_3210")
            oh1e = tbl[ord("8")] == 0xEF
            # ambiguous glyphs (5/S, 8/B ...) resolve in this order
            order = ("0123456789 -ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                     "abcdefghijklmnopqrstuvwxyz")
            order += "".join(c for c in map(chr, range(0x21, 0x7f)) if c not in order)
            for ch in order:
                g = tbl[ord(ch)]
                g = ((g & 15) | ((g >> 1) & 0x70)) if oh1e else g & 0x7f
                rev.setdefault(g, ch)
            rev[0] = " "
        return rev

    def _glyph(self, segs, base):
        g = 0
        for i in range(4):
            p = base + i
            g |= ((segs[p >> 3] >> (p & 7)) & 1) << i
        for i in range(3):
            p = base + 64 + i
            g |= ((segs[p >> 3] >> (p & 7)) & 1) << (4 + i)
        return g

    def glyphs(self):
        """Raw 7-segment glyphs (upper 6, lower 10); bit 0 f, 1 e, 2 c,
        3 b, 4 a, 5 g, 6 d (the CU53AN encoding)."""
        segs = self.segments()
        return ([self._glyph(segs, b) for b in UPPER],
                [self._glyph(segs, b) for b in LOWER])

    def display(self):
        """(upper 6 chars, lower 10 chars) decoded with the ROM font."""
        up, lo = self.glyphs()
        return ("".join(self._font.get(g, "?") for g in up),
                "".join(self._font.get(g, "?") for g in lo))

    def icons(self):
        """Names of the lit icon segments."""
        segs = self.segments()
        return {n for p, n in ICONS[self.cu].items() if segs[p >> 3] >> (p & 7) & 1}

    def led_names(self):
        v = self.leds()
        return [n for i, n in enumerate(LEDS) if v >> i & 1]

    def synth(self):
        out = (C.c_uint * 10)()
        self.L.md5xapi_synth(self.m, out)
        k = ["n", "a", "div", "sw", "off_n", "off_a", "off_sw", "loads",
             "off_loads", "nbits"]
        return dict(zip(k, out))

    def vco_hz(self, ref_hz=None):
        """Main PLL VCO frequency: MD5x (N*80+A)*25 kHz, ME59 div*12.5 kHz
        (the reference the OH3NWQ STEP25 / ME59 builds assume)."""
        s = self.synth()
        if ref_hz is None:
            ref_hz = 12500.0 if self.model == "ME59" else 25000.0
        return s["div"] * ref_hz

    def rx_hz(self, ref_hz=None):
        """RX frequency: MD5x VCO - 21.4 MHz IF; ME59 VCO + 81 MHz."""
        v = self.vco_hz(ref_hz)
        return v + 81.0e6 if self.model == "ME59" else v - 21.4e6

    def audio_capture(self, on=True, bit=None):
        self.L.md5xapi_audio_capture(self.m, 1 if on else 0,
                                     -1 if bit is None else bit)

    def audio_edges(self):
        n = self.L.md5xapi_audio_count(self.m)
        t = (C.c_double * max(n, 1))()
        v = C.create_string_buffer(max(n, 1))
        n = self.L.md5xapi_audio(self.m, n, t, v)
        return [(t[i], v.raw[i]) for i in range(n)]

    def tone_hz(self, t0=None, t1=None):
        """Tone frequency of the captured edge train, from the median
        half period (robust to gaps between bursts); 0 if no tone."""
        e = [t for t, _ in self.audio_edges()
             if (t0 is None or t >= t0) and (t1 is None or t <= t1)]
        if len(e) < 3:
            return 0.0
        d = sorted(b - a for a, b in zip(e, e[1:]))
        return 1.0 / (2.0 * d[len(d) // 2])
