#!/usr/bin/env python3
"""
Interactive terminal front end for the TMF-1 / TMN-1 emulator (radio unit
plus an HSN-2 or HSF-2 handset, each with its own firmware).

    python3 emu/python/tmx1tui.py [--fw tmf1] [--hs hsn2] [--ram radio.ram]
    python3 emu/python/tmx1tui.py --rom x.bin --lst x.lst --hs-rom hs.bin

--fw builds tmf1 or tmn1 (radio) and --hs hsn2 or hsf2 (handset) with
tests/tmx1/roms.py; --hs none leaves the handset out and the keys are then
sent as MBUS packets (M<key> on press, B<key> on release) from "ANY1".

Keys (handset):  0-9 * # + -   Enter=#   m=MUTE  o=OK  n=NEXT  a=R/ALPHA
                 ?=MENU  r=RCL  f=FCN  c=CLR  e=handset key  k=PTT key
                 h=HF
Hold time:       Tab cycles the hold time used for the NEXT key press
                 (0.15 s / 0.7 s / 1.5 s / 3 s).
Radio:           Space  toggle external PTT   g  toggle received signal
                 l      toggle hook           x  handset power key
                 F2     speed x1/x10          q  quit (RAM saved if --ram)

Firmware notes: the radio takes 6 digits in kHz, then # (433550# =
433.550 MHz).  The firmware shows its version for about 5.5 s after power
on.  x switches the unit on when it is off and the firmware off when on.
"""
import argparse
import curses
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tests", "tmx1"))
import tmx1emu  # noqa: E402
from tmx1emu import Radio  # noqa: E402

HOLDS = [0.15, 0.7, 1.5, 3.0]
KEYMAP = {ord(c): c for c in "0123456789*#+-"}
KEYMAP.update({ord("m"): "M", ord("o"): "O", ord("n"): "N", ord("a"): "A",
               ord("?"): "?", ord("r"): "R", ord("f"): "F", ord("c"): "C",
               ord("e"): "E", ord("k"): "P", ord("h"): "H",
               10: "#", 13: "#", curses.KEY_ENTER: "#"})
HELP = ("0-9*#+- m=MUTE o=OK n=NEXT a=ALPHA ?=MENU r=RCL f=FCN c=CLR "
        "e=E k=PTT h=HF")


class State:
    def __init__(self):
        self.hold_i = 0
        self.ptt = False
        self.signal = False
        self.offhook = False
        self.speed = 1
        self.msg = ""
        self.tone = 0.0
        self.packets = []           # last 3 MBUS packets, as strings
        self.mbus = []              # bytes of a packet still arriving


def pkt_str(p):
    dest, src, payload, _ = p
    pl = "".join(chr(b) if 32 <= b < 127 else "\\x%02X" % b for b in payload)
    return "%s %s %s" % (dest, src, pl)


def collect_mbus(r, st):
    """Fold the MBUS bytes seen since the last call into st.packets."""
    b = st.mbus + r.mbus_bytes()
    end = max((i for i, x in enumerate(b) if x[1] == 4), default=-1)
    for p in r.mbus_packets(b[:end + 1]):
        st.packets = (st.packets + [pkt_str(p)])[-3:]
    st.mbus = b[end + 1:][-64:]
    r.events.clear()                # the harness accumulates them


def render(r, st, args):
    out = []
    out.append("TMx-1 emulator  %s / %s  t=%.1f s  x%d" % (
        args.name, r.handset or "no handset", r.time(), st.speed))
    out.append("")
    mid, bot, top = r.display()
    out.append("  +----------+")
    out.append("  |      %2s  |" % top)
    out.append("  | %s |" % mid)
    out.append("  | %s |" % bot)
    out.append("  +----------+")
    out.append("  icons: %s" % " ".join(sorted(r.icons())))
    out.append("  LEDs:  %s" % " ".join(sorted(r.leds())))
    out.append("")
    tx = r.tx()
    out.append("  RX VCO %.4f MHz   %s" % (
        r.vco_hz() / 1e6,
        "** TRANSMITTING **  TX %.4f MHz" % (r.tx_hz() / 1e6)
        if tx else "receiving"))
    d = r.dtmf()
    out.append("  audio %s  DTMF %s  tone %s" % (
        "open" if r.audio_open() else "closed",
        "0x%02X" % d if d else "-",
        ("%.0f Hz" % st.tone) if st.tone else "-"))
    out.append("  PTT %s  signal %s  hook %s  power %s" % (
        "DOWN" if st.ptt else "up", "yes" if st.signal else "no",
        "off-hook" if st.offhook else "on-hook",
        "on" if r.powered() else "off"))
    out.append("")
    out.append("  MBUS (dest src payload):")
    for i in range(3):
        out.append("    " + (st.packets[i] if i < len(st.packets) else ""))
    out.append("")
    out.append("  next key hold: %.2f s (Tab)   %s" % (HOLDS[st.hold_i], st.msg))
    out.append("  keys: " + HELP)
    out.append("        Space=PTT g=signal l=hook x=power key F2=speed q=quit")
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
        r.set_rssi(200 if st.signal else 0)
    elif ch == ord("l"):
        st.offhook = not st.offhook
        r.set_hook(st.offhook)
    elif ch == ord("x"):
        r.power_key(True)
        r.run(0.4)
        r.power_key(False)
        st.msg = "power key"
    elif ch in KEYMAP:
        return KEYMAP[ch]
    return None


