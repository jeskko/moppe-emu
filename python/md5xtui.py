#!/usr/bin/env python3
"""
Interactive terminal front end for the Talkman MD50/MD59/ME59 emulator.

    python3 emu/python/md5xtui.py [--fw mx5x-md59] [--ram radio.ram]
    python3 emu/python/md5xtui.py --rom x.bin --lst x.lst --model ME59 --cu cu59

--fw builds one of tests/md5x/roms.py's images (mx5x-md50, mx5x-md59,
mx5x-me59, oh1e-md50, oh1e-md59, oh1e-me59) from the firmware repo's
reference/md5x sources.

Keys (handset):  0-9 * #   c=CL  s=CS(STO)  r=RCL  e/Enter=E  + -
                 p=P  b=B  d=D  x=X  (the keypad's extra keys; which do
                 what depends on the firmware and its PTT keymap)
Hold time:       Tab cycles the hold time used for the NEXT key press
                 (0.15 s / 0.7 s / 1.5 s / 3 s).
Radio:           Space  toggle PTT        g  toggle received signal
                 o      power switch      h  toggle handset hook
                 F2     speed x1/x10      q  quit (RAM saved if --ram)

NMT network (--nmt, the original MD59 firmware with osmocom-analog's
base station, real time):
                 F5  the network calls the phone (the last registered number)
                 F6  the network hangs up
    python3 emu/python/md5xtui.py --rom reference/oh5nxo/mods/Mx5x/orig/MD59SB.raw.orig \
        --model MD59 --cu CU59 --eeprom 8123,456F,1234,0,111A,26FF,FFFF,...,14DC \
        --nmt ~/osmo/osmocom-analog/src/nmt/nmt --radio-out pipewire --radio-in pipewire
    (dial: digits, then h for off-hook; h again hangs up.)  Sound devices
    are ALSA names ("pipewire:NODE=<node>" for one PipeWire node):
    --radio-out/--radio-in the handset, --net-audio the network's end
    (osmocom's call audio), --hold-music FILE plays FILE into calls
    instead of the network's microphone, --rings N rings of ringback
    before a call from the phone is answered (default 3); --nmt-opts for osmocom (default
    "-k 1 -Y DK,1").  Without --ram the phone boots cold, is switched off
    and boots warm (the original firmware's cold start garbles dialling).

Firmware notes: mx5x takes 5 digits with the 100 MHz digit implied
("33500" = 433.500).  OH1E: digits then #; on a CU59 the first key after
a cold start sets the handset type.
"""
import argparse
import collections
import curses
import os
import shlex
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tests", "md5x"))
from md5xemu import Radio  # noqa: E402
import nmtlink  # noqa: E402

HOLDS = [0.15, 0.7, 1.5, 3.0]
KEYMAP = {ord(c): c for c in "0123456789*#+-"}
KEYMAP.update({ord("c"): "C", ord("s"): "S", ord("r"): "R", ord("e"): "E",
               10: "E", 13: "E", curses.KEY_ENTER: "E", ord("p"): "P",
               ord("b"): "B", ord("d"): "D", ord("x"): "X"})


# 7-segment font bits: 0 f, 1 e, 2 c, 3 b, 4 a, 5 g, 6 d
def seg7(g):
    a, b, c, d = g >> 4 & 1, g >> 3 & 1, g >> 2 & 1, g >> 6 & 1
    e, f, gg = g >> 1 & 1, g & 1, g >> 5 & 1
    return [" " + ("_" if a else " ") + " ",
            ("|" if f else " ") + ("_" if gg else " ") + ("|" if b else " "),
            ("|" if e else " ") + ("_" if d else " ") + ("|" if c else " ")]


class State:
    def __init__(self):
        self.hold_i = 0
        self.ptt = False
        self.signal = False
        self.offhook = False
        self.power = True
        self.speed = 1
        self.msg = ""
        self.tone = 0.0


