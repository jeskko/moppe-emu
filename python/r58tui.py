#!/usr/bin/env python3
"""
Interactive terminal front end for the R58 emulator.

    python3 emu/python/r58tui.py [--cu cu58] [--card p8n] [--nv radio.nv]
    python3 python/r58tui.py --rom reference/r58/r58p8x3Z.bin.als --lst ''

The default ROM is the firmware repo's build (firmware/build/r58.bin and
its .map); in this repo alone, `make refs` fetches the published v3_Z ALs
binary, which runs without symbols (--lst ''), and is the default then.

Keys (handset):  0-9 * #   c=CL  s=STO  r=RCL  e/Enter=ENT  b=SHIFT(button)
                 + -  (side keys)
Hold time:       Tab cycles the hold time used for the NEXT key press
                 (0.15 s / 0.7 s / 1.5 s / 3 s); long presses reach the
                 firmware's "hold" functions (store, step, squelch...).
Radio:           Space  toggle PTT        g  toggle received signal (SQL)
                 p      power switch      h  toggle handset hook
                 F2     speed x1/x10      q  quit (NV saved if --nv)
"""
import argparse
import curses
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r58emu import Radio, P8E, P8N, CU53AN, CU58AF, AD_SQL, AD_RSSI  # noqa

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EMU = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ALS = os.path.join(EMU, "reference", "r58", "r58p8x3Z.bin.als")    # make refs

HOLDS = [0.15, 0.7, 1.5, 3.0]
KEYMAP = {ord(c): c for c in "0123456789*#+-"}
KEYMAP.update({ord("c"): "C", ord("s"): "S", ord("r"): "R", ord("e"): "E",
               10: "E", 13: "E", curses.KEY_ENTER: "E", ord("b"): "B"})

# 7-segment font bits: 0 f, 1 e, 2 c, 3 b, 4 a, 5 g, 6 d
def seg7(g):
    a, b, c, d = g >> 4 & 1, g >> 3 & 1, g >> 2 & 1, g >> 6 & 1
    e, f, gg = g >> 1 & 1, g & 1, g >> 5 & 1
    return [" " + ("_" if a else " ") + " ",
            ("|" if f else " ") + ("_" if gg else " ") + ("|" if b else " "),
            ("|" if e else " ") + ("_" if d else " ") + ("|" if c else " ")]


def fresh_nv(args):
    """Zeroed NV -> SAnE, like a first-time setup.  With symbols the
    zeroed hook scripts are blanked first (else the boot-time hook edge
    "types" eight 0 keys); SAnE resets them too, so a ROM without
    symbols (the published binary) also comes up right."""
    r = Radio(args.rom, args.lst, card=args.card, cu=args.cu)
    if "cfg_onhook_script" in r.sym:
        r.poke("cfg_onhook_script", b"\xff" * 8)
        r.poke("cfg_offhook_script", b"\xff" * 8)
    r.run(2.5)
    r.type("828")
    r.press("E")
    r.run(0.3)
    r.type("666")
    r.press("#")
    r.run(1.0)
    return r.nv()


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
    """The screen as a list of text lines."""
    out = []
    out.append("R58 emulator  %s / %s   t=%.1f s  x%d" % (
        "P8E" if args.card == P8E else "P8N",
        "CU53AN" if args.cu == CU53AN else "CU58AF", r.time, st.speed))
    out.append("")
    if args.cu == CU53AN:
        up, lo = r.glyphs()
        rows = [""] * 6
        for i, g in enumerate([None] * 4 + up):
            art = seg7(g) if g is not None else ["   "] * 3
            for k in range(3):
                rows[k] += art[k] + " "
        for g in lo:
            art = seg7(g)
            for k in range(3):
                rows[3 + k] += art[k] + " "
        out += ["  " + x for x in rows]
        ic = sorted(r.icons() - {"COLON_UL", "COLON_UR", "COLON_D"})
        out.append("  icons: " + " ".join(ic))
    else:
        up, lo = r.display()
        out += ["  +-----------+", "  | %-9s |" % up, "  | %-9s |" % lo,
                "  +-----------+"]
    out.append("")
    L = r.latches()
    ind = r.peek("indicators") if "indicators" in r.sym else None
    tx = r.transmitting()
    rx = r.rx_hz()
    txf = r.tx_hz()
    tone = r.tone_hz()
    out0 = L["out0"]
    out.append("  RX %s MHz   TX %s MHz   %s" % (
        "%.4f" % (rx / 1e6) if rx else "-",
        "%.4f" % (txf / 1e6) if txf else "-",
        "** TRANSMITTING  pwr DAC %d **" % L["da_txpwr"] if tx else "receiving"))
    out.append("  speaker %s vol %d  audio %s  tone %s%s  mic %s" % (
        "off" if out0 & 0x08 else "on ", out0 & 7,
        "OPEN  " if out0 & 0x10 else "closed",
        ("%.0f Hz" % tone) if tone and tone < 20000 else "-",
        " (local)" if out0 & 0x40 else (" (tx)" if out0 & 0x20 else ""),
        "muted" if out0 & 0x80 else "live"))
    # indicator bit layout differs between the handsets (r58.asm L776-792)
    bits = (((4, "SERV"), (1, "CALL"), (2, "TX"), (5, "ON"), (6, "LCDLIGHT"),
             (3, "KEYLIGHT")) if args.cu == CU53AN else
            ((0, "SERV"), (1, "CALL"), (3, "TX"), (2, "ON"), (6, "LCDLIGHT"),
             (5, "KEYLIGHT")))
    if ind is None:
        out.append("  LEDs: - (the firmware's indicators byte needs symbols)")
    else:
        out.append("  LEDs: %s" % " ".join(n for b, n in bits if ind >> b & 1))
    out.append("  PTT %s  signal %s  hook %s  power switch %s%s" % (
        "DOWN" if st.ptt else "up", "yes" if st.signal else "no",
        "off-hook" if st.offhook else "on-hook",
        "on" if st.power else "off", "" if r.powered else " (radio off)"))
    out.append("")
    out.append("  next key hold: %.2f s (Tab)   %s" % (HOLDS[st.hold_i], st.msg))
    out.append("  keys: 0-9*# c=CL s=STO r=RCL e=ENT b=SHIFT +/-  "
               "Space=PTT g=signal h=hook p=power q=quit")
    return out


