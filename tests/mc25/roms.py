"""
Build the MC25 TVL/PTL test firmware (OH5NXO/OH3NWQ mc25.asm v3.6) from
the firmware repo's gitignored reference/mc25ptl/ (a mirror of
oh3tr.fi/~ftp/modifications/mobira/mc25ptl/), with the as06 shipped
there (i386 binary: needs a 32-bit runtime).  MC25_REF points elsewhere.

    python3 tests/mc25/roms.py
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")

# name: (-D options, STEP_kHz)
BUILDS = {
    "mc25-25k": ("", 25),
    "mc25-12k": ("STEP_kHz=12", 12.5),
}


class Unavailable(Exception):
    pass


def reference():
    cands = [os.environ.get("MC25_REF"),
             os.path.join(HERE, "..", "..", "..", "reference", "mc25ptl", "mc25ptl"),
             os.path.join(HERE, "..", "..", "..", "moppe", "reference", "mc25ptl", "mc25ptl")]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "mc25.asm")):
            return os.path.abspath(c)
    return None


def build(name):
    """(bin, listing, step_khz) for one of BUILDS, building it if needed."""
    ref = reference()
    if not ref:
        raise Unavailable("reference/mc25ptl sources not found (set MC25_REF)")
    defs, step = BUILDS[name]
    asm = os.path.join(ref, "mc25.asm")
    out = os.path.join(BUILD, name + ".bin")
    lst = os.path.join(BUILD, name + ".lst")
    if not (os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(asm)):
        os.makedirs(BUILD, exist_ok=True)
        exe = os.path.join(BUILD, "as06")
        if not os.path.exists(exe):
            shutil.copy(os.path.join(ref, "as06"), exe)
            os.chmod(exe, 0o755)
        args = [exe, "-d"] + ["-D" + d for d in defs.split()] + ["-o", out, asm]
        try:
            with open(lst, "w") as f:
                p = subprocess.run(args, stdout=f, stderr=subprocess.PIPE,
                                   cwd=BUILD, text=True)
        except OSError as e:
            raise Unavailable("as06 does not run here (%s)" % e)
        if p.returncode or not os.path.exists(out):
            raise Unavailable("as06 failed (%d): %s" % (p.returncode, p.stderr[-300:]))
    return out, lst, step


if __name__ == "__main__":
    for n in BUILDS:
        try:
            print(n, *build(n))
        except Unavailable as e:
            print(n, "unavailable:", e)
            sys.exit(1)
