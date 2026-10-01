"""
Build the TMx-1 test firmware from the (gitignored, third-party) reference
sources: OH5NXO / OH3NWQ tmx1.asm v5.0 for the TMF-1 and TMN-1, and the
HSN-2 v1.6 and HSF-2 v0.2 handset firmware, all from OH3NWQ's
tmx1_v50.zip, with the as7810 assembler in the same zip (as its 'mak').

The zip lives in the firmware repo's reference/md5x/oh3nwq-moppe/ (see
its notes/tmx1.md); set TMX1_REF to the directory holding it.  as7810 is
a 2000 i386 Linux binary that runs the system cpp, so a 32-bit runtime
(/lib/ld-linux.so.2) and cpp are needed.

    python3 tests/tmx1/roms.py          # build all, print paths
"""
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
ZIP = "tmx1_v50.zip"

# name: (source, as7810 options)
BUILDS = {
    "tmf1": ("tmx1.asm", "-d -DTMF1"),
    "tmn1": ("tmx1.asm", "-d -DTMN1"),
    "hsn2": ("hsn2.asm", "-d"),
    "hsf2": ("hsf2.asm", "-d"),
}


def reference():
    cands = [os.environ.get("TMX1_REF"),
             os.path.join(HERE, "..", "..", "..", "reference", "md5x", "oh3nwq-moppe"),
             os.path.join(HERE, "..", "..", "..", "moppe", "reference", "md5x", "oh3nwq-moppe")]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, ZIP)):
            return os.path.abspath(c)
    return None


class Unavailable(Exception):
    pass


def _unpack(ref):
    src = os.path.join(BUILD, "src")
    stamp = os.path.join(src, ".unpacked")
    if not os.path.exists(stamp):
        os.makedirs(src, exist_ok=True)
        with zipfile.ZipFile(os.path.join(ref, ZIP)) as z:
            for n in z.namelist():
                if n.endswith(".asm") or n == "as7810":
                    with open(os.path.join(src, os.path.basename(n)), "wb") as f:
                        f.write(z.read(n))
        os.chmod(os.path.join(src, "as7810"), 0o755)
        open(stamp, "w").close()
    return src


def build(name):
    """(bin, listing) for one of BUILDS, building it if needed."""
    ref = reference()
    if not ref:
        raise Unavailable("%s not found (set TMX1_REF)" % ZIP)
    src = _unpack(ref)
    asm, opts = BUILDS[name]
    out = os.path.join(BUILD, name + ".bin")
    lst = os.path.join(BUILD, name + ".lst")
    if not os.path.exists(out):
        args = [os.path.join(src, "as7810")] + opts.split() + ["-o", out, asm]
        try:
            with open(lst, "w") as f:
                p = subprocess.run(args, stdout=f, stderr=subprocess.PIPE,
                                   cwd=src, text=True)
        except OSError as e:
            raise Unavailable("as7810 does not run here (%s)" % e)
        if p.returncode or not os.path.exists(out):
            if os.path.exists(out):
                os.remove(out)
            raise Unavailable("as7810 failed (%d): %s" % (p.returncode, p.stderr[-300:]))
    return out, lst


if __name__ == "__main__":
    for n in BUILDS:
        try:
            print(n, *build(n))
        except Unavailable as e:
            print(n, "unavailable:", e)
            sys.exit(1)
