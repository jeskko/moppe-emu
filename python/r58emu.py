"""
Python harness for the R58 emulator (libr58.so via ctypes).

    from r58emu import Radio
    r = Radio("firmware/build/r58.bin", listing="firmware/build/r58.lst")
    r.run(2.0)
    print(r.display())
    r.type("433500#")
"""
import ctypes as C
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.environ.get("R58_LIB", os.path.join(HERE, "..", "libr58.so"))

P8E, P8N = 0, 1
CU53AN, CU58AF = 0, 1
AD_RSSI, AD_SQL, AD_BATT, AD_TPC, AD_FPM, AD_RPM, AD_TP4, AD_IN7 = range(8)

EVENTS = {1: "WDRESET", 2: "NMI", 3: "POWEROFF", 4: "POWERON", 5: "TX_ON",
          6: "TX_OFF", 7: "SYNTH", 8: "MODEM_TX", 9: "MBUS_TX", 10: "GPS_TX",
          11: "LCD"}
STOP = {0: "time", 1: "break", 2: "watch", 3: "off"}

# CU53AN special segment icons, bit k = position 0x43 + 4k
CU53_ICONS = ["V_U", "PHONE", "CLOCK", "COLON_UR", "V_D", "COLON_D",
              "PHONE_NO", "MAST", "COLON_UL", "BAR_L", "STAR", "BAR_R",
              "KEY", "BOOK", "CAR_D", "CAR_U"]

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB)
        vp = C.c_void_p
        sig = {
            "r58api_new": (vp, [C.c_int, C.c_int]),
            "r58api_free": (None, [vp]),
            "r58api_load_rom": (C.c_int, [vp, C.c_char_p]),
            "r58api_load_nv": (C.c_int, [vp, C.c_char_p]),
            "r58api_save_nv": (C.c_int, [vp, C.c_char_p]),
            "r58api_run": (C.c_int, [vp, C.c_double]),
            "r58api_time": (C.c_double, [vp]),
            "r58api_power": (None, [vp, C.c_int]),
            "r58api_powered": (C.c_int, [vp]),
            "r58api_set_adc": (None, [vp, C.c_int, C.c_int]),
            "r58api_set_ptt": (None, [vp, C.c_int]),
            "r58api_set_local": (None, [vp, C.c_int]),
            "r58api_set_hook": (None, [vp, C.c_int]),
            "r58api_set_ccir": (None, [vp, C.c_int]),
            "r58api_set_multiboard": (None, [vp, C.c_int]),
            "r58api_key": (C.c_int, [vp, C.c_int]),
            "r58api_peek": (C.c_int, [vp, C.c_int]),
            "r58api_poke": (None, [vp, C.c_int, C.c_int]),
            "r58api_set_wd_timeout": (None, [vp, C.c_double]),
            "r58api_read": (None, [vp, C.c_int, C.c_int, C.c_char_p]),
            "r58api_write": (None, [vp, C.c_int, C.c_int, C.c_char_p]),
            "r58api_nv": (None, [vp, C.c_char_p, C.c_char_p]),
            "r58api_display": (C.c_int, [vp, C.c_char_p, C.c_char_p]),
            "r58api_display_raw": (C.c_int, [vp, C.c_char_p]),
            "r58api_synth": (None, [vp, C.POINTER(C.c_uint32)]),
            "r58api_latches": (None, [vp, C.c_char_p]),
            "r58api_pit": (None, [vp, C.c_int, C.POINTER(C.c_uint32)]),
            "r58api_cpu": (None, [vp, C.POINTER(C.c_uint32)]),
            "r58api_instructions": (C.c_ulonglong, [vp]),
            "r58api_next_event": (C.c_int, [vp, C.POINTER(C.c_double),
                                            C.POINTER(C.c_int), C.POINTER(C.c_int)]),
            "r58api_bp": (None, [vp, C.c_int, C.c_int]),
            "r58api_wp": (None, [vp, C.c_int, C.c_int]),
            "r58api_stop_addr": (C.c_int, [vp]),
            "r58api_trace": (C.c_int, [vp, C.POINTER(C.c_uint16), C.c_int]),
            "r58api_serial_rx": (None, [vp, C.c_int, C.c_char_p, C.c_int]),
            "r58api_modem_rx": (None, [vp, C.c_char_p, C.c_int]),
            "r58api_cu58_buttons": (None, [vp, C.c_int, C.c_int, C.c_int]),
        }
        for name, (res, args) in sig.items():
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
        _lib = L
    return _lib


