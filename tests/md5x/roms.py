"""
Build the Talkman test firmware from the (gitignored, third-party)
reference sources: OH3NWQ mx5x.asm v3.183 and OH1E md50.asm #42, with the
as06 assembler from the OH3NWQ v3.18 release zip.

The sources live in the firmware repo's reference/md5x/ (see its
notes/md5x.md), or in this repo's reference/md5x/ after `make refs`; set
MD5X_REF to point elsewhere.  as06 is a 2008 i386
Linux binary, so a 32-bit runtime (/lib/ld-linux.so.2) is needed.

    python3 tests/md5x/roms.py          # build all, print paths
"""
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")

# name: (source, model, handset, -D options)
BUILDS = {
    # OH3NWQ, Finnish ham band, mic-PTT keymap, 31.4 MHz crystal
    "mx5x-md50": ("mx5x", "MD50", "CU53",
                  "MD50 CU53 PROM MPTT NORMAL FINHAM SHIFT16_20 X314 STEP25 NOPIN EUROPE"),
    "mx5x-md59": ("mx5x", "MD59", "CU59",
                  "MD59 CU59 PROM MPTT NORMAL FINHAM SHIFT16_20 X314 STEP25 NOPIN EUROPE"),
    "mx5x-me59": ("mx5x", "ME59", "CU59",
                  "ME59 CU59 PROM MPTT NORMAL EUROHAM X81 NOPIN STEP125"),
    # OH1E rewrite (as in its own 'mak')
    "oh1e-md50": ("oh1e", "MD50", "CU53", "MD50"),
    "oh1e-md59": ("oh1e", "MD59", "CU59", "MD59"),
    "oh1e-me59": ("oh1e", "ME59", "CU59", "ME59"),
}


def reference():
    cands = [os.environ.get("MD5X_REF"),
             os.path.join(HERE, "..", "..", "reference", "md5x"),   # make refs
             os.path.join(HERE, "..", "..", "..", "reference", "md5x"),   # emu/ in moppe
             os.path.join(HERE, "..", "..", "..", "moppe", "reference", "md5x")]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "oh3nwq-moppe", "mx5x.asm")):
            return os.path.abspath(c)
    return None


class Unavailable(Exception):
    pass


def _as06(ref):
    exe = os.path.join(BUILD, "as06")
    if not os.path.exists(exe):
        os.makedirs(BUILD, exist_ok=True)
        with zipfile.ZipFile(os.path.join(ref, "oh3nwq-moppe", "md59_v318.zip")) as z:
            with open(exe, "wb") as f:
                f.write(z.read("as06"))
        os.chmod(exe, 0o755)
    return exe


def build(name):
    """(bin, listing, model, handset) for one of BUILDS, building it if needed."""
    ref = reference()
    if not ref:
        raise Unavailable("reference/md5x sources not found (set MD5X_REF)")
    src, model, cu, defs = BUILDS[name]
    asm = {"mx5x": os.path.join(ref, "oh3nwq-moppe", "mx5x.asm"),
           "oh1e": os.path.join(ref, "titanix", "md50bis", "md50.asm")}[src]
    out = os.path.join(BUILD, name + ".bin")
    lst = os.path.join(BUILD, name + ".lst")
    if not (os.path.exists(out) and os.path.getmtime(out) >= os.path.getmtime(asm)):
        exe = _as06(ref)
        args = [exe, "-d"] + ["-D" + d for d in defs.split()] + ["-o", out, asm]
        try:
            with open(lst, "w") as f:
                p = subprocess.run(args, stdout=f, stderr=subprocess.PIPE,
                                   cwd=BUILD, text=True)
        except OSError as e:
            raise Unavailable("as06 does not run here (%s)" % e)
        if p.returncode or not os.path.exists(out):
            raise Unavailable("as06 failed (%d): %s" % (p.returncode, p.stderr[-300:]))
    return out, lst, model, cu


if __name__ == "__main__":
    for n in BUILDS:
        try:
            print(n, *build(n))
        except Unavailable as e:
            print(n, "unavailable:", e)
            sys.exit(1)