def render(r, st, args):
    out = []
    out.append("Talkman emulator  %s / %s  %s   t=%.1f s  x%d" % (
        r.model, r.cu, args.name, r.time(), st.speed))
    out.append("")
    up, lo = r.glyphs()
    rows = [""] * 6
    for g in [None] * 4 + up:
        art = seg7(g) if g is not None else ["   "] * 3
        for k in range(3):
            rows[k] += art[k] + " "
    for g in lo:
        art = seg7(g)
        for k in range(3):
            rows[3 + k] += art[k] + " "
    out += ["  " + x for x in rows]
    out.append("  icons: " + " ".join(sorted(r.icons())))
    out.append("")
    tx = r.tx()
    l0, l1, l3 = r.latch(0), r.latch(1), r.latch(3)
    out.append("  RX %s MHz   VCO %.4f MHz   %s" % (
        "-" if tx else "%.4f" % (r.rx_hz() / 1e6), r.vco_hz() / 1e6,
        "** TRANSMITTING  power %d **" % ((r.latch(2) & 1) << 1 | (l1 & 1))
        if tx else "receiving"))
    out.append("  earpiece %s  speaker %s  vol bits %d  tone %s" % (
        "on " if l1 & 2 else "off", "on " if l3 & 8 else "off", l0 >> 1,
        ("%.0f Hz" % st.tone) if st.tone else "-"))
    out.append("  LEDs: %s" % " ".join(r.led_names()))
    out.append("  PTT %s  signal %s  hook %s  power switch %s%s" % (
        "DOWN" if st.ptt else "up", "yes" if st.signal else "no",
        "off-hook" if st.offhook else "on-hook",
        "on" if st.power else "off", "" if r.powered() else " (radio off)"))
    out.append("")
    out.append("  next key hold: %.2f s (Tab)   %s" % (HOLDS[st.hold_i], st.msg))
    out.append("  keys: 0-9*# c=CL s=CS r=RCL e=E p b d x +/-  "
               "Space=PTT g=signal h=hook o=power q=quit")
    net = getattr(st, "net", None)
    if net:
        out.append("")
        out.append("  NMT network: osmocom %s   F5 call the phone  F6 hang up" %
                   " ".join(net.opts))
        out.append("  link:")
        out += ["    " + x for x in list(net.log)[-6:]]
        out.append("  osmocom:")
        out += ["    " + x for x in net.osmo.lines(10)]
    return out


def set_signal(r, on):
    r.set_squelch(on)               # MD5x: modem GPIN2 (SQ)
    if r.model == "ME59":
        r.set_adc(1, 200 if on else 20)     # RSSI, mx5x squelch 70/80


def handle(r, st, ch):
    """Apply one input character; returns a key to press or None."""
    if ch == 9:
        st.hold_i = (st.hold_i + 1) % len(HOLDS)
    elif ch == ord(" "):
        st.ptt = not st.ptt
        r.set_ptt(st.ptt)
    elif ch == ord("g"):
        st.signal = not st.signal
        set_signal(r, st.signal)
    elif ch == ord("h"):
        st.offhook = not st.offhook
        r.set_hook(st.offhook)
    elif ch == ord("o"):
        st.power = not st.power
        r.set_input("poweroff", not st.power)   # firmware shuts itself down
        if st.power and not r.powered():
            r.power(True)
    elif ch in KEYMAP:
        return KEYMAP[ch]
    return None


class Net:
    """the radio linked to osmocom-analog's NMT (nmtlink.Session)"""

    def __init__(self, r, args):
        self.opts = shlex.split(args.nmt_opts)
        self.log = collections.deque(maxlen=50)
        self.file = open(args.nmt_log, "w") if args.nmt_log else None

        def log(line):
            self.log.append(line)
            if self.file:
                self.file.write(line + "\n")
                self.file.flush()

        def osmo_line(line):
            if self.file:
                self.file.write("%8.3f osmo %s\n" % (r.time(), line))
                self.file.flush()
        if not args.ram or not os.path.exists(args.ram):
            nmtlink.warm_boot(r)
        else:
            r.power(False)
        ear, mic, net, calldev = nmtlink.open_audio(args)
        self.osmo = nmtlink.Osmocom(args.nmt, self.opts, calldev, on_line=osmo_line)
        if net:
            self.osmo.listeners.append(net.on_line)
        self.session = nmtlink.Session(
            r, self.osmo, args.channel, ear=ear, mic=mic, hold=net, log=log, ear_gain=args.ear_gain, mic_gain=args.mic_gain,
            compandor=not args.no_compandor)
        self.session.start()

    def close(self):
        self.session.close()
        if self.file:
            self.file.close()


def open_radio(args):
    if args.rom:
        rom, lst, model, cu = args.rom, args.lst, args.model, args.cu
        args.name = os.path.basename(rom)
    else:
        import roms
        try:
            rom, lst, model, cu = roms.build(args.fw)
        except roms.Unavailable as e:
            sys.exit("cannot build %s: %s" % (args.fw, e))
        model = args.model or model
        cu = args.cu or cu
        args.name = args.fw
    ram = None
    if args.ram and os.path.exists(args.ram):
        with open(args.ram, "rb") as f:
            ram = f.read()
    r = Radio(rom, lst, model=model or "MD59", cu=cu or "CU59", ram=ram,
              ram_size=args.ram_size)
    if args.eeprom:
        r.eeprom([int(w, 16) for w in args.eeprom.split(",")])
    if ram is None and "tx_start" in r.syms and "TX_START" in r.syms:
        # OH1E with zeroed RAM refuses all TX: use its band defaults
        r.write("tx_start", r.syms["TX_START"].to_bytes(2, "little"))
        r.write("tx_end", r.syms["TX_END"].to_bytes(2, "little"))
    r.audio_capture(True)
    return r


def save_ram(r, args):
    if args.ram:
        with open(args.ram, "wb") as f:
            f.write(r.ram())


