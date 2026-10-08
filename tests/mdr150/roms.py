"""
The MDR150 test firmware: OH5NXO's HaMDR 174 (2012-09-17, his 2015
build, hamdr.hex) and his MDR150 bootstrap (2009-12-06) from his archive
(no licence; never committed).  The flash image is the Am29F010 as the
radio's chip would hold it: the bootstrap in the boot sector (0x20000 on
the CPU's final map, 0x00000 at reset) and HaMDR from 0x24000.  Found in
MDR150_REF (a directory holding hamdr.hex and bootstrap), this repo's
reference/ after `make refs`, or the firmware repo's reference/.

    python3 tests/mdr150/roms.py          # print the directory
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
MEMBER = os.path.join("oh5nxo", "mods", "MDR150", "hamdr")
FLASH_BASE = 0x20000
FLASH_SIZE = 0x20000


class Unavailable(Exception):
    pass


def refdir():
    cands = [os.environ.get("MDR150_REF"),
             os.path.join(HERE, "..", "..", "reference", MEMBER),          # make refs
             os.path.join(HERE, "..", "..", "..", "reference", MEMBER),    # firmware repo
             os.path.join(HERE, "..", "..", "..", "moppe", "reference", MEMBER)]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "hamdr.hex")) \
                and os.path.isfile(os.path.join(c, "bootstrap")):
            return os.path.abspath(c)
    raise Unavailable("HaMDR not found: set MDR150_REF or run make refs")


def srec(path):
    """{address: byte} from an S-record file (S1/S2/S3; a leading 'L',
    the MDR loader's command, is ignored)"""
    mem = {}
    for line in open(path, "rb").read().decode("latin-1").splitlines():
        line = line.strip().lstrip("L")
        if len(line) < 4 or line[0] != "S" or line[1] not in "123":
            continue
        al = {"1": 2, "2": 3, "3": 4}[line[1]]
        n = int(line[2:4], 16)
        a = int(line[4:4 + 2 * al], 16)
        data = bytes.fromhex(line[4 + 2 * al:2 + 2 * (n + 1) - 2])
        for i, b in enumerate(data):
            mem[a + i] = b
    return mem


def flash_image(hex_path=None, boot_path=None):
    """128 KB Am29F010 image: bootstrap at offset 0, HaMDR where its
    S-records say (0x24000.. = offset 0x4000..), the rest erased"""
    d = None
    if hex_path is None or boot_path is None:
        d = refdir()
    img = bytearray(b"\xFF" * FLASH_SIZE)
    boot = open(boot_path or os.path.join(d, "bootstrap"), "rb").read()
    img[:len(boot)] = boot
    for a, b in srec(hex_path or os.path.join(d, "hamdr.hex")).items():
        if FLASH_BASE <= a < FLASH_BASE + FLASH_SIZE:
            img[a - FLASH_BASE] = b
    return bytes(img)


if __name__ == "__main__":
    print(refdir())
