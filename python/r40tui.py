#!/usr/bin/env python3
"""
Interactive terminal front end for the Nokia R40 (RC40 / RD40) emulator.

    python3 emu/python/r40tui.py [--rom ABSBIN] [--service] [--pixels]
                                 [--nv radio.nv [--save-nv]]
    python3 emu/python/r40tui.py --service --script "18164000 OK wait:1.5"

Keys (CU43):   0-9 * #   Enter=OK  Backspace=CLR  f=FNC  r=RCL/STO
               Up / Down arrows = UP / DOWN
Radio:         p  toggle PTT          o  hold / release PWR (toggle)
               O  tap PWR for 1 s     h  toggle hook (off-hook)
               s  restart into service mode (power off, hold PWR, on)
               v  toggle text / pixel view     q  quit
Each key is held for 0.15 s of emulated time, then released.

--script runs headless, no curses.  Space-separated tokens:
    wait:S          run S seconds        digits / * #  typed one key at a time
    OK CLR FNC RCL STO UP DOWN   a key press (OK, FNC, UP, DOWN, RCL, STO
                    are followed by --ok-wait seconds, default 1.5)
    key:NAME        press NAME (any KEYS name)   ptt:0|1   hook:0|1
    pwr:0|1         release / hold PWR           power:0|1 supply off / on
    svc             restart into service mode    hold:S  set key hold time
The screen is printed afterwards, with the synthesizers.
"""
import argparse
import curses
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tests", "r40"))
from r40emu import Radio, KEYS  # noqa: E402

HOLD = 0.15
KEYMAP = {ord(c): c for c in "0123456789*#"}
KEYMAP.update({10: "OK", 13: "OK", curses.KEY_ENTER: "OK",
               curses.KEY_BACKSPACE: "CLR", 127: "CLR", 8: "CLR",
               ord("f"): "FNC", ord("r"): "RCL",
               curses.KEY_UP: "UP", curses.KEY_DOWN: "DOWN"})


class State:
    def __init__(self, args):
        self.ptt = False
        self.hook = False
        self.pwr = False
        self.service = args.service
        self.pixels = args.pixels
        self.msg = ""


def open_radio(args, st):
    import roms
    rom = args.rom
    if not rom:
        try:
            rom = roms.rom()
        except roms.Unavailable as e:
            sys.exit(str(e))
    nv = None
    if args.make_nv:
        args.nv = args.nv or "r40.nv"
        if not os.path.exists(args.nv):
            import r40nv
            print("building %s with r40nv.py (service-mode set-up, ~15 s)" % args.nv)
            with open(args.nv, "wb") as f:
                f.write(r40nv.default_nv(rom))
    if args.nv and os.path.exists(args.nv):
        with open(args.nv, "rb") as f:
            nv = f.read()
    args.name = os.path.basename(rom)
    if args.service:
        r = Radio(rom, nv=nv, service_head=True, power=False)
        r.service_mode()
    else:
        r = Radio(rom, nv=nv)
    return r


def restart_service(r, st):
    r.ptt(False)
    st.ptt = False
    r.L.r40api_set_service_head(r.m, 1)
    st.service = True
    r.service_mode()
    st.pwr = False


def mhz(r, which):
    try:
        return r.pll(which)[-1] / 1e6
    except Exception:
        return 0.0


def render_text(r, st, args):
    out = ["Nokia R40 emulator  %s" % args.name, ""]
    if st.pixels:
        px = r.pixels()
        for y in range(0, 24, 2):
            s = ""
            for x in range(120):
                a, b = px[y][x] == "#", px[y + 1][x] == "#"
                s += "█" if a and b else "▀" if a else "▄" if b else " "
            out.append(" " + s)
    else:
        d = r.display()
        out.append("  +" + "-" * 26 + "+")
        for w, line in zip((20, 24, 24), d):
            out.append("  | %-24s |" % line[:24])
        out.append("  +" + "-" * 26 + "+")
    out.append("")
    out.append("  t=%.2f s  RX %.4f MHz  TX %.4f MHz  %s  PTT %s  hook %s  PWR %s  head %s" % (
        r.time(), mhz(r, 0), mhz(r, 1),
        "** TX ON **" if r.out(1) & 1 else "tx off",
        "DOWN" if st.ptt else "up", "off-hook" if st.hook else "on-hook",
        "held" if st.pwr else "-", "service" if st.service else "CU43"))
    out.append("  " + st.msg)
    out.append("  keys: 0-9*# Enter=OK Bksp=CLR f=FNC r=RCL/STO Up/Down  "
               "p=PTT o=PWR O=tap PWR h=hook s=service v=view q=quit")
    return out


