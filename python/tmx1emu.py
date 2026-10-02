"""
Python harness for the TMF-1 / TMN-1 (Talkman 520 / 620) emulator
(libtmx1.so): the radio unit and an HSN-2 or HSF-2 handset, each running
its own firmware, linked by MBUS.

    from tmx1emu import Radio
    r = Radio("tmf1.bin", "tmf1.lst", hs_rom="hsn2.bin")
    r.run(6.0)
    print(r.display())               # ('    30 2', '  433500', ' 0')
    r.type("433550#")

Listings are as7810 -d output; their symbol table is the
"# name t 0xADDR dec size" block at the end.
"""
import ctypes as C
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.environ.get("TMX1_LIB", os.path.join(HERE, "..", "libtmx1.so"))

HANDSETS = {None: 0, "NONE": 0, "HSN2": 1, "HSF2": 2}
EVENTS = {1: "WDNMI", 2: "POWEROFF", 3: "POWERON", 4: "TX_ON", 5: "TX_OFF",
          6: "SYNTH", 7: "LFU", 8: "DAC", 9: "DEV", 10: "MODEM_TX",
          11: "MBUS", 12: "DTMF", 13: "ILLEGAL"}
STOP = {0: "time", 1: "break", 2: "watch", 3: "off"}
AN = ["FSKL", "BCR", "BTMP", "RSSI", "PFB", "BATT", "TEMP", "TIMEOUT"]

# keypad scan codes by the character each firmware sends for the key
# (hsn2.asm / hsf2.asm 'scancodes'): digits, # *, M MUTE, O OK, N NEXT,
# P PTT, A R/ALPHA, ? MENU, R RCL, F FCN, C CLR, E handset, H HF, + -
KEYS = {
    "HSN2": {"M": 1, "O": 2, "N": 3, "P": 4, "A": 6, "?": 7, "R": 8,
             "3": 9, "2": 10, "1": 11, "F": 12, "6": 13, "5": 14, "4": 15,
             "C": 16, "9": 17, "8": 18, "7": 19, "E": 20, "#": 21, "0": 22,
             "*": 23, "H": 24, "-": 27, "+": 28},
    "HSF2": {"+": 1, "O": 2, "N": 3, "P": 4, "-": 5, "A": 6, "?": 7,
             "R": 8, "M": 10, "3": 13, "2": 14, "1": 15, "F": 16, "6": 17,
             "5": 18, "4": 19, "C": 20, "9": 21, "8": 22, "7": 23, "E": 24,
             "#": 25, "0": 26, "*": 27, "H": 28},
}
ICONS = ["FCN", "LEFT", "RIGHT", "T", "ALPHA", "KEY", "MINUS"]
LEDS = ["ON", "SERV", "ROAM", "CALL", "HF", "MFT", "BACKLIGHT"]