def load_symbols(listing):
    """Symbol table from an as80 listing: {name: value}."""
    syms = {}
    rx = re.compile(r"^# (\S+)\s+([a-z])\s+0x([0-9A-F]{4})\s")
    with open(listing, errors="replace") as f:
        for line in f:
            m = rx.match(line)
            if m:
                syms[m.group(1)] = int(m.group(3), 16)
    return syms


class Radio:
    """One emulated radio."""

    def __init__(self, rom, listing=None, card=P8E, cu=CU53AN, nv=None,
                 prescaler=128, tcxo=12.8e6, if_hz=86.5125e6):
        self.L = lib()
        self.m = self.L.r58api_new(card, cu)
        self.cu = cu
        self.card = card
        self.prescaler = prescaler
        self.tcxo = tcxo
        self.if_hz = if_hz
        if self.L.r58api_load_rom(self.m, rom.encode()):
            raise OSError("cannot load ROM %s" % rom)
        self.sym = load_symbols(listing) if listing else {}
        if nv is not None:
            self.set_nv(nv)
        self.events = []

    def __del__(self):
        if getattr(self, "m", None):
            self.L.r58api_free(self.m)
            self.m = None

    # ---- time
    def run(self, seconds):
        rc = self.L.r58api_run(self.m, seconds)
        self._drain()
        return STOP[rc]

    @property
    def time(self):
        return self.L.r58api_time(self.m)

    def _drain(self):
        at, ty, arg = C.c_double(), C.c_int(), C.c_int()
        while self.L.r58api_next_event(self.m, C.byref(at), C.byref(ty), C.byref(arg)):
            self.events.append((at.value, EVENTS.get(ty.value, ty.value), arg.value))

    def take_events(self, kind=None):
        ev = [e for e in self.events if kind is None or e[1] == kind]
        self.events = [e for e in self.events if kind is not None and e[1] != kind]
        return ev

    # ---- memory
    def addr(self, a):
        return self.sym[a] if isinstance(a, str) else a

    def peek(self, a, n=1):
        a = self.addr(a)
        if n == 1:
            return self.L.r58api_peek(self.m, a)
        buf = C.create_string_buffer(n)
        self.L.r58api_read(self.m, a, n, buf)
        return buf.raw

    def poke(self, a, data):
        a = self.addr(a)
        if isinstance(data, int):
            data = bytes([data])
        self.L.r58api_write(self.m, a, len(data), bytes(data))

    def peek16(self, a):
        b = self.peek(a, 2)
        return b[0] | b[1] << 8

    def peek24(self, a):
        b = self.peek(a, 3)
        return b[0] | b[1] << 8 | b[2] << 16

    def poke24(self, a, v):
        self.poke(a, bytes([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff]))

    def nv(self):
        buf = C.create_string_buffer(4096)
        self.L.r58api_nv(self.m, buf, None)
        return buf.raw

    def set_nv(self, data):
        assert len(data) == 4096
        self.L.r58api_nv(self.m, None, bytes(data))

    def save_nv(self, path):
        with open(path, "wb") as f:
            f.write(self.nv())

    # ---- inputs
    def power(self, on):
        self.L.r58api_power(self.m, 1 if on else 0)

    @property
    def powered(self):
        return bool(self.L.r58api_powered(self.m))

    def adc(self, ch, v):
        self.L.r58api_set_adc(self.m, ch, v)

    def ptt(self, on):
        self.L.r58api_set_ptt(self.m, 1 if on else 0)

    def local(self, on):
        self.L.r58api_set_local(self.m, 1 if on else 0)

    def hook(self, offhook):
        self.L.r58api_set_hook(self.m, 1 if offhook else 0)

    def ccir(self, nibble):
        self.L.r58api_set_ccir(self.m, nibble)

    def multiboard(self, v):
        self.L.r58api_set_multiboard(self.m, v)

    def key_down(self, k):
        if not self.L.r58api_key(self.m, ord(k)):
            raise ValueError("unknown key %r" % k)

    def key_up(self):
        self.L.r58api_key(self.m, 0)

    def press(self, k, hold=0.15, gap=0.15):
        """Press and release one key. `hold` must exceed the 100 ms
        debounce; digits held >= 0.5 s become 'long' (0x80|d)."""
        self.key_down(k)
        self.run(hold)
        self.key_up()
        self.run(gap)

    def type(self, keys, hold=0.15, gap=0.15):
        for k in keys:
            self.press(k, hold, gap)

    def serial_rx(self, chan, data):
        self.L.r58api_serial_rx(self.m, chan, bytes(data), len(data))

    def modem_rx(self, data):
        self.L.r58api_modem_rx(self.m, bytes(data), len(data))

    # ---- outputs
    def display(self):
        """(upper, lower) text of the handset display."""
        up = C.create_string_buffer(16)
        lo = C.create_string_buffer(16)
        self.icons_raw = self.L.r58api_display(self.m, up, lo)
        return up.value.decode(), lo.value.decode()

    def icons(self):
        self.display()
        if self.cu == CU53AN:
            return {n for k, n in enumerate(CU53_ICONS) if self.icons_raw >> k & 1}
        return self.icons_raw

    def display_raw(self):
        buf = C.create_string_buffer(64)
        n = self.L.r58api_display_raw(self.m, buf)
        return buf.raw[:n]

    def synth(self):
        o = (C.c_uint32 * 12)()
        self.L.r58api_synth(self.m, o)
        k = ["rx_r", "rx_n", "rx_a", "tx_r", "tx_n", "tx_a", "ctrl", "ext_a",
             "ext_b", "rx_loads", "tx_loads", "ctrl_loads"]
        return dict(zip(k, o))

    def vco_hz(self, tx=False):
        s = self.synth()
        r = s["tx_r"] if tx else s["rx_r"]
        n = s["tx_n"] if tx else s["rx_n"]
        a = s["tx_a"] if tx else s["rx_a"]
        if not r:
            return None
        return (n * self.prescaler + a) * self.tcxo / r

    def rx_hz(self, inj_above=True):
        lo = self.vco_hz(False)
        if lo is None:
            return None
        return lo - self.if_hz if inj_above else lo + self.if_hz

    def tx_hz(self):
        return self.vco_hz(True)

    def latches(self):
        buf = C.create_string_buffer(8)
        self.L.r58api_latches(self.m, buf)
        k = ["out0", "out1", "out2", "da_rfc", "da_txpwr", "pio_a", "pio_b", "csmem"]
        return dict(zip(k, buf.raw))

    def transmitting(self):
        return not (self.latches()["out1"] & 0x80)

    def pit(self, n):
        o = (C.c_uint32 * 4)()
        self.L.r58api_pit(self.m, n, o)
        return dict(zip(["mode", "count", "out", "null"], o))

    def tone_hz(self):
        """Frequency on the 8254 counter-1 tone pin (mode 3), else None."""
        p = self.pit(1)
        if p["mode"] != 3 or p["null"]:
            return None
        return 4032000.0 / (p["count"] or 65536)

    def cpu(self):
        o = (C.c_uint32 * 17)()
        self.L.r58api_cpu(self.m, o)
        k = ["af", "bc", "de", "hl", "ix", "iy", "sp", "pc", "af_", "bc_",
             "de_", "hl_", "i", "r", "iff1", "im", "halted"]
        return dict(zip(k, o))

    # ---- debugging
    def symbolize(self, a):
        best = None
        for n, v in self.sym.items():
            if v <= a and not n.startswith("_") and v < 0x8000:
                if best is None or v > best[1]:
                    best = (n, v)
        return "%s+%d" % (best[0], a - best[1]) if best else "%04x" % a

    def breakpoint(self, a, on=True):
        self.L.r58api_bp(self.m, self.addr(a), 1 if on else 0)

    def watchpoint(self, a, on=True):
        self.L.r58api_wp(self.m, self.addr(a), 1 if on else 0)

    def trace(self, n=64):
        buf = (C.c_uint16 * n)()
        k = self.L.r58api_trace(self.m, buf, n)
        return list(buf[:k])