def run_script(args):
    st = State(args)
    r = open_radio(args, st)
    hold = HOLD
    r.run(args.boot)
    slow = {"OK", "FNC", "RCL", "STO", "UP", "DOWN"}
    for tok in args.script.split():
        name, _, val = tok.partition(":")
        if name == "wait":
            r.run(float(val))
        elif name == "ptt":
            st.ptt = val == "1"; r.ptt(st.ptt)
        elif name == "hook":
            st.hook = val == "1"; r.hook(st.hook)
        elif name == "pwr":
            st.pwr = val == "1"; r.power_key(st.pwr)
        elif name == "power":
            r.power(val == "1")
        elif name == "hold":
            hold = float(val)
        elif name == "svc":
            restart_service(r, st)
        elif name == "key":
            r.press(val.upper() if val.upper() in KEYS else val, hold=hold)
        elif tok.upper() in KEYS:
            r.press(tok.upper(), hold=hold)
            if tok.upper() in slow:
                r.run(args.ok_wait)
        elif all(c in "0123456789*#" for c in tok):
            for c in tok:
                r.press(c, hold=hold)
        else:
            sys.exit("bad script token: %s" % tok)
    print("\n".join(render_text(r, st, args)))
    if args.save_nv and args.nv:
        with open(args.nv, "wb") as f:
            f.write(r.nv())


def main(stdscr, args, r, st):
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.keypad(True)
    pending = None          # (key name, release time)
    t0, emu0 = time.monotonic(), r.time()
    SLICE = 0.03

    while True:
        ch = stdscr.getch()
        while ch != -1:
            if ch == ord("q"):
                return
            elif ch == ord("p"):
                st.ptt = not st.ptt
                r.ptt(st.ptt)
            elif ch == ord("h"):
                st.hook = not st.hook
                r.hook(st.hook)
            elif ch == ord("v"):
                st.pixels = not st.pixels
            elif ch == ord("o"):
                st.pwr = not st.pwr
                r.power_key(st.pwr)
            elif ch == ord("O"):
                st.pwr = True
                r.power_key(True)
                pending = ("PWR", r.time() + 1.0)
            elif ch == ord("s"):
                st.msg = "restarting into service mode..."
                restart_service(r, st)
                t0, emu0 = time.monotonic(), r.time()
            elif ch in KEYMAP and pending is None:
                k = KEYMAP[ch]
                r.key(k, True)
                pending = (k, r.time() + HOLD)
                st.msg = "pressed %s" % k
            ch = stdscr.getch()

        target = emu0 + (time.monotonic() - t0)
        if target - r.time() > 0.5:         # fell behind: do not spiral
            t0, emu0 = time.monotonic(), r.time()
            target = r.time()
        while target - r.time() > 1e-6:
            step = min(target - r.time(), SLICE)
            if pending and r.time() + step >= pending[1]:
                r.run(max(0.0, pending[1] - r.time()))
                if pending[0] == "PWR":
                    r.power_key(False)
                    st.pwr = False
                else:
                    r.key(pending[0], False)
                pending = None
                continue
            r.run(step)
        r.events.clear()

        stdscr.erase()
        h, w = stdscr.getmaxyx()
        for i, line in enumerate(render_text(r, st, args)[:h - 1]):
            try:
                stdscr.addstr(i, 0, line[:w - 1])
            except curses.error:
                pass
        stdscr.refresh()
        time.sleep(0.02)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", help="R40 ROM image (default: tests/r40/roms.py)")
    ap.add_argument("--service", action="store_true",
                    help="CU43PROG service head, start in LOCAL mode")
    ap.add_argument("--pixels", action="store_true", help="pixel view of the LCD")
    ap.add_argument("--nv", help="NV image to load")
    ap.add_argument("--save-nv", action="store_true", help="save --nv back on quit")
    ap.add_argument("--make-nv", action="store_true",
                    help="build --nv (default r40.nv) with r40nv.py if it does not exist:"
                         " a set-up radio instead of Error 6")
    ap.add_argument("--script", help="headless: token script, print screen")
    ap.add_argument("--ok-wait", type=float, default=1.5,
                    help="script: seconds to run after OK/FNC/UP/DOWN (1.5)")
    ap.add_argument("--boot", type=float, default=0.0,
                    help="script: seconds to run before the script (0)")
    a = ap.parse_args()
    if a.script is not None:
        run_script(a)
    else:
        s = State(a)
        radio = open_radio(a, s)
        try:
            curses.wrapper(main, a, radio, s)
        finally:
            if a.save_nv and a.nv:
                with open(a.nv, "wb") as f:
                    f.write(radio.nv())
