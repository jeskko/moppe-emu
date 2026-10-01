#!/usr/bin/env python3
"""
Interactive terminal front end for the MC25 TVL/PTL emulator.

    python3 emu/python/mc25tui.py [--fw mc25-25k] [--nv radio.nv]
    python3 emu/python/mc25tui.py --rom x.bin --lst x.lst --step 25

--fw builds one of tests/mc25/roms.py's images (mc25-25k, mc25-12k)
from the firmware repo's reference/mc25ptl sources.  Without --nv (or
with a missing file) the radio first goes through the README's
first-time setup (bASEF 14540045, TX 145.400-146.000, 145.500).

Keys (CU41):     0-9 * #   + -   a=A  t=T  n=N  c=C  u=UP  d=DOWN
Hold time:       Tab cycles the hold time used for the NEXT key press
                 (0.15 s / 0.7 s / 1.5 s / 3 s); long presses matter
                 (e.g. a long 0).
Radio:           Space  toggle PTT        g  toggle received signal
                 h      toggle hook       l  lock switch (enters setup)
                 [ ]    handset UP/DOWN buttons (PROGRAM/OFFICE inputs)
                 o      power             F2 speed x1/x10   q  quit

Firmware notes (mc25.asm README): frequencies as 6 digits + #, or short
"512#" (base's first and last two digits); 92# enters setup, + - walk
it, T to its start, # leaves; NNNN* sends a CCIR sequence.
"""
import argparse
import curses
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tests", "mc25"))
from mc25emu import Radio, first_time_setup  # noqa: E402

HOLDS = [0.15, 0.7, 1.5, 3.0]
KEYMAP = {ord(c): c for c in "0123456789*#+-"}
KEYMAP.update({ord("a"): "A", ord("t"): "T", ord("n"): "N", ord("c"): "C",
               ord("u"): "U", ord("d"): "D", 10: "#", 13: "#",
               curses.KEY_ENTER: "#"})
BUTTONS = {ord("l"): "lock", ord("["): "program", ord("]"): "office"}


class State:
    def __init__(self):
        self.hold_i = 0
        self.ptt = False
        self.signal = False
        self.offhook = False
        self.power = True
        self.speed = 1
        self.msg = ""


def render(r, st, args):
    out = []
    out.append("MC25 TVL/PTL emulator  %s   t=%.1f s  x%d" % (
        args.name, r.time(), st.speed))
    out.append("")
    text = r.display()
    out.append("  +" + "-" * 18 + "+")
    out.append("  | %-16s |" % text)
    out.append("  +" + "-" * 18 + "+")
    leds = r.leds()
    out.append("  LEDs: " + " ".join("[%s]" % n if n in leds else " %s " % n.lower()
                                     for n in ("ON", "BUSY", "REQE", "REQA",
                                               "AUT", "MAN", "E", "A")))
    out.append("")
    tx = r.tx()
    o3, o5, o7 = r.out(3), r.out(5), r.out(7)
    out.append("  RX %.4f MHz   divisor %s   %s" % (
        r.rx_hz() / 1e6, r.divisor(),
        "** TRANSMITTING %s **" % ("hi" if o7 & 0x10 else "lo") if tx else "receiving"))
    ccir = o5 >> 4
    out.append("  volume %d  earphone %s  squelch gate %s  CCIR out %s  mic %s" % (
        o3 & 15, "on " if o3 & 0x20 else "off", "on " if o3 & 0x40 else "off",
        "%X" % ccir if ccir != 0xF else "-", "on" if o5 & 0x08 else "off"))
    tones = [n for b, n in ((0x80, "2000 Hz"), (0x01, "3112 Hz")) if o7 & b]
    if o3 & 0x80:
        tones.append("beep")
    out.append("  tones: %s" % (" ".join(tones) or "-"))
    out.append("  PTT %s  signal %s  hook %s  power %s" % (
        "DOWN" if st.ptt else "up", "yes" if st.signal else "no",
        "lifted" if st.offhook else "on hook",
        "on" if r.powered() else "off"))
    out.append("")
    out.append("  next key hold: %.2f s (Tab)   %s" % (HOLDS[st.hold_i], st.msg))
    out.append("  keys: 0-9*#+- a t n c u d  Space=PTT g=signal h=hook "
               "l=lock [ ]=up/down o=power q=quit")
    return out


