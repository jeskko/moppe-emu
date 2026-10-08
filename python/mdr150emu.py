"""
Python harness for the Comarco MDR150 data radio emulator
(libmdr150.so): the 68HC16Z1 logic board with its Am29F010 flash, RAM,
radio module (4094 supplies, MB1504 synthesizer), RX audio into the ADC,
TX audio from the PWM, and the two D25 serial ports.

    from mdr150emu import Radio
    r = Radio(flash_image)            # 128 KB Am29F010 image
    r.run(2.0)
    print(r.serial_text())            # what the radio printed on port 1

APRS in and out:

    r.receive("OH5NXO-1>APRS:>hello")  # AFSK into the receiver
    r.transmitted()                    # TNC2 text of frames it sent

Symbols for breakpoints and dis(): Radio(img, symbols=load_syms(path)),
path = the firmware repo's build/hamdr/hamdr.sym (tools/hc16/hamdr.sh).
"""
import ctypes as C
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.environ.get("MDR150_LIB", os.path.join(HERE, "..", "libmdr150.so"))
sys.path.insert(0, HERE)

import afsk          # noqa: E402
from cpu16dis import disasm   # noqa: E402

EVENTS = {1: "ILLEGAL", 2: "EXC", 3: "RESET", 4: "SR", 5: "PLL", 6: "TX_ON",
          7: "TX_OFF", 8: "LEDS", 9: "FLASH", 10: "UNMAPPED"}
STOP = {0: "time", 1: "break", 2: "watch"}
FSYS = 16777216.0

_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(LIB)
        vp = C.c_void_p
        ip = C.POINTER(C.c_int)
        I = C.c_int
        D = C.c_double
        bp = C.c_char_p
        fp = C.POINTER(C.c_float)
        dp = C.POINTER(C.c_double)
        sig = {
            "mdrapi_new": (vp, []),
            "mdrapi_free": (None, [vp]),
            "mdrapi_load_flash": (I, [vp, bp, I]),
            "mdrapi_power": (None, [vp, I]),
            "mdrapi_run": (I, [vp, D]),
            "mdrapi_step": (I, [vp]),
            "mdrapi_time": (D, [vp]),
            "mdrapi_fsys": (D, [vp]),
            "mdrapi_vco_hz": (D, [vp]),
            "mdrapi_sci_baud": (D, [vp]),
            "mdrapi_oplen": (I, [vp, I]),
            "mdrapi_log_exc": (None, [vp, I]),
            "mdrapi_set_rssi": (None, [vp, I]),
            "mdrapi_set_busy": (None, [vp, I]),
            "mdrapi_set_levels": (None, [vp, I, I]),
            "mdrapi_set_rx_audio": (None, [vp, fp, I, D]),
            "mdrapi_rx_audio_left": (I, [vp]),
            "mdrapi_set_lines": (None, [vp, I, I, I]),
            "mdrapi_serial_in": (None, [vp, I, I]),
            "mdrapi_serial_pending": (I, [vp]),
            "mdrapi_ssel": (I, [vp]),
            "mdrapi_serial_out": (I, [vp, I, bp, I]),
            "mdrapi_radio": (None, [vp, ip]),
            "mdrapi_pwm_count": (I, [vp]),
            "mdrapi_pwm_take": (None, [vp, dp, ip]),
            "mdrapi_read": (None, [vp, I, I, bp]),
            "mdrapi_write": (None, [vp, I, I, bp]),
            "mdrapi_flash": (I, [vp, bp, bp]),
            "mdrapi_cpu": (None, [vp, ip]),
            "mdrapi_breakpoint": (None, [vp, I, I]),
            "mdrapi_event": (I, [vp, dp, ip, ip]),
            "mdrapi_trace": (I, [vp, I, ip]),
            "mdrapi_unmapped": (I, [vp, ip]),
        }
        for name, (res, args) in sig.items():
            f = getattr(L, name)
            f.restype = res
            f.argtypes = args
        _lib = L
    return _lib