def handle(r, st, ch):
    """Apply one input character; returns a key to press or None."""
    if ch == 9:
        st.hold_i = (st.hold_i + 1) % len(HOLDS)
    elif ch == ord(" "):
        st.ptt = not st.ptt
        r.ptt(st.ptt)
    elif ch == ord("g"):
        st.signal = not st.signal
        r.adc(AD_SQL, 0xE0 if st.signal else 0x00)
        r.adc(AD_RSSI, 0xC0 if st.signal else 0x20)
    elif ch == ord("h"):
        st.offhook = not st.offhook
        r.hook(st.offhook)
    elif ch == ord("p"):
        st.power = not st.power
        r.power(st.power)
    elif ch in KEYMAP:
        return KEYMAP[ch]
    return None


def open_radio(args):
    nv = None
    if args.nv and os.path.exists(args.nv):
        with open(args.nv, "rb") as f:
            nv = f.read()
    if nv is None:
        nv = fresh_nv(args)
    return Radio(args.rom, args.lst, card=args.card, cu=args.cu, nv=nv)


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
    r.run(0.5)
    print("\n".join(render(r, st, args)))
    if args.nv:
        r.save_nv(args.nv)


def main(stdscr, args):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.keypad(True)
    stdscr.addstr(0, 0, "starting...")
    stdscr.refresh()
    r = open_radio(args)
    st = State()
    pending = None          # (key, release time)
    t0, emu0 = time.monotonic(), r.time

    while True:
        ch = stdscr.getch()
        while ch != -1:
            if ch == ord("q"):
                if args.nv:
                    r.save_nv(args.nv)
                return
            if ch == curses.KEY_F2:
                st.speed = 10 if st.speed == 1 else 1
                t0, emu0 = time.monotonic(), r.time
            elif pending is None:
                k = handle(r, st, ch)
                if k:
                    r.key_down(k)
                    pending = (k, r.time + HOLDS[st.hold_i])
                    st.msg = "pressed %s for %.2f s" % (k, HOLDS[st.hold_i])
                    st.hold_i = 0
            ch = stdscr.getch()

        target = emu0 + (time.monotonic() - t0) * st.speed
        while target - r.time > 1e-6:
            step = min(target - r.time, 0.02)
            if pending and r.time + step >= pending[1]:
                r.run(max(0.0, pending[1] - r.time))
                r.key_up()
                pending = None
                continue
            r.run(step)

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        for i, line in enumerate(render(r, st, args)[:h - 1]):
            stdscr.addstr(i, 0, line[:w - 1])
        stdscr.refresh()
        time.sleep(0.02)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=os.path.join(ROOT, "firmware/build/r58.bin"))
    ap.add_argument("--lst", default=os.path.join(ROOT, "firmware/build/r58.map"),
                    help="symbols: sdldz80 .map or as80 listing ('' for none)")
    ap.add_argument("--cu", choices=["cu53", "cu58"], default="cu53")
    ap.add_argument("--card", choices=["p8e", "p8n"], default="p8e")
    ap.add_argument("--nv", help="NV (battery RAM) image to load/save")
    ap.add_argument("--script", help="headless: key script, print screen")
    a = ap.parse_args()
    if not os.path.exists(a.rom) and a.rom == ap.get_default("rom") and os.path.exists(ALS):
        a.rom, a.lst = ALS, ""      # no firmware repo around: the published binary
    a.cu = CU58AF if a.cu == "cu58" else CU53AN
    a.card = P8N if a.card == "p8n" else P8E
    if a.script is not None:
        run_script(a)
    else:
        curses.wrapper(main, a)