def handle(r, st, ch):
    """Apply one input character; returns a key to press or None."""
    if ch == 9:
        st.hold_i = (st.hold_i + 1) % len(HOLDS)
    elif ch == ord(" "):
        st.ptt = not st.ptt
        r.set_ptt(st.ptt)
    elif ch == ord("g"):
        st.signal = not st.signal
        r.set_squelch(st.signal)
    elif ch == ord("h"):
        st.offhook = not st.offhook
        r.set_hook(st.offhook)
    elif ch == ord("o"):
        st.power = not st.power
        r.power(st.power)
    elif ch in BUTTONS:
        return ("button", BUTTONS[ch])
    elif ch in KEYMAP:
        return ("key", KEYMAP[ch])
    return None


def open_radio(args):
    if args.rom:
        rom, lst, step = args.rom, args.lst, args.step
        args.name = os.path.basename(rom)
    else:
        import roms
        try:
            rom, lst, step = roms.build(args.fw)
        except roms.Unavailable as e:
            sys.exit("cannot build %s: %s" % (args.fw, e))
        args.name = args.fw
    nv = None
    if args.nv and os.path.exists(args.nv):
        with open(args.nv, "rb") as f:
            nv = f.read()
    if nv is None:
        print("first-time setup (about a second)...", file=sys.stderr)
        nv = first_time_setup(Radio(rom, lst, step_khz=step))
    return Radio(rom, lst, nv=nv, step_khz=step)


def save_nv(r, args):
    if args.nv:
        with open(args.nv, "wb") as f:
            f.write(r.nv())


def run_script(args):
    """Headless: run the key script (same letters as the TUI; '.' waits
    0.5 s) and print the final screen."""
    r = open_radio(args)
    st = State()
    r.run(2.0)
    for _ in range(80):             # banner and lamp test, then main screen
        if r.display().strip() and "." not in r.display():
            break
        r.run(0.1)
    for c in args.script:
        if c == ".":
            r.run(0.5)
            continue
        k = handle(r, st, ord(c) if c != "\n" else 10)
        if k and k[0] == "key":
            r.press(k[1], hold=HOLDS[st.hold_i])
            st.hold_i = 0
        elif k:
            r.button(k[1])
        else:
            r.run(0.05)
    r.run(0.6)
    print("\n".join(render(r, st, args)))
    save_nv(r, args)


def main(stdscr, args, r):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.keypad(True)
    st = State()
    pending = None          # (kind, name, release time)
    t0, emu0 = time.monotonic(), r.time()

    while True:
        ch = stdscr.getch()
        while ch != -1:
            if ch == ord("q"):
                save_nv(r, args)
                return
            if ch == curses.KEY_F2:
                st.speed = 10 if st.speed == 1 else 1
                t0, emu0 = time.monotonic(), r.time()
            elif pending is None:
                k = handle(r, st, ch)
                if k:
                    hold = HOLDS[st.hold_i] if k[0] == "key" else max(HOLDS[st.hold_i], 0.3)
                    if k[0] == "key":
                        r.key(k[1])
                    else:
                        r.set_input(k[1], True)
                    pending = (k[0], k[1], r.time() + hold)
                    st.msg = "pressed %s for %.2f s" % (k[1], hold)
                    st.hold_i = 0
            ch = stdscr.getch()

        target = emu0 + (time.monotonic() - t0) * st.speed
        while target - r.time() > 1e-6:
            step = min(target - r.time(), 0.02)
            if pending and r.time() + step >= pending[2]:
                r.run(max(0.0, pending[2] - r.time()))
                if pending[0] == "key":
                    r.key(None)
                else:
                    r.set_input(pending[1], False)
                pending = None
                continue
            r.run(step)
        r.events.clear()

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        for i, line in enumerate(render(r, st, args)[:h - 1]):
            stdscr.addstr(i, 0, line[:w - 1])
        stdscr.refresh()
        time.sleep(0.02)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fw", default="mc25-25k",
                    help="firmware from tests/mc25/roms.py (default mc25-25k)")
    ap.add_argument("--rom", help="ROM image instead of --fw")
    ap.add_argument("--lst", help="as06 listing for --rom (symbols, tables)")
    ap.add_argument("--step", type=float, default=25,
                    help="the --rom build's STEP_kHz (25, 20, 12.5, 10)")
    ap.add_argument("--nv", help="RAM+NOVRAM image to load/save")
    ap.add_argument("--script", help="headless: key script, print screen")
    a = ap.parse_args()
    if a.script is not None:
        run_script(a)
    else:
        radio = open_radio(a)
        curses.wrapper(main, a, radio)
