"""
Python harness for the Nokia R40 (RC40 / RD40) emulator (libr40.so):
the L100 logic board running the original Nokia firmware, with a CU43
control head (or a CU43PROG service head).

    from r40emu import Radio
    r = Radio("ABSBIN")
    r.run(6.0)
    print(r.display())        # ['Self test', 'Service necessary', 'Error 6']

The display text is decoded from the LCD drivers' RAM with the
firmware's own 5 x 7 font, found in the ROM image.
"""
import ctypes as C
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.environ.get("R40_LIB", os.path.join(HERE, "..", "libr40.so"))

EVENTS = {1: "ILLEGAL", 2: "EXC", 3: "OUT0", 4: "OUT1", 5: "SR", 6: "DAC",
          7: "SYNTH", 8: "I2C", 9: "MODEM", 10: "SCI_TX", 11: "FX803",
          12: "TX_ON", 13: "TX_OFF", 14: "DTMF", 15: "POWEROFF"}
STOP = {0: "time", 1: "break", 2: "watch", 3: "off"}

# CU43 keypad: (row on IC200, column on IC190), found by pressing each
# matrix position in service mode (digits echo, OK runs a test, CLR
# deletes).  The other function keys (FNC, STO, RCL, arrows; 9 positions
# in column 0, column 4 and row 4) are not identified yet.  PWR is not in
# the matrix: power_key().
KEYS = {
    "1": (0, 3), "2": (0, 2), "3": (0, 1),
    "4": (1, 3), "5": (1, 2), "6": (1, 1),
    "7": (2, 3), "8": (2, 2), "9": (2, 1),
    "*": (3, 3), "0": (3, 2), "#": (3, 1),
    "OK": (1, 4), "CLR": (3, 0),
}

# the 'S' glyph of the R40 font; the table starts 0x33 characters before it
_S_GLYPH = bytes([0x8C, 0x92, 0x92, 0x92, 0x62])

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB)
        vp = C.c_void_p
        ip = C.POINTER(C.c_int)
        I = C.c_int
        D = C.c_double
        cp = C.c_char_p
        sig = {
            "r40api_new": (vp, []),
            "r40api_free": (None, [vp]),
            "r40api_load_rom": (I, [vp, cp]),
            "r40api_power": (None, [vp, I]),
            "r40api_powered": (I, [vp]),
            "r40api_run": (I, [vp, D]),
            "r40api_step": (I, [vp]),
            "r40api_time": (D, [vp]),
            "r40api_cpu_hz": (D, [vp]),
            "r40api_set_adc": (None, [vp, I, I]),
            "r40api_adc": (I, [vp, I]),
            "r40api_key": (None, [vp, I, I, I]),
            "r40api_set_onoff": (None, [vp, I]),
            "r40api_set_hook": (None, [vp, I]),
            "r40api_set_ptt": (None, [vp, I]),
            "r40api_set_service_head": (None, [vp, I]),
            "r40api_set_head_cu43": (None, [vp, I]),
            "r40api_modem_rx": (None, [vp, I]),
            "r40api_sci_rx": (None, [vp, I]),
            "r40api_peek": (I, [vp, I]),
            "r40api_poke": (None, [vp, I, I]),
            "r40api_breakpoint": (None, [vp, I, I]),
            "r40api_watch": (None, [vp, I, I]),
            "r40api_watch_addr": (I, [vp]),
            "r40api_out": (I, [vp, I]),
            "r40api_sreg": (I, [vp, I]),
            "r40api_dac": (I, [vp, I]),
            "r40api_pcf": (I, [vp, I]),
            "r40api_oplen": (I, [vp, I]),
            "r40api_sci_baud": (D, [vp]),
            "r40api_read": (None, [vp, I, I, cp]),
            "r40api_write": (None, [vp, I, I, cp]),
            "r40api_nv": (I, [vp, cp, cp]),
            "r40api_eeprom": (None, [vp, cp, cp]),
            "r40api_lcd": (None, [vp, cp]),
            "r40api_pll": (D, [vp, I, C.POINTER(C.c_uint)]),
            "r40api_cpu": (None, [vp, ip]),
            "r40api_event": (I, [vp, C.POINTER(D), ip, ip]),
            "r40api_trace": (I, [vp, I, ip]),
            "r40api_i2c_count": (I, [vp]),
            "r40api_i2c_entry": (I, [vp, I, C.POINTER(D), cp]),
        }
        for name, (res, args) in sig.items():
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
        _lib = L
    return _lib


