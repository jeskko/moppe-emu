#!/usr/bin/env python3
"""
Interactive terminal front end for the Comarco MDR150 emulator running
OH5NXO's HaMDR: the serial console on the left, the radio's state and
the AX.25 traffic on the right, and simulated APRS packets on demand.

    python3 emu/python/mdr150tui.py [--flash radio.bin] [--preset digi|kiss]
    python3 emu/python/mdr150tui.py --preset digi --script "wait:5;rf:OH5ABC-9>APRS,WIDE1-1:>hi;wait:4"

Firmware: the flash image from --flash if it exists, else a fresh one
(bootstrap + HaMDR 174 from tests/mdr150/roms.py, `make refs`).  The
image is written back to --flash on quit, and with /save, so settings
saved on the radio with `perm` persist between runs.  --preset configures
a fresh image first: digi = APRS digipeater (Tracker, tracker_digi wide,
144.800, OH3RDX), kiss = KISS TNC (144.800, OH3RDR).

Keys:  typing goes to the radio's serial port 1 (its console)
       Ctrl-A  prompt: a packet in TNC2 form ("SRC>DST,PATH:info") is put
               on the air towards the radio as AFSK; a line starting
               with / is a command (/help lists them)
       Ctrl-B  a beacon from a station near the radio, with WIDE1-1
       Ctrl-F  the same from a far station (outside digi_area)
       Ctrl-K  prompt: a packet sent to the radio as KISS on port 1
       Ctrl-D  toggle DTR on port 1 and power-cycle: command mode / operating
       Ctrl-R  power-cycle the radio

DTR on port 1 is HaMDR's mode switch, read when it starts (the
programming cable has it, the operating cable not): with DTR, or with
defaulted parameters, it runs its command mode with echo and prompt and
the radio does not operate; without, the personality runs (beacons,
digipeating, KISS) and the console still executes commands but neither
echoes nor prompts.  DTR is off by default; --dtr starts with it on.
       Ctrl-W  save the flash (--flash) Ctrl-X  quit

--script runs headless, no curses.  ';'-separated tokens:
    wait:S        run S seconds          type:TEXT  type TEXT and Enter
    rf:TNC2       a packet on the air    kiss:TNC2  a KISS frame on port 1
    /command      as at the prompt
The console and the AX.25 log are printed afterwards.
"""
import argparse
import curses
import os
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tests", "mdr150"))
import afsk                    # noqa: E402
from mdr150emu import Radio    # noqa: E402

RATE = 48000
POWER = ["5W", "1W", "200mW", "50mW"]
# 4094 outputs, active low (hamdr.c RFCMD_*)
SUPPLIES = [("AUDIO", 0x02), ("PLL", 0x04), ("VCO", 0x08), ("RX", 0x10), ("TX", 0x20)]

PRESETS = {
    # reference/mdr150/mdr150_digipeater_configuration.txt, shortened
    "digi": ["personality tracker", "freq 144.800", "mycall OH3RDX", "use_softdcd on",
             "tracker_digi wide", "gps_lat 6141.21,N", "gps_lon 02444.64,E",
             "mysymbol /#", "ttylines 0"],
    "kiss": ["personality kiss", "freq 144.800", "mycall OH3RDR", "use_softdcd on",
             "ttylines 0"],
}

HELP = [
    "/rssi N      RSSI on the ADC (0..1023)       /busy 0|1   BUSY line",
    "/audio N     RX audio peak, ADC counts       /noise N    noise without signal",
    "/dtr 0|1     DTR on port 1 (read at start)   /rts 0|1    RTS on port 1",
    "/kiss TNC2   send a KISS frame on port 1     /speed X    x real time (0 = max)",
    "/near /far   canned beacons                  /if MHz     RX IF offset shown",
    "/save [FILE] write the flash                 /reboot     power-cycle",
    "anything else: a TNC2 packet put on the air towards the radio",
]


def kiss_frame(text):
    fr = afsk.ax25_frame(text)
    return b"\xc0\x00" + fr.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"


