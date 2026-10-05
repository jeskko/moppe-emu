"""
The R40 test firmware: the original Nokia RC40 ROM "Cr 13.04-0" (1993)
from OH5NXO's archive (proprietary; never committed).  Found in R40_ROM,
this repo's reference/ after `make refs`, or the firmware repo's
reference/.

    python3 tests/r40/roms.py          # print the path
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MEMBER = os.path.join("oh5nxo", "mods", "R40-manuals", "rc40_rom", "ABSBIN")


class Unavailable(Exception):
    pass


def rom():
    cands = [os.environ.get("R40_ROM"),
             os.path.join(HERE, "..", "..", "reference", MEMBER),          # make refs
             os.path.join(HERE, "..", "..", "..", "reference", MEMBER),    # firmware repo
             os.path.join(HERE, "..", "..", "..", "moppe", "reference", MEMBER)]
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    raise Unavailable("R40 ROM not found: set R40_ROM or run make refs")


if __name__ == "__main__":
    print(rom())
