"""
The L8M test firmware, from OH5NXO's archive (no licence; never
committed; `make refs` fetches it):

- R58vy/rom.0: EPROM0 (27C256) of a Mobira RB58VY, original Nokia
  firmware.  EPROM1 (0x8000..0xBFFF) was not dumped, and five of its 16
  tasks start there, so it runs only up to the first of them.
- R58bis/R58/L8M.bin: OH5NXO's R58bis (C, SDCC 2.9.0, 2014-02-11) built
  for L8M, 48 KB: 0x0000..0x7FFF EPROM0, 0x8000..0xBFFF EPROM1.

Found in L8M_REF (a directory holding R58vy/ and R58bis/), this repo's
reference/oh5nxo/mods after `make refs`, or the firmware repo's.

    python3 tests/l8m/roms.py          # print the directory
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MEMBER = os.path.join("oh5nxo", "mods")
NOKIA = os.path.join("R58vy", "rom.0")
NXO = os.path.join("R58bis", "R58", "L8M.bin")


class Unavailable(Exception):
    pass


def refdir(need=(NOKIA, NXO)):
    cands = [os.environ.get("L8M_REF"),
             os.path.join(HERE, "..", "..", "reference", MEMBER),          # make refs
             os.path.join(HERE, "..", "..", "..", "reference", MEMBER),    # firmware repo
             os.path.join(HERE, "..", "..", "..", "moppe", "reference", MEMBER)]
    for c in cands:
        if c and all(os.path.isfile(os.path.join(c, n)) for n in need):
            return os.path.abspath(c)
    raise Unavailable("L8M firmware not found: set L8M_REF or run make refs")


def path(name):
    return os.path.join(refdir((name,)), name)


if __name__ == "__main__":
    print(refdir())
