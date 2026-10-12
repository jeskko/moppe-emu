#!/usr/bin/env python3
"""
Behaviour digest of the six ham firmware builds on libmd5x.so: RAM, CPU,
events, LCD and latches after a scripted session (keys, PTT, two power
cycles).  Run it on the old and the new library after a board change;
the ALL lines must match unless the change is meant to alter behaviour.

    python3 tools/md5x/digest.py                       # this tree's library
    MD5X_LIB=/path/to/old/libmd5x.so python3 tools/md5x/digest.py

(The old library must have every API function the harness loads; build
it from the same harness revision, e.g. git worktree of the old commit.)
The TX slot stream is not included.  Needs the reference sources
(tests/md5x/roms.py).
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tests", "md5x"))

from md5xemu import Radio  # noqa: E402
import roms  # noqa: E402

FW = ["mx5x-md50", "mx5x-md59", "mx5x-me59", "oh1e-md50", "oh1e-md59", "oh1e-me59"]

h = hashlib.sha256()
for fw in FW:
    rom, lst, model, cu = roms.build(fw)
    r = Radio(rom, lst, model=model, cu=cu)
    r.run(3.0)
    r.type("43350")
    r.set_ptt(1); r.run(1.0); r.set_ptt(0); r.run(0.5)
    r.power(False); r.run(2.0); r.power(True); r.run(3.0)
    r.set_input("poweroff", 1); r.run(3.0); r.set_input("poweroff", 0)
    r.power(True); r.run(3.0)
    r.type("1#"); r.run(1.0)
    d = [r.ram(), repr(r.cpu()), repr(r.events), repr(r.glyphs()),
         repr([r.latch(i) for i in range(8)])]
    hh = hashlib.sha256("".join(map(str, d)).encode()).hexdigest()[:16]
    print(fw, hh, len(r.events))
    h.update(hh.encode())
print("ALL", h.hexdigest()[:16])