# HSN-2 PCD3312 / HSF-2 HSIC tone codes, as the handset firmware's tables
DTMF_HSN2 = {0x10 + i: str(i) for i in range(10)}
DTMF_HSN2.update({0x1A: "A", 0x1B: "B", 0x1C: "C", 0x1D: "D", 0x1E: "*",
                  0x1F: "#", 0x3F: "1750", 0x00: None})

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB)
        vp = C.c_void_p
        ip = C.POINTER(C.c_int)
        I = C.c_int
        sig = {
            "tmx1api_new": (vp, [I]),
            "tmx1api_free": (None, [vp]),
            "tmx1api_load_rom": (I, [vp, C.c_char_p]),
            "tmx1api_load_hs_rom": (I, [vp, C.c_char_p]),
            "tmx1api_power": (None, [vp, I]),
            "tmx1api_powered": (I, [vp]),
            "tmx1api_run": (I, [vp, C.c_double]),
            "tmx1api_step": (I, [vp]),
            "tmx1api_time": (C.c_double, [vp]),
            "tmx1api_cpu_hz": (C.c_double, [vp]),
            "tmx1api_set_adc": (None, [vp, I, I]),
            "tmx1api_adc": (I, [vp, I]),
            "tmx1api_set_ptt": (None, [vp, I]),
            "tmx1api_key": (None, [vp, I, I]),
            "tmx1api_set_hook": (None, [vp, I]),
            "tmx1api_power_key": (None, [vp, I]),
            "tmx1api_mbus_send": (None, [vp, I]),
            "tmx1api_mbus_pending": (I, [vp]),
            "tmx1api_modem_rx": (None, [vp, I]),
            "tmx1api_set_if_hz": (None, [vp, C.c_double]),
            "tmx1api_set_wd": (None, [vp, C.c_double, C.c_double]),
            "tmx1api_peek": (I, [vp, I]),
            "tmx1api_poke": (None, [vp, I, I]),
            "tmx1api_hs_peek": (I, [vp, I]),
            "tmx1api_breakpoint": (None, [vp, I, I]),
            "tmx1api_watch": (None, [vp, I, I]),
            "tmx1api_watch_addr": (I, [vp]),
            "tmx1api_hs_type": (I, [vp]),
            "tmx1api_hs_icons": (I, [vp]),
            "tmx1api_hs_leds": (I, [vp]),
            "tmx1api_hs_dtmf": (I, [vp]),
            "tmx1api_hs_port": (I, [vp, I]),
            "tmx1api_hs_hsic": (I, [vp, I]),
            "tmx1api_port": (I, [vp, I]),
            "tmx1api_dev": (I, [vp]),
            "tmx1api_lfu": (I, [vp, I]),
            "tmx1api_dac": (I, [vp, I]),
            "tmx1api_out2": (I, [vp]),
            "tmx1api_oplen": (I, [vp, I]),
            "tmx1api_read": (None, [vp, I, I, C.c_char_p]),
            "tmx1api_write": (None, [vp, I, I, C.c_char_p]),
            "tmx1api_ram": (I, [vp, C.c_char_p, C.c_char_p]),
            "tmx1api_hs_text": (None, [vp, C.c_char_p, C.c_char_p, C.c_char_p]),
            "tmx1api_hs_lcd": (None, [vp, C.c_char_p]),
            "tmx1api_pll": (C.c_double, [vp, I, C.POINTER(C.c_uint)]),
            "tmx1api_cpu": (None, [vp, I, ip]),
            "tmx1api_event": (I, [vp, C.POINTER(C.c_double), ip, ip]),
            "tmx1api_trace": (I, [vp, I, ip]),
            "tmx1api_audio_capture": (None, [vp, I]),
            "tmx1api_audio": (I, [vp, I, C.POINTER(C.c_double), C.c_char_p]),
            "tmx1api_audio_count": (I, [vp]),
        }
        for name, (res, args) in sig.items():
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
        _lib = L
    return _lib


def load_symbols(listing):
    """{name: value} from an as7810 listing's symbol table."""
    syms = {}
    rx = re.compile(r"^# (\S+)\s+[a-z]\s+0x([0-9A-F]{4})\s")
    in_tab = False
    with open(listing, errors="replace") as f:
        for line in f:
            if line.startswith("# Symbols:"):
                in_tab = True
                continue
            if in_tab:
                m = rx.match(line)
                if m:
                    syms[m.group(1)] = int(m.group(2), 16)
    return syms


