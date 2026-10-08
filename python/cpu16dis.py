"""
CPU16 disassembler for traces and debugging, from the opcode table the
core is generated from (tools/cpu16_ops.json).  Branch targets follow the
CPU16 pipeline rule: instruction address + 6 + offset.

    from cpu16dis import disasm
    n, text = disasm(read, addr)      # read(a) -> byte
"""
import json
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_TAB = None


def _table():
    global _TAB
    if _TAB is None:
        rows = json.load(open(os.path.join(_HERE, "..", "tools", "cpu16_ops.json")))
        _TAB = {}
        for x in rows:
            op = int(x['op'], 16)
            if len(x['op']) == 4:
                key = (op >> 8, op & 0xFF)
            else:
                key = (None, op)
            _TAB[key] = (x['mn'], x['mode'].replace(' ', ''))
    return _TAB


def _sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def disasm(read, addr, sym=None):
    """(length, text) of the instruction at the 20-bit address addr"""
    def b(i):
        return read((addr + i) & 0xFFFFF)

    def w(i):
        return b(i) << 8 | b(i + 1)

    def name(a):
        a &= 0xFFFFF
        if sym:
            s = sym(a)
            if s:
                return s
        return "%05X" % a

    tab = _table()
    b0, b1 = b(0), b(1)
    if b0 in (0x17, 0x27, 0x37):
        ent = tab.get((b0, b1))
        p = 2
    else:
        ent = tab.get((None, b0))
        p = 1
    if ent is None:
        return 2, "dc.w $%04X" % w(0)
    mn, mode = ent
    mn = mn.lower()
    reg = mode[-1].lower() if mode[-2:] in (",X", ",Y", ",Z") else ''
    base = mode.split(',')[0]
    tgt6 = addr + 6

    if mn in ("bset", "bclr", "brset", "brclr"):
        mask = b(p)
        q = p + 1
        if base == "IND8":
            opnd, q = "$%02X,%s" % (b(q), reg), q + 1
        elif base == "IND16":
            opnd, q = "%d,%s" % (_sx(w(q), 16), reg), q + 2
        else:
            opnd, q = "$%04X" % w(q), q + 2
        text = "%s %s, #$%02X" % (mn, opnd, mask)
        if mn.startswith("br"):
            if base == "IND8":
                off, q = _sx(b(q), 8), q + 1
            else:
                off, q = _sx(w(q), 16), q + 2
            text += ", " + name(tgt6 + off)
        return q, text
    if mn in ("bsetw", "bclrw"):
        if base == "IND16":
            opnd = "%d,%s" % (_sx(w(p), 16), reg)
        else:
            opnd = "$%04X" % w(p)
        return p + 4, "%s %s, #$%04X" % (mn, opnd, w(p + 2))
    if mn in ("movb", "movw"):
        if base == "EXTtoEXT":
            return p + 4, "%s $%04X, $%04X" % (mn, w(p), w(p + 2))
        if base == "IXPtoEXT":
            return p + 3, "%s %d,x, $%04X" % (mn, _sx(b(p), 8), w(p + 1))
        return p + 3, "%s $%04X, %d,x" % (mn, w(p + 1), _sx(b(p), 8))
    if mn in ("mac", "rmac"):
        v = b(p)
        return p + 1, "%s %d,%d" % (mn, _sx(v >> 4, 4), _sx(v & 0xF, 4))
    if mn in ("pshm", "pulm"):
        regs = (["d", "e", "x", "y", "z", "k", "ccr"] if mn == "pshm"
                else ["ccr", "k", "z", "y", "x", "e", "d"])
        v = b(p)
        return p + 1, "%s %s" % (mn, ",".join(r for i, r in enumerate(regs) if v >> i & 1))

    if base == "INH":
        return p, mn
    if base == "IMM8":
        return p + 1, "%s #$%02X" % (mn, b(p))
    if base == "IMM16":
        return p + 2, "%s #$%04X" % (mn, w(p))
    if base == "IND8":
        return p + 1, "%s $%02X,%s" % (mn, b(p), reg)
    if base == "IND16":
        return p + 2, "%s %d,%s" % (mn, _sx(w(p), 16), reg)
    if base == "E":
        return p, "%s e,%s" % (mn, reg)
    if base == "EXT":
        return p + 2, "%s $%04X" % (mn, w(p))
    if base == "IND20":
        return p + 3, "%s %d,%s" % (mn, _sx((b(p) & 0xF) << 16 | w(p + 1), 20), reg)
    if base == "EXT20":
        return p + 3, "%s %s" % (mn, name((b(p) & 0xF) << 16 | w(p + 1)))
    if base == "REL8":
        return p + 1, "%s %s" % (mn, name(tgt6 + _sx(b(p), 8)))
    if base == "REL16":
        return p + 2, "%s %s" % (mn, name(tgt6 + _sx(w(p), 16)))
    return 2, "%s ?%s" % (mn, mode)


if __name__ == "__main__":
    import sys
    data = open(sys.argv[1], "rb").read()
    org = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    a = 0
    while a < len(data):
        n, t = disasm(lambda x: data[x - org] if 0 <= x - org < len(data) else 0, org + a)
        print("%05X  %-18s %s" % (org + a, " ".join("%02X" % data[a + i] for i in range(min(n, len(data) - a))), t))
        a += n
