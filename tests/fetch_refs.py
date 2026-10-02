#!/usr/bin/env python3
"""
Fetch the third-party firmware sources and assemblers the scenario tests
build from, from their authors' public sites, into reference/ (gitignored).
Nothing of it is in this repository: each file stays under its authors'
terms (README.md, "Test firmware"), and fetching it here is the same as
downloading it from them.

    python3 tests/fetch_refs.py          # or: make refs
    python3 tests/fetch_refs.py --check  # verify what is there, fetch nothing

The layout matches the firmware repo's reference/, so the roms.py
builders find either.  The hashes are of the files as published on
2026-10-02; a changed upstream file is reported, not used.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
REF = os.path.join(ROOT, "reference")

OH3NWQ = "https://github.com/oh3nwq/moppe/raw/master/"
OH3TR_MC25 = "https://oh3tr.fi/~ftp/modifications/mobira/mc25ptl/"
TITANIX = "https://titanix.net/DMR/"

# (path under reference/, URL, sha256, what)
FILES = [
    ("md5x/oh3nwq-moppe/mx5x.asm", OH3NWQ + "mx5x.asm",
     "8899b7df9e57f049", "OH3NWQ Talkman MD50/MD59/ME59 firmware v3.183"),
    ("md5x/oh3nwq-moppe/md59_v318.zip", OH3NWQ + "md59_v318.zip",
     "9ba588f102c65bf2", "OH3NWQ v3.18 release (its as06 assembler)"),
    ("md5x/oh3nwq-moppe/tmx1_v50.zip", OH3NWQ + "tmx1_v50.zip",
     "98810eb2b94db048", "OH5NXO/OH3NWQ TMF-1/TMN-1 v5.0, HSN-2/HSF-2, as7810"),
    ("md5x/titanix/md50bis/md50.asm", TITANIX + "md50/md50bis/md50.asm",
     "92bf6c52195786b6", "OH1E Talkman firmware #42"),
    ("mc25ptl/mc25ptl/mc25.asm", OH3TR_MC25 + "mc25.asm",
     "1372f3e3f2172c81", "OH5NXO/OH3NWQ MC25 TVL/PTL firmware v3.6"),
    ("mc25ptl/mc25ptl/as06", OH3TR_MC25 + "as06",
     "26a7dfdf561f5147", "as06 assembler shipped with it"),
    ("r58/r58p8x3Z.bin.als", TITANIX + "r58/r58p8x3Z.bin.als",
     "cdfe7016a9917664", "OH1E/OH5NXO R58 firmware v3_Z ALs (binary, runs without symbols)"),
    ("r58/r58.asm.als", TITANIX + "r58/r58.asm.als",
     "70c4426ade7823bd", "its as80 source (reference only)"),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only verify")
    ap.add_argument("--force", action="store_true", help="fetch again")
    a = ap.parse_args()
    bad = 0
    for path, url, want, what in FILES:
        dst = os.path.join(REF, path)
        if os.path.exists(dst) and not a.force:
            data = open(dst, "rb").read()
            src = "have"
        elif a.check:
            print("missing  %s  (%s)" % (path, what))
            bad += 1
            continue
        else:
            try:
                with urllib.request.urlopen(url, timeout=120) as f:
                    data = f.read()
            except OSError as e:
                print("FAILED   %s  %s: %s" % (path, url, e))
                bad += 1
                continue
            src = "fetched"
        h = sha(data)
        if want and not h.startswith(want):
            print("CHANGED  %s  sha256 %s, expected %s...  (%s)" % (path, h[:16], want, url))
            bad += 1
            continue
        if src == "fetched":
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data)
        print("%-8s %s  (%s)" % (src, path, what))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