class Session:
    """the radio plus what the UI keeps: console text, AX.25 log, the
    queue of packets waiting for the channel"""

    def __init__(self, args):
        self.args = args
        self.flash_path = args.flash
        img = None
        if args.flash and os.path.exists(args.flash):
            img = open(args.flash, "rb").read()
        if img is None:
            import roms
            try:
                img = roms.flash_image()
            except roms.Unavailable as e:
                sys.exit(str(e))
        self.r = Radio(img, power=False)
        self.console = [""]
        self.col = 0                   # cursor column in the last console line
        self.log = []                  # (time, tag, text)
        self.rfq = []                  # TNC2 texts waiting to go on the air
        self.on_air = None
        self.kiss_buf = bytearray()
        self.kiss_in = False
        self.dtr = getattr(args, "dtr", False)
        self.rts = False
        self.if_mhz = 21.4
        self.speed = 1.0
        self.seq = 0
        self.msg = ""
        self.tx_on_at = None
        self.counts = {"rf in": 0, "rf out": 0}
        self.power_on()
        if args.preset:
            self.preset(args.preset)

    # ---- radio i/o
    def preset(self, name):
        self.r.run(0.6)
        self.feed_serial()
        for line in PRESETS[name]:
            self.type(line)
            self.run(0.5)
        self.type("perm")
        self.run(1.0)
        self.reboot()
        self.msg = "preset %s saved with perm, radio rebooted" % name

    def power_on(self):
        """power on as a terminal program would see it: DTR rises 50 ms
        after the supply.  OH5NXO's bootstrap reads DTR in its first
        milliseconds and would wait 10 s for S-records with it asserted
        (the Comarco loader it stands in for does not look at DTR); HaMDR
        reads it at its start, and runs its command mode (echo, prompt)
        only with DTR or with defaulted parameters"""
        self.r.lines(0, dtr=False, rts=self.rts)
        self.r.power(True)
        self.r.run(0.05)
        self.set_lines()

    def type(self, text):
        self.r.send(text + "\r")

    def key(self, b):
        self.r.send(bytes([b]))

    def reboot(self):
        img = self.r.flash()
        self.r.power(False)
        self.r.load_flash(img)
        self.power_on()
        self.rfq.clear()
        self.on_air = None
        self.log_add("--", "power cycled, DTR %s" % ("on" if self.dtr else "off"))

    def set_lines(self):
        self.r.lines(0, dtr=self.dtr, rts=self.rts)

    def save(self, path=None):
        path = path or self.flash_path
        if not path:
            self.msg = "no --flash file to save to (/save FILE)"
            return
        with open(path, "wb") as f:
            f.write(self.r.flash())
        self.msg = "flash saved to %s" % path

    def send_rf(self, text):
        try:
            afsk.ax25_frame(text)
            if ">" not in text.partition(":")[0]:
                raise ValueError
        except Exception:
            self.msg = "not a TNC2 packet: SRC>DST[,PATH]:info"
            return
        self.rfq.append(text)

    def send_kiss(self, text):
        try:
            self.r.send(kiss_frame(text))
        except Exception:
            self.msg = "not a TNC2 packet"
            return
        self.log_add("kiss>", text)

    def canned(self, far=False):
        self.seq += 1
        if far:
            self.send_rf("OH5FAR-9>APRS,WIDE1-1:!6130.00N/02500.00E>far station #%d" % self.seq)
        else:
            self.send_rf("OH5ABC-9>APRS,WIDE1-1:!6140.00N/02445.00E>near station #%d" % self.seq)

    def command(self, line):
        line = line.strip()
        if not line:
            return
        if not line.startswith("/"):
            self.send_rf(line)
            return
        cmd, _, arg = line[1:].partition(" ")
        try:
            if cmd == "help":
                for h in HELP:
                    self.log_add("help", h)
            elif cmd == "rssi":
                self.args.rssi = int(arg)
                self.r.set_rssi(self.args.rssi)
            elif cmd == "noise":
                self.args.noise = int(arg)
                self.r.set_levels(self.args.audio, self.args.noise)
            elif cmd == "audio":
                self.args.audio = int(arg)
                self.r.set_levels(self.args.audio, self.args.noise)
            elif cmd == "busy":
                self.args.busy = int(arg) != 0
                self.r.set_busy(self.args.busy)
            elif cmd == "dtr":
                self.dtr = int(arg) != 0
                self.set_lines()
                self.msg = "DTR %s: takes effect at the next start (/reboot)" % (
                    "on" if self.dtr else "off")
            elif cmd == "rts":
                self.rts = int(arg) != 0
                self.set_lines()
            elif cmd == "kiss":
                self.send_kiss(arg)
            elif cmd == "speed":
                self.speed = float(arg)
            elif cmd == "if":
                self.if_mhz = float(arg)
            elif cmd == "near":
                self.canned()
            elif cmd == "far":
                self.canned(far=True)
            elif cmd == "save":
                self.save(arg or None)
            elif cmd == "reboot":
                self.reboot()
            else:
                self.msg = "unknown command /%s (/help)" % cmd
        except ValueError:
            self.msg = "bad argument for /%s" % cmd

    # ---- running
    def run(self, s):
        """run s seconds in slices, servicing the channel and the outputs"""
        end = self.r.time() + s
        while self.r.time() < end - 1e-9:
            if self.on_air is None and self.rfq:
                text = self.rfq.pop(0)
                x = afsk.encode(text, rate=RATE, preamble=40, amp=0.9)
                self.r.rx_audio([0.0] * int(0.02 * RATE) + x, RATE)
                self.on_air = text
                self.counts["rf in"] += 1
                self.log_add("rf>", text)
            before = self.r.time()
            self.r.run(min(0.02, end - self.r.time()))
            if self.r.time() <= before:
                break
            if self.on_air and not self.r.rx_audio_left():
                self.on_air = None
            self.feed_events()
            self.feed_serial()

    def feed_events(self):
        for at, kind, arg in self.r.take_events():
            if kind == "TX_ON":
                self.tx_on_at = at
            elif kind == "TX_OFF":
                frames = self.r.transmitted(RATE)
                dur = at - self.tx_on_at if self.tx_on_at is not None else 0
                if frames:
                    for f in frames:
                        self.counts["rf out"] += 1
                        self.log_add("<rf", f)
                else:
                    self.log_add("<rf", "(%.2f s of transmission, no frame decoded)" % dur)
                self.tx_on_at = None
            elif kind == "RESET":
                self.log_add("--", "reset (%s)" % {1: "watchdog", 2: "halt"}.get(arg, arg))
            elif kind == "ILLEGAL":
                self.log_add("--", "illegal instruction at %05X" % arg)

    def feed_serial(self):
        data = self.r.serial_text(0)
        if self.r.serial[1]:
            self.r.serial[1] = bytearray()    # port 2: not shown
        for ch in data:
            b = ord(ch)
            self.kiss_byte(b)
            if self.kiss_in or b == 0xC0:
                continue
            line = self.console[-1]
            if ch == "\n":
                self.console.append("")
                self.col = 0
            elif ch == "\r":
                self.col = 0               # overwrite from the start (stat)
            elif b == 7:
                continue
            elif ch == "\b" or b == 0x7F:
                if self.col:
                    self.col -= 1
                    self.console[-1] = line[:self.col] + line[self.col + 1:]
            else:
                if not (32 <= b < 127 or b >= 160):
                    ch = "\u00b7"
                self.console[-1] = line[:self.col] + ch + line[self.col + 1:]
                self.col += 1
        del self.console[:-2000]

    def kiss_byte(self, b):
        """KISS frames in the serial output go to the AX.25 log"""
        if b == 0xC0:
            if self.kiss_in and len(self.kiss_buf) > 1:
                raw = bytes(self.kiss_buf)
                cmd, body = raw[0], raw[1:].replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb")
                if cmd & 0x0F == 0 and len(body) >= 16:
                    note = ""
                    if cmd & 0xA0:            # SMACK ($80) / RMNC ($20): CRC at the end
                        body = body[:-2]
                        note = "  (%s probe)" % ("SMACK" if cmd & 0x80 else "RMNC CRC")
                    try:
                        self.log_add("<kiss", afsk.ax25_text(body) + note)
                    except Exception:
                        self.log_add("<kiss", body.hex() + note)
            self.kiss_in = True
            self.kiss_buf = bytearray()
        elif self.kiss_in:
            self.kiss_buf.append(b)
            if len(self.kiss_buf) > 600:
                self.kiss_in = False

    def log_add(self, tag, text):
        self.log.append((self.r.time(), tag, text))
        del self.log[:-500]

    # ---- the state panel
    def state_lines(self):
        r = self.r
        st = r.radio()
        sr, tx = st["sr"], st["tx"]
        vco = st["vco_hz"] / 1e6
        if not st["r"]:
            freq = "synth not loaded"
        elif tx:
            freq = "TX %.4f MHz" % vco
        else:
            freq = "RX %.4f MHz  (VCO %.4f)" % (vco + self.if_mhz, vco)
        on = [n for n, bit in SUPPLIES if not sr & bit and st["portf"] & 0x20]
        leds = "".join("●" if st["leds"] >> i & 1 else "○" for i in range(6))
        lines = [
            "time %8.2f s   speed %s" % (r.time(), "max" if not self.speed else "x%g" % self.speed),
            "%-34s %s" % (freq, "*** TRANSMITTING ***" if tx else ""),
            "power %-5s  supplies %s%s" % (POWER[st["power"] & 3], " ".join(on) or "off",
                                           "  muted" if sr & 0x80 else ""),
            "LEDs %s  buzzer %s" % (leds, "on" if st["leds"] & 0x40 else "off"),
            "port 1: DTR %s RTS %s   SCI on port %d, %.0f baud" % (
                "on" if self.dtr else "off", "on" if self.rts else "off",
                r.ssel() + 1, r.baud()),
            "RSSI %d  audio %d  noise %d  BUSY %s" % (
                self.args.rssi, self.args.audio, self.args.noise, "on" if self.args.busy else "off"),
            "packets: %d to the radio, %d decoded from it%s" % (
                self.counts["rf in"], self.counts["rf out"],
                ("   on air: " + self.on_air[:30]) if self.on_air else
                ("   queued %d" % len(self.rfq) if self.rfq else "")),
        ]
        return lines

    def log_lines(self, width):
        out = []
        for at, tag, text in self.log:
            s = "%7.2f %-5s %s" % (at, tag, text)
            while len(s) > width:
                out.append(s[:width])
                s = " " * 14 + s[width:]
            out.append(s)
        return out