class Radio:
    def __init__(self, flash=None, power=True, symbols=None):
        self.L = lib()
        self.r = self.L.mdrapi_new()
        self.events = []
        self.sym = symbols or {}
        self._rsym = None
        self._rx_buf = None
        self.serial = [bytearray(), bytearray()]
        if flash is not None:
            self.load_flash(flash)
        if power:
            self.power(True)

    def __del__(self):
        try:
            if self.r:
                self.L.mdrapi_free(self.r)
                self.r = None
        except Exception:
            pass

    # ---- setup
    def load_flash(self, img):
        img = bytes(img)
        if self.L.mdrapi_load_flash(self.r, img, len(img)):
            raise ValueError("flash image too large")

    def flash(self, new=None):
        """the flash image as it is now (programs and erases included)"""
        out = C.create_string_buffer(0x20000)
        self.L.mdrapi_flash(self.r, out, new)
        return out.raw

    def power(self, on=True):
        self.L.mdrapi_power(self.r, 1 if on else 0)

    # ---- running
    def run(self, s):
        why = self.L.mdrapi_run(self.r, s)
        self._collect()
        return STOP[why]

    def step(self, n=1):
        for _ in range(n):
            self.L.mdrapi_step(self.r)
        self._collect()

    def run_until(self, cond, timeout=10.0, slice=0.01):
        end = self.time() + timeout
        while self.time() < end:
            self.run(slice)
            if cond():
                return True
        return False

    def time(self):
        return self.L.mdrapi_time(self.r)

    def _collect(self):
        at = C.c_double()
        t = C.c_int()
        a = C.c_int()
        while self.L.mdrapi_event(self.r, C.byref(at), C.byref(t), C.byref(a)):
            self.events.append((at.value, EVENTS.get(t.value, t.value), a.value))
        buf = C.create_string_buffer(4096)
        for p in (0, 1):
            while True:
                n = self.L.mdrapi_serial_out(self.r, p, buf, 4096)
                if not n:
                    break
                self.serial[p] += buf.raw[:n]

    def take_events(self, kind=None):
        ev = [e for e in self.events if kind is None or e[1] == kind]
        if kind is None:
            self.events = []
        else:
            self.events = [e for e in self.events if e[1] != kind]
        return ev

    # ---- serial ports (0 = port 1, the configuration port; 1 = port 2)
    def lines(self, port, dtr=False, rts=False):
        self.L.mdrapi_set_lines(self.r, port, 1 if dtr else 0, 1 if rts else 0)

    def send(self, data, port=0):
        if isinstance(data, str):
            data = data.encode("latin-1")
        for b in data:
            self.L.mdrapi_serial_in(self.r, port, b)

    def send_slowly(self, data, port=0, gap=0.002):
        """one byte per gap seconds (for loaders polling a small buffer)"""
        if isinstance(data, str):
            data = data.encode("latin-1")
        for b in data:
            self.L.mdrapi_serial_in(self.r, port, b)
            self.run(gap)

    def serial_pending(self):
        return self.L.mdrapi_serial_pending(self.r)

    def serial_text(self, port=0, take=True):
        s = bytes(self.serial[port]).decode("latin-1")
        if take:
            self.serial[port] = bytearray()
        return s

    def ssel(self):
        return self.L.mdrapi_ssel(self.r)

    def baud(self):
        return self.L.mdrapi_sci_baud(self.r)

    # ---- radio
    def radio(self):
        out = (C.c_int * 7)()
        self.L.mdrapi_radio(self.r, out)
        return {"sr": out[0], "portf": out[1], "tx": bool(out[2]), "power": out[3],
                "leds": out[4], "na": out[5], "r": out[6],
                "vco_hz": self.L.mdrapi_vco_hz(self.r)}

    def leds(self):
        return self.radio()["leds"]

    def log_exceptions(self, on=True):
        """log every exception (EXC events): thousands a second"""
        self.L.mdrapi_log_exc(self.r, 1 if on else 0)

    def set_rssi(self, v):
        self.L.mdrapi_set_rssi(self.r, int(v))

    def set_busy(self, on):
        self.L.mdrapi_set_busy(self.r, 1 if on else 0)

    def set_levels(self, audio=300, noise=150):
        self.L.mdrapi_set_levels(self.r, int(audio), int(noise))

    def rx_audio(self, samples, rate=48000.0):
        """put audio (-1..1) on the channel from now on"""
        buf = (C.c_float * len(samples))(*samples)
        self._rx_buf = buf          # keep alive while the radio reads it
        self.L.mdrapi_set_rx_audio(self.r, buf, len(samples), rate)

    def rx_audio_left(self):
        return bool(self.L.mdrapi_rx_audio_left(self.r))

    def receive(self, text, rate=48000.0, preamble=40, after=0.3):
        """send one AX.25 UI frame (TNC2 text) on the channel as AFSK and
        run until it has been heard"""
        x = afsk.encode(text, rate=int(rate), preamble=preamble, amp=0.9)
        x = [0.0] * int(0.05 * rate) + x + [0.0] * int(0.05 * rate)
        self.rx_audio(x, rate)
        self.run_until(lambda: not self.rx_audio_left(), timeout=len(x) / rate + 1)
        self.run(after)

    def tx_audio(self, rate=48000):
        """the PWMA duty while keyed, resampled to rate (centred, -1..1);
        consumes it"""
        n = self.L.mdrapi_pwm_count(self.r)
        if not n:
            return []
        at = (C.c_double * n)()
        duty = (C.c_int * n)()
        self.L.mdrapi_pwm_take(self.r, at, duty)
        out = []
        t0 = at[0]
        k = 0
        t = t0
        end = at[n - 1]
        while t < end:
            while k + 1 < n and at[k + 1] <= t:
                k += 1
            out.append((duty[k] - 128) / 128.0)
            t += 1.0 / rate
        return out

    def transmitted(self, rate=48000):
        """TNC2 text of the AX.25 frames in the TX audio so far"""
        return afsk.decode(self.tx_audio(rate), rate)

    # ---- memory and CPU
    def read(self, addr, n):
        out = C.create_string_buffer(n)
        self.L.mdrapi_read(self.r, addr, n, out)
        return out.raw

    def write(self, addr, data):
        data = bytes(data)
        self.L.mdrapi_write(self.r, addr, len(data), data)

    def r8(self, addr):
        return self.read(addr, 1)[0]

    def r16(self, addr):
        b = self.read(addr, 2)
        return b[0] << 8 | b[1]

    def cpu(self):
        out = (C.c_int * 17)()
        self.L.mdrapi_cpu(self.r, out)
        k = ["d", "e", "x", "y", "z", "sp", "pc", "ccr", "xk", "yk", "zk", "ek",
             "sk", "pk", "waiting", "hr", "ir"]
        return dict(zip(k, list(out)))

    def pc(self):
        c = self.cpu()
        return c["pk"] << 16 | c["pc"]

    def breakpoint(self, addr, on=True):
        if isinstance(addr, str):
            addr = self.sym[addr]
        self.L.mdrapi_breakpoint(self.r, addr, 1 if on else 0)

    def trace(self, n=64):
        out = (C.c_int * n)()
        n = self.L.mdrapi_trace(self.r, n, out)
        return list(out)[:n]

    def unmapped(self):
        a = C.c_int()
        n = self.L.mdrapi_unmapped(self.r, C.byref(a))
        return n, a.value

    # ---- symbols and disassembly
    def symbol(self, addr):
        if self._rsym is None:
            self._rsym = sorted((v, k) for k, v in self.sym.items())
        best = None
        for v, k in self._rsym:
            if v > addr:
                break
            best = (v, k)
        if best and addr - best[0] < 0x1000:
            return best[1] if addr == best[0] else "%s+%X" % (best[1], addr - best[0])
        return None

    def dis(self, addr, n=10):
        lines = []
        for _ in range(n):
            ln, text = disasm(lambda a: self.r8(a), addr,
                              lambda a: self.symbol(a))
            s = self.symbol(addr)
            lines.append("%05X %-20s %s" % (addr, s or "", text))
            addr += ln
        return "\n".join(lines)


def load_syms(path):
    """symbols from build/hamdr/hamdr.sym of the firmware repo
    (tools/hc16/hamdr.sh): local labels too, "AAAAA name" per line"""
    sym = {}
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if len(p) == 2:
            sym.setdefault(p[1], int(p[0], 16))
    return sym


def load_map(path):
    """symbols from a GNU ld -Map file of the HaMDR build"""
    sym = {}
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if len(p) == 2 and p[0].startswith("0x") and not p[1].startswith("."):
            try:
                sym[p[1]] = int(p[0], 16) & 0xFFFFF
            except ValueError:
                pass
    return sym