def open_radio(args):
    import roms
    if args.rom:
        rom, lst = args.rom, args.lst
        args.name = os.path.basename(args.rom)
    else:
        try:
            rom, lst = roms.build(args.fw)
        except roms.Unavailable as e:
            sys.exit("cannot build %s: %s" % (args.fw, e))
        args.name = args.fw
    hs_rom, handset = None, None
    if args.hs != "none":
        handset = args.hs.upper()
        if args.hs_rom:
            hs_rom = args.hs_rom
        else:
            try:
                hs_rom = roms.build(args.hs)[0]
            except roms.Unavailable as e:
                sys.exit("cannot build %s: %s" % (args.hs, e))
    ram = None
    if args.ram and os.path.exists(args.ram):
        with open(args.ram, "rb") as f:
            ram = f.read()
    r = Radio(rom, lst, hs_rom=hs_rom, handset=handset or "HSN2", ram=ram)
    r.audio_capture(True)
    return r


def save_ram(r, args):
    if args.ram:
        with open(args.ram, "wb") as f:
            f.write(r.ram())


def measure_tone(r, st):
    """OUT2 edge frequency since the last call, then restart capture."""
    st.tone = r.tone_hz()
    r.audio_capture(True)


def run_script(args):
    """Headless: boot, run the key script (same key letters as the TUI;
    '.' waits 0.5 s) and print the final screen."""
    r = open_radio(args)
    st = State()
    r.run(6.0)                      # the firmware shows its version till ~5.5 s
    collect_mbus(r, st)
    for c in args.script:
        if c == ".":
            r.run(0.5)
            continue
        k = handle(r, st, ord(c) if c != "\n" else 10)
        if k:
            r.key(k)
            r.run(HOLDS[st.hold_i])
            r.key(k, False)
            r.run(0.15)
            st.hold_i = 0
        else:
            r.run(0.05)
        collect_mbus(r, st)
    r.run(0.3)
    collect_mbus(r, st)
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
    pending = None          # (key, release time)
    t0, emu0 = time.monotonic(), r.time()
    last_tone = 0.0

    while True:
        ch = stdscr.getch()
        while ch != -1:
            if ch == ord("q"):
                save_ram(r, args)
                return
            if ch == curses.KEY_F2:
                st.speed = 10 if st.speed == 1 else 1
                t0, emu0 = time.monotonic(), r.time()
            elif pending is None:
                k = handle(r, st, ch)
                if ch == ord("x"):
                    t0, emu0 = time.monotonic(), r.time()
                if k:
                    r.key(k)
                    pending = (k, r.time() + HOLDS[st.hold_i])
                    st.msg = "pressed %s for %.2f s" % (k, HOLDS[st.hold_i])
                    st.hold_i = 0
            ch = stdscr.getch()

        target = emu0 + (time.monotonic() - t0) * st.speed
        while target - r.time() > 1e-6:
            step = min(target - r.time(), 0.02)
            if pending and r.time() + step >= pending[1]:
                r.run(max(0.0, pending[1] - r.time()))
                r.key(pending[0], False)
                pending = None
                continue
            r.run(step)
        collect_mbus(r, st)

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
    ap.add_argument("--fw", default="tmf1", choices=["tmf1", "tmn1"],
                    help="radio firmware from tests/tmx1/roms.py (default tmf1)")
    ap.add_argument("--hs", default="hsn2", choices=["hsn2", "hsf2", "none"],
                    help="handset (default hsn2); none: keys go out as MBUS")
    ap.add_argument("--rom", help="radio ROM image instead of --fw")
    ap.add_argument("--lst", help="as7810 listing for --rom (symbols)")
    ap.add_argument("--hs-rom", help="handset ROM image instead of the built one")
    ap.add_argument("--ram", help="8 KB battery RAM image to load/save")
    ap.add_argument("--script", help="headless: key script, print screen")
    a = ap.parse_args()
    if a.script is not None:
        run_script(a)
    else:
        curses.wrapper(main, a)
