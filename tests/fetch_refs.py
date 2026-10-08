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
import tarfile
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
REF = os.path.join(ROOT, "reference")

OH3NWQ = "https://github.com/oh3nwq/moppe/raw/master/"
OH3TR_MC25 = "https://oh3tr.fi/~ftp/modifications/mobira/mc25ptl/"
TITANIX = "https://titanix.net/DMR/"
OH5NXO_TGZ = "https://oh3tr.fi/~ftp/modifications/sorsat/oh5nxo.mods.2018.tar.gz"

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
    # a member of a 339 MB archive: streamed, the rest not kept
    ("oh5nxo/mods/R40-manuals/rc40_rom/ABSBIN",
     OH5NXO_TGZ + "#mods/R40-manuals/rc40_rom/ABSBIN",
     "5da1d0854c45361a", "Nokia RC40 original firmware Cr 13.04-0 (1993), from OH5NXO's archive"),
    ("oh5nxo/mods/R58vy/rom.0",
     OH5NXO_TGZ + "#mods/R58vy/rom.0",
     "4c4a86399b58d991", "Mobira RB58VY (L8M) EPROM0, original Nokia firmware (EPROM1 not dumped)"),
    ("oh5nxo/mods/R58bis/R58/L8M.bin",
     OH5NXO_TGZ + "#mods/R58bis/R58/L8M.bin",
     "ef255ece90805148", "OH5NXO R58bis 130509#61 built for L8M (2014-02-11)"),
    ("oh5nxo/mods/MDR150/hamdr/hamdr.hex",
     OH5NXO_TGZ + "#mods/MDR150/hamdr/hamdr.hex",
     "4d0a108bb13095f2", "OH5NXO HaMDR 174 (2012-09-17) for the MDR150, his 2015 build"),
    ("oh5nxo/mods/MDR150/hamdr/bootstrap",
     OH5NXO_TGZ + "#mods/MDR150/hamdr/bootstrap",
     "8cf1319ad3211cf4", "OH5NXO MDR150 bootstrap (2009-12-06), binary"),
]


_members = {}


def fetch(url, wanted=()):
    """the file at url, or url#member: that member of a .tar.gz, streamed
    until it is found; the other members in wanted (url#member too) found
    on the way are kept for their own fetch, so one pass serves them all"""
    if "#" not in url:
        with urllib.request.urlopen(url, timeout=120) as f:
            return f.read()
    if url in _members:
        return _members.pop(url)
    base, member = url.split("#", 1)
    want = {w.split("#", 1)[1] for w in wanted if w.startswith(base + "#")}
    want.add(member)
    with urllib.request.urlopen(base, timeout=120) as f:
        with tarfile.open(fileobj=f, mode="r|gz") as t:
            for ti in t:
                if ti.name in want:
                    _members[base + "#" + ti.name] = t.extractfile(ti).read()
                    want.discard(ti.name)
                    if not want:
                        break
    if url not in _members:
        raise OSError("%s not in the archive" % member)
    return _members.pop(url)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="only verify")
    ap.add_argument("--force", action="store_true", help="fetch again")
    a = ap.parse_args()
    bad = 0
    todo = [u for p, u, w, x in FILES
            if a.force or not os.path.exists(os.path.join(REF, p))]
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
                data = fetch(url, todo)
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
