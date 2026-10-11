"""
CDP1802 / CDP1806 disassembler (RCA mnemonics, lower case).

    from cdp1802dis import disasm
    text, length, targets = disasm(read, addr)   # read(addr) -> byte

'targets' lists the static branch/call targets (short branches resolve
in the page of the operand byte, as the CPU does).

As a script, writes a listing of a ROM image.  Addresses executed in an
emulator run (a coverage bitmap from md5xemu.Radio.coverage()) are code;
the rest is shown as bytes, or disassembled with '?' when --all is
given.  A symbol file has lines "ADDR name [; comment]" (hex address).

    python3 cdp1802dis.py rom.bin [--cov cov.bin] [--syms md59.sym]
                          [--from 0x0000 --to 0x7FFF] [--all] [--scrt 4]
"""
import argparse
import re

SHORT = {0x30: "br", 0x31: "bq", 0x32: "bz", 0x33: "bdf", 0x34: "b1",
         0x35: "b2", 0x36: "b3", 0x37: "b4", 0x39: "bnq", 0x3A: "bnz",
         0x3B: "bnf", 0x3C: "bn1", 0x3D: "bn2", 0x3E: "bn3", 0x3F: "bn4"}
LONG = {0xC0: "lbr", 0xC1: "lbq", 0xC2: "lbz", 0xC3: "lbdf", 0xC9: "lbnq",
        0xCA: "lbnz", 0xCB: "lbnf"}
LSKIP = {0xC4: "nop", 0xC5: "lsnq", 0xC6: "lsnz", 0xC7: "lsnf",
         0xC8: "lskp", 0xCC: "lsie", 0xCD: "lsq", 0xCE: "lsz", 0xCF: "lsdf"}
OP7 = ["ret", "dis", "ldxa", "stxd", "adc", "sdb", "shrc", "smb", "sav",
       "mark", "req", "seq", "adci", "sdbi", "shlc", "smbi"]
OPF = ["ldx", "or", "and", "xor", "add", "sd", "shr", "sm", "ldi", "ori",
       "ani", "xri", "adi", "sdi", "shl", "smi"]
REGOP = {0x0: "ldn", 0x1: "inc", 0x2: "dec", 0x4: "lda", 0x5: "str",
         0x8: "glo", 0x9: "ghi", 0xA: "plo", 0xB: "phi", 0xD: "sep",
         0xE: "sex"}
# 1804/5/6 extended (68xx)
EXT = {0x00: "stpc", 0x01: "dtc", 0x02: "spm2", 0x03: "scm2", 0x04: "spm1",
       0x05: "scm1", 0x06: "ldc", 0x07: "stm", 0x08: "gec", 0x09: "etq",
       0x0A: "xie", 0x0B: "xid", 0x0C: "cie", 0x0D: "cid", 0x74: "dadc",
       0x76: "dsav", 0x77: "dsmb", 0xF4: "dadd", 0xF7: "dsm"}
EXT_I = {0x7C: "daci", 0x7F: "dsbi", 0xFC: "dadi", 0xFF: "dsmi"}


def _r(n):
    return "r%X" % n


def disasm(read, addr, cdp1806=True, scrt=None):
    """(text, length, targets) for the instruction at addr.  scrt = the
    SCRT call register (RCA convention: 'sep r4' is followed by the
    2-byte subroutine address), shown as 'call ADDR'."""
    op = read(addr)
    hi, lo = op >> 4, op & 15
    b1 = read((addr + 1) & 0xFFFF)
    b2 = read((addr + 2) & 0xFFFF)
    if op == 0x00:
        return "idl", 1, []
    if scrt is not None and op == 0xD0 | scrt:
        t = b1 << 8 | b2
        return "call %04X" % t, 3, [t]
    if hi in REGOP:
        return "%s %s" % (REGOP[hi], _r(lo)), 1, []
    if hi == 0x3:
        if op == 0x38:
            return "skp", 1, [(addr + 2) & 0xFFFF]
        t = ((addr + 1) & 0xFF00) | b1
        return "%s %04X" % (SHORT[op], t), 2, [t]
    if op == 0x60:
        return "irx", 1, []
    if 0x61 <= op <= 0x67:
        return "out %d" % (op & 7), 1, []
    if 0x69 <= op <= 0x6F:
        return "inp %d" % (op & 7), 1, []
    if op == 0x68:
        if not cdp1806:
            return ".byte 68", 1, []
        e = b1
        b3 = read((addr + 3) & 0xFFFF)
        if e in EXT:
            return EXT[e], 2, []
        if e in EXT_I:
            return "%s %02X" % (EXT_I[e], b2), 3, []
        if e in (0x3E, 0x3F):
            t = ((addr + 2) & 0xFF00) | b2
            return "%s %04X" % ("bci" if e == 0x3E else "bxi", t), 3, [t]
        n = e & 15
        if e >> 4 == 0x6:
            return "rlxa %s" % _r(n), 2, []
        if e >> 4 == 0x8:
            t = b2 << 8 | b3
            return "scal %s,%04X" % (_r(n), t), 4, [t]
        if e >> 4 == 0x9:
            return "sret %s" % _r(n), 2, []
        if e >> 4 == 0xA:
            return "rsxd %s" % _r(n), 2, []
        if e >> 4 == 0xB:
            return "rnx %s" % _r(n), 2, []
        if e >> 4 == 0xC:
            return "rldi %s,%04X" % (_r(n), b2 << 8 | b3), 4, []
        return ".byte 68 %02X" % e, 2, []
    if hi == 0x7:
        if lo in (0xC, 0xD, 0xF):
            return "%s %02X" % (OP7[lo], b1), 2, []
        return OP7[lo], 1, []
    if hi == 0xC:
        if op in LONG:
            t = b1 << 8 | b2
            return "%s %04X" % (LONG[op], t), 3, [t]
        t = (addr + 3) & 0xFFFF
        return LSKIP[op], 1, ([] if op == 0xC4 else [t])
    if hi == 0xF:
        if lo >= 8 and lo != 0xE:
            return "%s %02X" % (OPF[lo], b1), 2, []
        return OPF[lo], 1, []
    return ".byte %02X" % op, 1, []