def measure_tone(r, st):
    """PHI edge frequency since the last call, then restart capture."""
    st.tone = r.tone_hz()
    r.audio_capture(True)


def run_script(args):
    """Headless: run the key script (same key letters as the TUI; '.'
    waits 0.5 s) and print the final screen."""
    r = open_radio(args)
    st = State()
    r.run(2.5)
    for c in args.script:
        if c == ".":
            r.run(0.5)
            continue
        k = handle(r, st, ord(c) if c != "\n" else 10)
        if k:
            r.press(k, hold=HOLDS[st.hold_i])
            st.hold_i = 0
        else:
            r.run(0.05)
    r.run(0.3)
    st.tone = r.tone_hz()           # over the whole run
    print("\n".join(render(r, st, args)))
    save_ram(r, args)


def main(stdscr, args):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.keypad(True)
    stdscr.addstr(0, 0, "starting...")
    stdscr.refresh()
    r = open_radio(args)
    st = State()
    st.net = Net(r, args) if args.nmt else None
    try:
        loop(stdscr, r, st, args)
    finally:
        if st.net:
            st.net.close()


def loop(stdscr, r, st, args):
    pending = None          # (key, release time)
    t0, emu0 = time.monotonic(), r.time()
    last_tone = 0.0

    def release(r):
        nonlocal pending
        if pending and r.time() >= pending[1]:
            r.key(None)
            pending = None

    while True:
        ch = stdscr.getch()
        while ch != -1:
            if ch == ord("q"):
                save_ram(r, args)
                return
            if ch == curses.KEY_F2:
                if st.net:
                    st.msg = "real time only with the network"
                else:
                    st.speed = 10 if st.speed == 1 else 1
                    t0, emu0 = time.monotonic(), r.time()
            elif ch == curses.KEY_F5 and st.net:
                st.net.osmo.console("d")
                st.msg = "network: dial"
            elif ch == curses.KEY_F6 and st.net:
                st.net.osmo.console("h")
                st.msg = "network: hang up"
            elif pending is None:
                k = handle(r, st, ch)
                if k:
                    r.key(k)
                    pending = (k, r.time() + HOLDS[st.hold_i])
                    st.msg = "pressed %s for %.2f s" % (k, HOLDS[st.hold_i])
                    st.hold_i = 0
            ch = stdscr.getch()

        target = emu0 + (time.monotonic() - t0) * st.speed
        if st.net:
            if target - r.time() > 0.5:         # fell behind: catch up later
                t0, emu0 = time.monotonic(), r.time()
                target = emu0
            try:
                while target - r.time() >= nmtlink.BLOCK / nmtlink.RATE:
                    st.net.session.step(release)
            except EOFError:
                save_ram(r, args)
                raise SystemExit("osmocom stopped:\n" + "\n".join(st.net.osmo.lines(20)))
        while not st.net and target - r.time() > 1e-6:
            step = min(target - r.time(), 0.02)
            if pending and r.time() + step >= pending[1]:
                r.run(max(0.0, pending[1] - r.time()))
                r.key(None)
                pending = None
                continue
            r.run(step)
        r.events.clear()            # nobody reads them here

        if r.time() - last_tone >= 0.2:
            measure_tone(r, st)
            last_tone = r.time()

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        for i, line in enumerate(render(r, st, args)[:h - 1]):
            stdscr.addstr(i, 0, line[:w - 1])
        stdscr.refresh()
        time.sleep(0.02)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fw", default="mx5x-md59",
                    help="firmware from tests/md5x/roms.py (default mx5x-md59)")
    ap.add_argument("--rom", help="ROM image instead of --fw")
    ap.add_argument("--lst", help="as06 listing for --rom (symbols, font)")
    ap.add_argument("--model", type=str.upper, choices=["MD50", "MD59", "ME59"])
    ap.add_argument("--cu", type=str.upper, choices=["CU53", "CU59"])
    ap.add_argument("--ram", help="battery RAM image to load/save")
    ap.add_argument("--ram-size", type=int, default=0,
                    help="RAM bytes (default 1024 MD50, 2048 MD59/ME59)")
    ap.add_argument("--script", help="headless: key script, print screen")
    ap.add_argument("--eeprom", help="93C06 contents: 16 hex words, comma separated")
    ap.add_argument("--nmt", metavar="BINARY",
                    help="link to osmocom-analog's NMT base station (its nmt binary)")
    ap.add_argument("--nmt-opts", default=" ".join(nmtlink.DEFAULT_NMT_OPTS),
                    help="osmocom's options (default %(default)s)")
    ap.add_argument("--channel", type=int, default=1, help="osmocom's channel")
    ap.add_argument("--nmt-log", metavar="FILE", help="link and osmocom log lines to FILE")
    nmtlink.add_audio_args(ap)
    a = ap.parse_args()
    if a.script is not None:
        run_script(a)
    else:
        curses.wrapper(main, a)