def font_from_rom(rom):
    """{5 column bytes: character} from the firmware's font table."""
    i = rom.find(_S_GLYPH)
    if i < 0:
        return {}
    base = i - (ord("S") - 0x20) * 5
    font = {}
    for ch in range(0x20, 0x80):
        g = rom[base + (ch - 0x20) * 5: base + (ch - 0x20) * 5 + 5]
        font.setdefault(bytes(b & 0xFE for b in g), chr(ch))
    return font


class Radio:
    def __init__(self, rom, nv=None, service_head=False, cu43=True, power=True):
        L = lib()
        self.m = L.r40api_new()
        self.L = L
        if L.r40api_load_rom(self.m, rom.encode()) < 0:
            raise FileNotFoundError(rom)
        with open(rom, "rb") as f:
            self.font = font_from_rom(f.read())
        L.r40api_set_service_head(self.m, int(service_head))
        L.r40api_set_head_cu43(self.m, int(cu43))
        if nv is not None:
            self.set_nv(nv)
        self.events = []
        if power:
            self.power(True)

    def __del__(self):
        if getattr(self, "m", None):
            self.L.r40api_free(self.m)
            self.m = None

    # ------------------------------------------------------------ running
    def power(self, on):
        self.L.r40api_power(self.m, int(on))

    def run(self, seconds):
        r = self.L.r40api_run(self.m, seconds)
        self._drain()
        return STOP[r]

    def step(self):
        r = self.L.r40api_step(self.m)
        self._drain()
        return STOP[r]

    def time(self):
        return self.L.r40api_time(self.m)

    def _drain(self):
        at, ty, arg = C.c_double(), C.c_int(), C.c_int()
        while self.L.r40api_event(self.m, C.byref(at), C.byref(ty), C.byref(arg)):
            self.events.append((at.value, EVENTS.get(ty.value, ty.value), arg.value))

    def take_events(self, kind=None):
        ev = [e for e in self.events if kind is None or e[1] == kind]
        self.events = [e for e in self.events if not (kind is None or e[1] == kind)]
        return ev

    # ------------------------------------------------------------- inputs
    def key(self, name, down):
        r, c = KEYS[name] if isinstance(name, str) else name
        self.L.r40api_key(self.m, r, c, int(down))

    def press(self, name, hold=0.15, gap=0.15):
        self.key(name, True)
        self.run(hold)
        self.key(name, False)
        self.run(gap)

    def type(self, keys, hold=0.15, gap=0.15):
        """digits and * # as a string, or a list of key names"""
        for k in keys:
            self.press(k, hold, gap)

    def service_mode(self, timeout=40.0):
        """power on holding PWR (a service head fitted) and wait for the
        LOCAL-mode display ('rsl' on line 2); returns the display"""
        self.power(False)
        self.power_key(True)
        self.power(True)
        self.run(5.0)
        self.power_key(False)
        while self.display()[1] != "rsl" and self.time() < timeout:
            self.run(0.5)
        self.run(1.0)
        return self.display()

    def power_key(self, down):
        self.L.r40api_set_onoff(self.m, int(down))

    def hook(self, offhook):
        self.L.r40api_set_hook(self.m, int(offhook))

    def ptt(self, down):
        self.L.r40api_set_ptt(self.m, int(down))

    def set_adc(self, ch, value):
        self.L.r40api_set_adc(self.m, ch, value)

    def modem_rx(self, b):
        self.L.r40api_modem_rx(self.m, b)

    def sci_rx(self, b):
        self.L.r40api_sci_rx(self.m, b)

    # ------------------------------------------------------------ display
    def lcd(self):
        """LCD RAM as bytes: [device][bank][column 0..39], 4 x 4 x 40."""
        buf = C.create_string_buffer(4 * 4 * 40)
        self.L.r40api_lcd(self.m, buf)
        return buf.raw

    def columns(self, bank):
        """the 120 display columns of one bank (8 pixel rows each)."""
        raw = self.lcd()
        out = []
        for col in range(120):
            if col < 16:
                dev, x = 0, 24 + col
            else:
                dev, x = 1 + (col - 16) // 40, (col - 16) % 40
            out.append(raw[(dev * 4 + bank) * 40 + x])
        return out

    def pixels(self):
        """24 rows of 120 pixels, '#' set."""
        rows = []
        for y in range(24):
            cols = self.columns(y // 8)
            rows.append("".join("#" if (b >> (y % 8)) & 1 else "." for b in cols))
        return rows

    # the top row has 20 characters on the same 24 five-column cells as the
    # others; cells 2, 9, 16 and 23 are not characters on the glass
    TOP_GAPS = (2, 9, 16, 23)

    def _cells(self, cols):
        out = []
        for i in range(24):
            g = bytes(b & 0xFE for b in cols[i * 5: i * 5 + 5])
            out.append(self.font.get(g, "?"))
        return out

    def display(self):
        """the three text lines (20, 24, 24 characters), trailing spaces removed."""
        lines = []
        for bank in range(3):
            cells = self._cells(self.columns(bank))
            if bank == 0:
                cells = [ch for i, ch in enumerate(cells) if i not in self.TOP_GAPS]
            lines.append("".join(cells).rstrip())
        return lines

    # ---------------------------------------------------------- inspection
    def peek(self, a):
        return self.L.r40api_peek(self.m, a)

    def poke(self, a, v):
        self.L.r40api_poke(self.m, a, v)

    def read(self, a, n):
        buf = C.create_string_buffer(n)
        self.L.r40api_read(self.m, a, n, buf)
        return buf.raw

    def nv(self):
        buf = C.create_string_buffer(32768)
        self.L.r40api_nv(self.m, buf, None)
        return buf.raw

    def set_nv(self, data):
        data = bytes(data).ljust(32768, b"\0")[:32768]
        self.L.r40api_nv(self.m, None, data)

    def eeprom(self):
        buf = C.create_string_buffer(256)
        self.L.r40api_eeprom(self.m, buf, None)
        return buf.raw

    def cpu(self):
        a = (C.c_int * 16)()
        self.L.r40api_cpu(self.m, a)
        n = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "pc", "sr",
             "cp", "dp", "ep", "tp", "br", "sleeping"]
        return dict(zip(n, a))

    def pc24(self):
        c = self.cpu()
        return c["cp"] << 16 | c["pc"]

    def pll(self, which):
        """(r, sw, n, a, loads, Hz) of the RX (0) or TX (1) synthesizer."""
        a = (C.c_uint * 5)()
        hz = self.L.r40api_pll(self.m, which, a)
        return tuple(a) + (hz,)

    def i2c_log(self, since=0):
        """[(time, bytes)] of completed I2C transfers from index since."""
        n = self.L.r40api_i2c_count(self.m)
        at = C.c_double()
        buf = C.create_string_buffer(48)
        out = []
        for k in range(max(since, n - 4096), n):
            ln = self.L.r40api_i2c_entry(self.m, k, C.byref(at), buf)
            if ln >= 0:
                out.append((at.value, buf.raw[:min(ln, 48)]))
        return out

    def i2c_count(self):
        return self.L.r40api_i2c_count(self.m)

    def out(self, n):
        return self.L.r40api_out(self.m, n)

    def sreg(self, n):
        return self.L.r40api_sreg(self.m, n)

    def breakpoint(self, a, on=True):
        self.L.r40api_breakpoint(self.m, a, int(on))

    def watch(self, lo, hi=None):
        self.L.r40api_watch(self.m, lo, lo if hi is None else hi)

    def trace(self, n=64):
        a = (C.c_int * n)()
        k = self.L.r40api_trace(self.m, n, a)
        return list(a[:k])