def load_syms(path):
    """{addr: (name, comment)} from 'ADDR name [; comment]' lines."""
    syms = {}
    if not path:
        return syms
    for line in open(path):
        line = line.rstrip("\n")
        m = re.match(r"\s*([0-9A-Fa-f]{1,4})\s+(\S+)\s*(?:;\s*(.*))?$", line)
        if m:
            syms[int(m.group(1), 16)] = (m.group(2), m.group(3) or "")
    return syms


def listing(rom, cov=None, syms=None, lo=0, hi=None, show_all=False,
            cdp1806=True, scrt=None):
    syms = syms or {}
    hi = len(rom) - 1 if hi is None else hi

    def read(a):
        return rom[a] if a < len(rom) else 0xFF

    def covered(a):
        return cov is None or (a >> 3 < len(cov) and (cov[a >> 3] >> (a & 7)) & 1)

    # labels: targets of executed branches
    refs = {}
    a = 0
    while a < len(rom):
        if covered(a):
            _, n, ts = disasm(read, a, cdp1806, scrt)
            for t in ts:
                refs.setdefault(t, []).append(a)
            a += n
        else:
            a += 1

    def name(t):
        return syms[t][0] if t in syms else "L%04X" % t

    out = []
    a = lo
    while a <= hi:
        if a in syms or a in refs:
            note = syms.get(a, ("", ""))[1]
            xr = refs.get(a, [])
            x = " ; from " + " ".join("%04X" % f for f in xr[:6]) + (" ..." if len(xr) > 6 else "") if xr else ""
            out.append("%s:%s%s" % (name(a), "  ; " + note if note else "", x))
        if covered(a) or show_all:
            text, n, ts = disasm(read, a, cdp1806, scrt)
            for t in ts:
                text = text.replace("%04X" % t, name(t)) if t in syms else text
            raw = " ".join("%02X" % read(a + i) for i in range(n))
            mark = " " if covered(a) else "?"
            out.append("  %04X%s %-12s %s" % (a, mark, raw, text))
            a += n
        else:
            # a run of unexecuted bytes, 16 per line, broken at labels
            run = []
            while a <= hi and not covered(a) and len(run) < 16 and \
                    not (run and (a in syms or a in refs)):
                run.append(read(a))
                a += 1
            ff = all(b == 0xFF for b in run)
            txt = "".join(chr(b) if 32 <= b < 127 else "." for b in run)
            out.append("  %04X. %s%s" % (a - len(run), " ".join("%02X" % b for b in run),
                                         "" if ff else "  |" + txt))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--cov")
    ap.add_argument("--syms")
    ap.add_argument("--from", dest="lo", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--to", dest="hi", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--1802", dest="c1802", action="store_true")
    ap.add_argument("--scrt", type=lambda s: int(s, 0), default=None,
                    help="SCRT call register (e.g. 4: 'sep r4' + address)")
    a = ap.parse_args()
    rom = open(a.rom, "rb").read()
    cov = open(a.cov, "rb").read() if a.cov else None
    print(listing(rom, cov, load_syms(a.syms), a.lo, a.hi, a.all,
                  cdp1806=not a.c1802, scrt=a.scrt))


if __name__ == "__main__":
    main()
