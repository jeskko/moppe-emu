#!/usr/bin/env python3
"""
Drive a curses program (md5xtui.py) in a pseudo-terminal: keys on a
timeline, the screen stream saved raw (for grep; no terminal emulation).

    tuidrive.py OUT 'T:KEYS,T:KEYS,...' -- python3 python/md5xtui.py ...

T is seconds from start; KEYS is literal text, or F1-F12 / q.  Ends when
the program exits (send q last).  For a log of an NMT session use the
TUI's --nmt-log rather than the screen.
"""
import fcntl
import os
import pty
import struct
import sys
import termios
import threading
import time

FKEYS = {"F1": "\x1bOP", "F2": "\x1bOQ", "F3": "\x1bOR", "F4": "\x1bOS",
         "F5": "\x1b[15~", "F6": "\x1b[17~", "F7": "\x1b[18~", "F8": "\x1b[19~",
         "F9": "\x1b[20~", "F10": "\x1b[21~", "F11": "\x1b[23~", "F12": "\x1b[24~"}

out_path, plan = sys.argv[1], sys.argv[2]
argv = sys.argv[sys.argv.index("--") + 1:]
steps = []
for item in filter(None, plan.split(",")):
    t, k = item.split(":", 1)
    steps.append((float(t), FKEYS.get(k, k)))
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execvp(argv[0], argv)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 60, 200, 0, 0))
out = open(out_path, "wb")


def drain():
    while True:
        try:
            b = os.read(fd, 65536)
        except OSError:
            break
        if not b:
            break
        out.write(b)
        out.flush()


threading.Thread(target=drain, daemon=True).start()
t0 = time.monotonic()
for t, k in sorted(steps):
    time.sleep(max(0.0, t0 + t - time.monotonic()))
    os.write(fd, k.encode())
os.waitpid(pid, 0)
time.sleep(0.3)
