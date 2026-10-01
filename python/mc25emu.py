"""
Python harness for the MC25 TVL/PTL emulator (libmc25.so).

    from mc25emu import Radio
    r = Radio("mc25.bin", listing="mc25.lst")
    r.run(3.0)
    print(r.display())
    r.type("145500#")

Listings are as06 output of OH5NXO/OH3NWQ mc25.asm; their symbol table
is the "# name l 0xADDR dec size" block at the end.
"""
import ctypes as C
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.environ.get("MC25_LIB", os.path.join(HERE, "..", "libmc25.so"))

INPUTS = ["ptt", "offhook", "squelch", "lock", "program", "office", "1800", "local"]
EVENTS = {1: "WDRESET", 2: "POWERON", 3: "POWEROFF", 4: "TX_ON", 5: "TX_OFF",
          6: "SYNTH", 7: "CCIR_TX", 8: "CCIR_ACK", 9: "DPY", 10: "ILLEGAL"}
STOP = {0: "time", 1: "break", 2: "watch", 3: "off"}

# raw CU41 key codes, in the order of the firmware's keytbl
KEYS = {"1": 0, "4": 1, "7": 2, "*": 3, "2": 4, "5": 5, "8": 6, "0": 7,
        "3": 8, "6": 9, "9": 10, "#": 11, "-": 12, "+": 13, "A": 14,
        "T": 15, "N": 16, "C": 17, "U": 18, "D": 19}

# display codes (mc25.asm chrtbl and C_* constants); other codes are ASCII
DPY_CODES = {0x4F: "0", 0x28: "1", 0x1E: "b", 0x23: "d", 0x2B: "r", 0x26: "o",
             0x7E: "J", 0x7B: "t", 0x4D: "n", 0x6F: "y", 0x20: " "}
LED0 = ["REQA", "REQE", "BUSY", "ON"]
LED1 = ["MAN", "E", "A", "AUT"]

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB)
        vp, ip, cp = C.c_void_p, C.POINTER(C.c_int), C.c_char_p
        sig = {
            "mc25api_new": (vp, []),
            "mc25api_free": (None, [vp]),
            "mc25api_load_rom": (C.c_int, [vp, cp]),
            "mc25api_load_eerom": (C.c_int, [vp, cp]),
            "mc25api_power": (None, [vp, C.c_int]),
            "mc25api_powered": (C.c_int, [vp]),
            "mc25api_run": (C.c_int, [vp, C.c_double]),
            "mc25api_step": (C.c_int, [vp]),
            "mc25api_time": (C.c_double, [vp]),
            "mc25api_set_input": (None, [vp, C.c_int, C.c_int]),
            "mc25api_key": (None, [vp, C.c_int]),
            "mc25api_set_wd_timeout": (None, [vp, C.c_double]),
            "mc25api_peek": (C.c_int, [vp, C.c_int]),
            "mc25api_poke": (None, [vp, C.c_int, C.c_int]),
            "mc25api_breakpoint": (None, [vp, C.c_int, C.c_int]),
            "mc25api_watch": (None, [vp, C.c_int, C.c_int]),
            "mc25api_out": (C.c_int, [vp, C.c_int]),
            "mc25api_ticks": (C.c_int, [vp]),
            "mc25api_dpy_bytes": (C.c_uint, [vp]),
            "mc25api_dpy_commands": (C.c_int, [vp]),
            "mc25api_ccir_rx": (None, [vp, C.c_int, C.c_int, cp]),
            "mc25api_serial_rx": (None, [vp, C.c_int, cp]),
            "mc25api_serial_pending": (C.c_int, [vp]),
            "mc25api_read": (None, [vp, C.c_int, C.c_int, cp]),
            "mc25api_write": (None, [vp, C.c_int, C.c_int, cp]),
            "mc25api_nv": (None, [vp, cp, cp]),
            "mc25api_display": (C.c_int, [vp, cp]),
            "mc25api_cpu": (None, [vp, ip]),
            "mc25api_event": (C.c_int, [vp, C.POINTER(C.c_double), ip, ip]),
            "mc25api_trace": (C.c_int, [vp, C.c_int, ip]),
            "mc25api_q_take": (C.c_int, [vp, C.c_int, C.POINTER(C.c_double), cp]),
            "mc25api_q_level": (C.c_int, [vp]),
        }
        for name, (res, args) in sig.items():
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
        _lib = L
    return _lib


def load_symbols(listing):
    syms = {}
    rx = re.compile(r"^# (\S+)\s+[a-z]\s+0x([0-9A-F]{4})\s")
    with open(listing, errors="replace") as f:
        for line in f:
            m = rx.match(line)
            if m:
                syms[m.group(1)] = int(m.group(2), 16)
    return syms


NV_SIZE = 1024 + 256


def setup_goto(r, label):
    """Step backwards through setup until the display shows label (the
    display needs ~0.3 s per frame to catch up)."""
    for _ in range(120):
        r.run(0.4)
        if r.display().startswith(label):
            return
        r.press("-")
    raise RuntimeError("setup field %r not found (%r)" % (label, r.display()))