# ---------------------------------------------------------------- curses

def wrap(lines, width):
    out = []
    for line in lines:
        while len(line) > width:
            out.append(line[:width])
            line = line[width:]
        out.append(line)
    return out


def prompt(stdscr, title, initial=""):
    """one-line editor on the bottom row; None if cancelled"""
    curses.curs_set(1)
    stdscr.nodelay(False)
    buf = initial
    try:
        while True:
            h, w = stdscr.getmaxyx()
            show = (title + buf)[-(w - 1):]
            stdscr.move(h - 1, 0)
            stdscr.clrtoeol()
            stdscr.addstr(h - 1, 0, show, curses.A_REVERSE)
            stdscr.refresh()
            ch = stdscr.get_wch()
            if ch in ("\n", "\r", curses.KEY_ENTER):
                return buf
            if ch in ("\x1b", "\x07"):
                return None
            if ch in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                buf = buf[:-1]
            elif ch == "\x15":           # Ctrl-U
                buf = ""
            elif isinstance(ch, str) and ch.isprintable():
                buf += ch
    finally:
        curses.curs_set(0)
        stdscr.nodelay(True)


def draw(stdscr, s):
    stdscr.erase()
    h, w = stdscr.getmaxyx()
    lw = max(20, w // 2)
    rw = w - lw - 1
    hl = curses.A_BOLD

    def put(y, x, text, attr=0):
        try:
            stdscr.addstr(y, x, text, attr)
        except curses.error:
            pass

    # left: console
    put(0, 0, " serial port 1 ".center(lw, "-"), hl)
    con = wrap(s.console, lw)[-(h - 2):]
    for i, line in enumerate(con):
        put(1 + i, 0, line)
    # divider
    for y in range(h - 1):
        put(y, lw, "|")
    # right: state, then AX.25 log
    put(0, lw + 1, " radio ".center(rw, "-"), hl)
    st = s.state_lines()
    for i, line in enumerate(st):
        attr = curses.A_REVERSE if "TRANSMITTING" in line else 0
        put(1 + i, lw + 1, line[:rw], attr)
    y0 = 2 + len(st)
    put(y0, lw + 1, " AX.25 ".center(rw, "-"), hl)
    logl = s.log_lines(rw)[-(h - 2 - y0):]
    for i, line in enumerate(logl):
        attr = curses.A_BOLD if line[8:13].strip() in ("<rf", "<kiss") else 0
        put(y0 + 1 + i, lw + 1, line, attr)
    # bottom
    hint = ("[command mode: DTR on] " if s.dtr else
            "[operating: no echo/prompt; ^D for command mode] ")
    bar = s.msg or hint + "^A packet/command  ^B near beacon  ^F far  ^K KISS  ^D DTR  ^R reboot  ^W save  ^X quit"
    put(h - 1, 0, bar[:w - 1], curses.A_REVERSE)
    stdscr.refresh()


def main(stdscr, s):
    curses.curs_set(0)
    curses.raw()
    stdscr.nodelay(True)
    stdscr.keypad(True)
    last_rf = "OH5ABC-9>APRS,WIDE1-1:!6140.00N/02445.00E>hello from the TUI"
    t0, emu0 = time.monotonic(), s.r.time()
    while True:
        try:
            ch = stdscr.get_wch()
        except curses.error:
            ch = None
        while ch is not None:
            if ch == "\x18":                       # Ctrl-X
                return
            elif ch == "\x01":                     # Ctrl-A
                line = prompt(stdscr, "on air> ", last_rf)
                if line:
                    if not line.startswith("/"):
                        last_rf = line
                    s.msg = ""
                    s.command(line)
                t0, emu0 = time.monotonic(), s.r.time()
            elif ch == "\x0b":                     # Ctrl-K
                line = prompt(stdscr, "KISS> ", "OH3RDR>APRS:>sent as KISS")
                if line:
                    s.send_kiss(line)
                t0, emu0 = time.monotonic(), s.r.time()
            elif ch == "\x02":
                s.canned()
            elif ch == "\x06":
                s.canned(far=True)
            elif ch == "\x04":
                s.dtr = not s.dtr
                s.reboot()
                s.msg = ("DTR on, power cycled: HaMDR command mode (radio not operating)"
                         if s.dtr else "DTR off, power cycled: radio operating, console silent")
            elif ch == "\x12":
                s.reboot()
            elif ch == "\x17":
                s.save()
            elif ch in ("\n", "\r", curses.KEY_ENTER):
                s.key(13)
                s.msg = ""
            elif ch in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                s.key(8)
            elif isinstance(ch, str) and len(ch) == 1 and ord(ch) < 256:
                s.key(ord(ch))
            elif ch == curses.KEY_RESIZE:
                pass
            try:
                ch = stdscr.get_wch()
            except curses.error:
                ch = None

        if s.speed:
            target = emu0 + (time.monotonic() - t0) * s.speed
            if target - s.r.time() > 0.5:          # fell behind: do not spiral
                t0, emu0 = time.monotonic(), s.r.time()
                target = s.r.time() + 0.05
        else:
            target = s.r.time() + 0.1
        if target > s.r.time():
            s.run(target - s.r.time())
        draw(stdscr, s)
        time.sleep(0.02)


# ---------------------------------------------------------------- headless

def run_script(s, script):
    for tok in [t for t in script.split(";") if t.strip()]:
        tok = tok.strip()
        if tok.startswith("wait:"):
            s.run(float(tok[5:]))
        elif tok.startswith("type:"):
            s.type(tok[5:])
            s.run(0.5)
        elif tok.startswith("rf:"):
            s.send_rf(tok[3:])
            s.run(0.05)
        elif tok.startswith("kiss:"):
            s.send_kiss(tok[5:])
            s.run(0.05)
        elif tok.startswith("/"):
            s.command(tok)
        else:
            sys.exit("bad script token: %s" % tok)
    while s.rfq or s.on_air:
        s.run(0.1)
    print("---- serial port 1")
    print("\n".join(s.console[-40:]))
    print("---- radio")
    print("\n".join(s.state_lines()))
    print("---- AX.25")
    print("\n".join(s.log_lines(200)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--flash", help="flash image file: loaded if it exists, saved on quit")
    ap.add_argument("--preset", choices=sorted(PRESETS), help="configure the radio first")
    ap.add_argument("--rssi", type=int, default=600, help="RSSI ADC value (600)")
    ap.add_argument("--audio", type=int, default=300, help="RX audio peak in ADC counts (300)")
    ap.add_argument("--noise", type=int, default=150, help="RX noise peak without signal (150)")
    ap.add_argument("--busy", action="store_true", help="BUSY line on")
    ap.add_argument("--dtr", action="store_true",
                    help="DTR on port 1 from the start: HaMDR's command mode (echo, prompt);"
                         " the radio does not operate (no beacons, digipeating, KISS)")
    ap.add_argument("--script", help="headless: ';'-separated tokens, print the result")
    a = ap.parse_args()
    sess = Session(a)
    sess.r.set_rssi(a.rssi)
    sess.r.set_levels(a.audio, a.noise)
    sess.r.set_busy(a.busy)
    if a.script is not None:
        run_script(sess, a.script)
        if a.flash:
            sess.save()
    else:
        def _quit(signum, frame):
            raise SystemExit(0)
        signal.signal(signal.SIGHUP, _quit)     # closed terminal: still save
        signal.signal(signal.SIGTERM, _quit)
        try:
            curses.wrapper(main, sess)
        except curses.error:
            pass                                # terminal already gone
        finally:
            if a.flash:
                sess.save()