class Radio:
    """One emulated TMx-1: radio rom/listing, handset HSN2 / HSF2 / None
    (None: nothing on MBUS but what the host sends with mbus_send)."""

    def __init__(self, rom, listing=None, hs_rom=None, handset="HSN2",
                 hs_listing=None, ram=None, power=True):
        self.handset = handset if hs_rom else None
        self.L = lib()
        self.m = self.L.tmx1api_new(HANDSETS[self.handset])
        if self.L.tmx1api_load_rom(self.m, rom.encode()) < 0:
            raise OSError("cannot load " + rom)
        if hs_rom and self.L.tmx1api_load_hs_rom(self.m, hs_rom.encode()) < 0:
            raise OSError("cannot load " + hs_rom)
        self.syms = load_symbols(listing) if listing else {}
        self.hs_syms = load_symbols(hs_listing) if hs_listing else {}
        self.addr2sym = sorted((v, k) for k, v in self.syms.items()
                               if v < 0xC000)
        self.events = []
        self.hz = self.L.tmx1api_cpu_hz(self.m)
        self.held = {}
        if ram is not None:
            self.ram(ram)
        if power:
            self.power(True)

    def __del__(self):
        if getattr(self, "m", None):
            self.L.tmx1api_free(self.m)
            self.m = None

    # ---- time and power

    def run(self, seconds):
        why = STOP[self.L.tmx1api_run(self.m, seconds)]
        self._collect()
        return why

    def step(self, n=1):
        for _ in range(n):
            self.L.tmx1api_step(self.m)
        self._collect()

    def time(self):
        return self.L.tmx1api_time(self.m)

    def power(self, on=True):
        self.L.tmx1api_power(self.m, 1 if on else 0)
        self._collect()

    def powered(self):
        return bool(self.L.tmx1api_powered(self.m))

    def power_key(self, down):
        """Handset power key: switches the unit on when off; when on the
        handset reports it ('Q') and the radio switches itself off."""
        self.L.tmx1api_power_key(self.m, 1 if down else 0)

    def _collect(self):
        at, t, a = C.c_double(), C.c_int(), C.c_int()
        while self.L.tmx1api_event(self.m, C.byref(at), C.byref(t), C.byref(a)):
            self.events.append((at.value, EVENTS.get(t.value, t.value), a.value))

    def take_events(self, kind=None):
        ev = [e for e in self.events if kind is None or e[1] == kind]
        self.events = [e for e in self.events
                       if not (kind is None or e[1] == kind)]
        return ev

    # ---- inputs

    def set_adc(self, ch, v):
        ch = AN.index(ch) if isinstance(ch, str) else ch
        self.L.tmx1api_set_adc(self.m, ch, int(v))

    def set_rssi(self, v):
        """RSSI A/D reading 0..255 (the firmware maps 30..225 to 0..99)."""
        self.set_adc("RSSI", v)

    def set_ptt(self, on):
        """External /PTT (I2DA, PC2)."""
        self.L.tmx1api_set_ptt(self.m, 1 if on else 0)

    def set_hook(self, offhook):
        self.L.tmx1api_set_hook(self.m, 1 if offhook else 0)

    def modem_rx(self, byte):
        self.L.tmx1api_modem_rx(self.m, byte)

    def key(self, name, down=True):
        """Hold (or release) a handset key by its firmware character.
        Without a handset, send the HSN-2's make / break packet instead."""
        if self.handset is None:
            self.mbus_packet("ANY1", "HSN2", ("M" if down else "B") + name)
            return
        code = KEYS[self.handset][name]
        self.L.tmx1api_key(self.m, code, 1 if down else 0)

    def press(self, name, hold=0.15, gap=0.15):
        """The handset scans its keys every 10 ticks of 112 Hz."""
        self.key(name, True)
        self.run(hold)
        self.key(name, False)
        self.run(gap)

    def type(self, keys, hold=0.15, gap=0.15):
        for k in keys:
            self.press(k, hold, gap)

    # ---- MBUS

    def mbus_send(self, data):
        """Bytes onto the MBUS from the host, 9600 8O1."""
        for b in bytes(data):
            self.L.tmx1api_mbus_send(self.m, b)

    def mbus_packet(self, dest, src, payload):
        """0 dest(4) src(4) payload \\4, the firmwares' framing."""
        self.mbus_send(b"\0" + dest.encode() + src.encode()
                       + (payload.encode() if isinstance(payload, str) else payload)
                       + b"\4")

    def mbus_bytes(self, events=None):
        """[(time, byte, error, senders)] from MBUS events (consumed)."""
        ev = self.take_events("MBUS") if events is None else events
        return [(t, a & 0xFF, bool(a & 0x100), a >> 9) for t, _, a in ev]

    @staticmethod
    def mbus_packets(bytes_):
        """Split monitored bytes into packets: (dest, src, payload, senders)."""
        out, cur, who = [], None, 0
        for _, b, err, s in bytes_:
            if b == 0:
                cur, who = bytearray(), s
            elif cur is not None:
                if b == 4:
                    if len(cur) >= 8:
                        out.append((cur[:4].decode("latin-1"),
                                    cur[4:8].decode("latin-1"),
                                    bytes(cur[8:]), who))
                    cur = None
                else:
                    cur.append(b)
        return out

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
        return self.L.tmx1api_peek(self.m, self.sym(a))

    def poke(self, a, v):
        self.L.tmx1api_poke(self.m, self.sym(a), v)

    def read(self, a, n):
        buf = C.create_string_buffer(n)
        self.L.tmx1api_read(self.m, self.sym(a), n, buf)
        return buf.raw

    def write(self, a, data):
        self.L.tmx1api_write(self.m, self.sym(a), len(data), bytes(data))

    def ram(self, data=None):
        """Battery RAM image, 8 KB at E000 (get, or set when data given)."""
        buf = C.create_string_buffer(8192)
        n = self.L.tmx1api_ram(self.m, buf, None)
        if data is not None:
            self.L.tmx1api_ram(self.m, None, bytes(data[:n]).ljust(n, b"\0"))
            return None
        return buf.raw[:n]

    def cpu(self, handset=False):
        out = (C.c_int * 16)()
        self.L.tmx1api_cpu(self.m, 1 if handset else 0, out)
        k = ["v", "a", "b", "c", "d", "e", "h", "l", "ea", "sp", "pc", "psw",
             "ie", "halt", "mk", "irr"]
        return dict(zip(k, out))

    def breakpoint(self, a, on=True):
        self.L.tmx1api_breakpoint(self.m, self.sym(a), 1 if on else 0)

    def watchpoint(self, a, n=1):
        a = self.sym(a)
        self.L.tmx1api_watch(self.m, a, a + n - 1)

    def trace(self, n=32):
        out = (C.c_int * n)()
        k = self.L.tmx1api_trace(self.m, n, out)
        return list(out[:k])

    def disasm(self, a, n=1):
        from upd7810dis import disasm
        a = self.sym(a)
        lines = []
        for _ in range(n):
            t, ln = disasm(self.peek, a)
            lines.append((a, t))
            a = (a + ln) & 0xFFFF
        return lines

    # ---- handset outputs

    def display(self):
        """(middle 8, bottom 8, top 2) characters of the handset LCD
        (blank while the unit is off: the handset is powered from it)."""
        mid, bot, top = (C.create_string_buffer(9), C.create_string_buffer(9),
                         C.create_string_buffer(3))
        if not self.powered():
            return (" " * 8, " " * 8, " " * 2)
        self.L.tmx1api_hs_text(self.m, mid, bot, top)
        return (mid.value.decode("latin-1"), bot.value.decode("latin-1"),
                top.value.decode("latin-1"))

    def lcd(self):
        """((ram, chr) for chip 0, (ram, chr) for chip 3), 128 bytes each."""
        buf = C.create_string_buffer(512)
        self.L.tmx1api_hs_lcd(self.m, buf)
        r = buf.raw
        return (r[0:128], r[128:256]), (r[256:384], r[384:512])

    def icons(self):
        if not self.powered():
            return set()
        v = self.L.tmx1api_hs_icons(self.m)
        return {n for i, n in enumerate(ICONS) if v >> i & 1}

    def leds(self):
        if not self.powered():
            return set()
        v = self.L.tmx1api_hs_leds(self.m)
        return {n for i, n in enumerate(LEDS) if v >> i & 1}

    def dtmf(self):
        """Current handset tone code (0 = none)."""
        return self.L.tmx1api_hs_dtmf(self.m)

    # ---- radio outputs

    def port(self, p):
        return self.L.tmx1api_port(self.m, "ABC".index(p) if isinstance(p, str) else p)

    def tx(self):
        """TXON (PB4): transmitter supply."""
        return bool(self.port("B") & 0x10)

    def pll(self, which="rx"):
        out = (C.c_uint * 4)()
        hz = self.L.tmx1api_pll(self.m, 1 if which == "tx" else 0, out)
        return dict(zip(["r", "sw", "d", "loads"], out), hz=hz)

    def vco_hz(self):
        """RX PLL output (the RX VCO, receive frequency + IF)."""
        return self.pll("rx")["hz"]

    def tx_hz(self):
        """TX PLL output: the transmit frequency."""
        return self.pll("tx")["hz"]

    def lfu(self):
        """MAS7845 registers 0..3 (6 bits each)."""
        return [self.L.tmx1api_lfu(self.m, i) for i in range(4)]

    def audio_open(self):
        """LFU SPK (register 1 bit 4): RX audio to the speaker."""
        return bool(self.lfu()[1] & 0x10)

    def dac(self):
        """MC144111 words in sending order: AFC, BCON, PC coarse, PC fine."""
        return [self.L.tmx1api_dac(self.m, i) for i in range(4)]

    def deviation(self):
        return self.L.tmx1api_dev(self.m) & 7

    def audio_capture(self, on=True):
        """Record i8253 OUT2 (CTCSS / CCIR tone output) edges."""
        self.L.tmx1api_audio_capture(self.m, 1 if on else 0)

    def audio_edges(self):
        n = self.L.tmx1api_audio_count(self.m)
        t = (C.c_double * max(n, 1))()
        v = C.create_string_buffer(max(n, 1))
        n = self.L.tmx1api_audio(self.m, n, t, v)
        return [(t[i], v.raw[i]) for i in range(n)]

    def tone_hz(self, t0=None, t1=None):
        """Tone frequency of the captured edges from the median half
        period; 0 if no tone."""
        e = [t for t, _ in self.audio_edges()
             if (t0 is None or t >= t0) and (t1 is None or t <= t1)]
        if len(e) < 3:
            return 0.0
        d = sorted(b - a for a, b in zip(e, e[1:]))
        return 1.0 / (2.0 * d[len(d) // 2])