def first_time_setup(r, base="14540045", trlo="145400", trhi="146000",
                     freq="145500"):
    """The README's procedure for a new install on a powered radio: 92# T *
    (reset all settings), power cycle, bASEF, TX limits, a frequency and
    a long 0 (clears the stray simplex LEDs).  Returns r.nv()."""
    r.run(3)
    r.type("92#")
    r.press("T")
    r.press("*")
    r.run(0.3)
    r.power(False)
    r.run(0.2)
    r.power(True)
    r.run(3)
    r.type("92#")
    setup_goto(r, "bASEF")
    r.type(base + "#")
    setup_goto(r, "tr Lo")
    r.type(trlo + "#")
    setup_goto(r, "tr H1")
    r.type(trhi + "#")
    r.run(0.4)
    r.press("T")
    r.press("#")
    r.run(0.5)
    r.type(freq + "#")
    r.press("0", hold=2.5)
    r.run(0.5)
    return r.nv()


class Radio:
    """One emulated MC25.  step_khz: the build's STEP_kHz (25, 20, 12, 10)."""

    def __init__(self, rom, listing=None, nv=None, step_khz=25, eerom=None,
                 power=True):
        self.L = lib()
        self.m = self.L.mc25api_new()
        if self.L.mc25api_load_rom(self.m, rom.encode()) < 0:
            raise OSError("cannot load " + rom)
        if eerom:
            self.L.mc25api_load_eerom(self.m, eerom.encode())
        self.syms = load_symbols(listing) if listing else {}
        self.addr2sym = sorted((v, k) for k, v in self.syms.items() if v < 0x8000)
        self.step_khz = step_khz
        self.events = []
        self._q = []
        self._ledtbl = None
        if "ledtbl" in self.syms:
            self._ledtbl = {b: i for i, b in enumerate(self.read("ledtbl", 16))}
        if nv is not None:
            self.nv(nv)
        if power:
            self.power(True)

    def __del__(self):
        if getattr(self, "m", None):
            self.L.mc25api_free(self.m)
            self.m = None

    # ---- time and power

    def run(self, seconds):
        why = STOP[self.L.mc25api_run(self.m, seconds)]
        self._collect()
        return why

    def step(self, n=1):
        for _ in range(n):
            self.L.mc25api_step(self.m)
        self._collect()

    def time(self):
        return self.L.mc25api_time(self.m)

    def power(self, on=True):
        self.L.mc25api_power(self.m, 1 if on else 0)

    def powered(self):
        return bool(self.L.mc25api_powered(self.m))

    def _collect(self):
        at, t, a = C.c_double(), C.c_int(), C.c_int()
        while self.L.mc25api_event(self.m, C.byref(at), C.byref(t), C.byref(a)):
            name = EVENTS.get(t.value, t.value)
            if name != "DPY":           # one per display byte: too many
                self.events.append((at.value, name, a.value))

    def take_events(self, kind=None):
        ev = [e for e in self.events if kind is None or e[1] == kind]
        self.events = [e for e in self.events if not (kind is None or e[1] == kind)]
        return ev

    # ---- inputs

    def set_input(self, name, v):
        self.L.mc25api_set_input(self.m, INPUTS.index(name), 1 if v else 0)

    def set_ptt(self, v):
        self.set_input("ptt", v)

    def set_hook(self, offhook):
        self.set_input("offhook", offhook)

    def set_squelch(self, carrier):
        self.set_input("squelch", carrier)

    def key(self, name):
        """Hold a key (see KEYS) or release with None."""
        self.L.mc25api_key(self.m, -1 if name is None else KEYS[name])

    def press(self, name, hold=0.15, gap=0.15):
        self.key(name)
        self.run(hold)
        self.key(None)
        self.run(gap)

    def type(self, keys, hold=0.15, gap=0.15):
        for k in keys:
            self.press(k, hold, gap)

    def button(self, name, hold=0.3, gap=0.3):
        """Press a switch input (lock, program, office) for 'hold' s."""
        self.set_input(name, True)
        self.run(hold)
        self.set_input(name, False)
        self.run(gap)

    def ccir_rx(self, digits, decoder="EV"):
        """Queue received CCIR digits ('0'-'9', 'A'-'F' hex, 'E' = repeat)
        on the EV or ARP decoder."""
        d = bytes(int(c, 16) for c in digits)
        self.L.mc25api_ccir_rx(self.m, 0 if decoder == "EV" else 1, len(d), d)

    def serial_rx(self, data):
        """Bytes into /EF2 at 1200 8E1 (idle mark)."""
        data = bytes(data)
        self.L.mc25api_serial_rx(self.m, len(data), data)

    def serial_keys(self, text):
        """Keys over the serial port, MobyDick style: a NUL wakes the
        interrupt-time receiver, which then reads characters (digits,
        # * + - A T N C, 'L' = next one long) until EOT."""
        self.serial_rx(b"\0" + text.encode() + b"\4")
        while self.L.mc25api_serial_pending(self.m):
            self.run(0.01)
        self.run(0.05)

    def serial_tx(self):
        """Bytes sent on Q (1200 8N1, idle high) since the last call."""
        n = 4096
        t = (C.c_double * n)()
        v = C.create_string_buffer(n)
        while True:
            k = self.L.mc25api_q_take(self.m, n, t, v)
            self._q += [(t[i], v.raw[i]) for i in range(k)]
            if k < n:
                break
        out, bit = bytearray(), 1.0 / 1200
        e = self._q
        i = 0
        while i < len(e):
            if e[i][1] != 0:             # start bit: Q falls
                i += 1
                continue
            t0 = e[i][0]
            if self.time() < t0 + 10 * bit:
                break                    # incomplete character
            byte = 0
            for b in range(8):
                ts = t0 + (1.5 + b) * bit
                lvl = 1
                for t_, v_ in e:
                    if t_ <= ts:
                        lvl = v_
                    else:
                        break
                byte |= lvl << b
            out.append(byte)
            while i < len(e) and e[i][0] < t0 + 9.5 * bit:
                i += 1
        self._q = e[i:]
        return bytes(out)

    # ---- memory

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
        return self.L.mc25api_peek(self.m, self.sym(a))

    def poke(self, a, v):
        self.L.mc25api_poke(self.m, self.sym(a), v)

    def read(self, a, n):
        buf = C.create_string_buffer(n)
        self.L.mc25api_read(self.m, self.sym(a), n, buf)
        return buf.raw

    def write(self, a, data):
        self.L.mc25api_write(self.m, self.sym(a), len(data), bytes(data))

    def nv(self, data=None):
        """RAM (1024) + X2212 nibbles (256): get, or set when data given."""
        if data is not None:
            self.L.mc25api_nv(self.m, None, bytes(data).ljust(NV_SIZE, b"\0"))
            return None
        buf = C.create_string_buffer(NV_SIZE)
        self.L.mc25api_nv(self.m, buf, None)
        return buf.raw

    def cpu(self):
        out = (C.c_int * 24)()
        self.L.mc25api_cpu(self.m, out)
        d = {"r": list(out[:16])}
        for i, k in enumerate(["p", "x", "d", "df", "t", "ie", "q", "idle"]):
            d[k] = out[16 + i]
        d["pc"] = d["r"][d["p"]]
        return d

    def breakpoint(self, a, on=True):
        self.L.mc25api_breakpoint(self.m, self.sym(a), 1 if on else 0)

    def watchpoint(self, a, n=1):
        a = self.sym(a)
        self.L.mc25api_watch(self.m, a, a + n - 1)

    def trace(self, n=32):
        out = (C.c_int * n)()
        k = self.L.mc25api_trace(self.m, n, out)
        return list(out[:k])

    # ---- outputs

    def out(self, n):
        return self.L.mc25api_out(self.m, n)

    def tx(self):
        return bool(self.out(3) & 0x10)

    def cells(self):
        buf = C.create_string_buffer(16)
        dp = self.L.mc25api_display(self.m, buf)
        return buf.raw, dp

    def display(self):
        """The 14 characters ('.' after a cell with its decimal point)."""
        cells, dp = self.cells()
        s = ""
        for i in range(14):
            s += DPY_CODES.get(cells[i], chr(cells[i]) if 0x20 < cells[i] < 0x7f else "?")
            if dp >> i & 1:
                s += "."
        return s

    def leds(self):
        """Lit LED names (from the two LED cells, via the ROM's ledtbl)."""
        cells, _ = self.cells()
        if not self._ledtbl:
            return []
        out = []
        for cell, names in ((cells[14], LED0), (cells[15], LED1)):
            v = self._ledtbl.get(cell, 0)
            out += [n for i, n in enumerate(names) if v >> i & 1]
        return out

    def divisor(self):
        """Synth divisor P8..P1 (OUT 2); P0 (OUT 1 bit 5) as half step."""
        return self.out(2) + (0.5 if self.out(1) & 0x20 else 0.0)

    def base_hz(self):
        """bASEF from the NOVRAM: divisor-0 frequency, 6 digits in kHz."""
        if "BASE_FREQ" not in self.syms:
            return 145.4e6
        d = self.read("BASE_FREQ", 8)
        try:
            return int("".join("%d" % (b & 15) for b in d[:6])) * 1e3
        except ValueError:
            return 0.0

    def synth_hz(self):
        return self.base_hz() + self.divisor() * self.step_khz * 1e3

    def rx_hz(self):
        return self.synth_hz()

    def tx_hz(self):
        """TX frequency the divisor means: the firmware adds trx_diff
        (the base-frequency difference of the TX chain) during TX."""
        diff = self.peek("trx_diff") if "trx_diff" in self.syms else 0
        diff -= 256 if diff > 127 else 0
        return self.synth_hz() - diff * self.step_khz * 1e3
