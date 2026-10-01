"""
uPD7810 disassembler, in the as7810 assembler's syntax (OH5NXO's jas:
"mov A, [word]", "ldax [HL+]", "jr label").

    from upd7810dis import disasm
    text, length = disasm(read, addr)      # read(addr) -> byte
"""

R = ["V", "A", "B", "C", "D", "E", "H", "L"]
R1 = ["EAH", "EAL", "B", "C", "D", "E", "H", "L"]
R2 = [None, "A", "B", "C"]
RP = ["SP", "BC", "DE", "HL", "EA"]
RP1 = ["VA", "BC", "DE", "HL", "EA"]
RPA = {1: "[BC]", 2: "[DE]", 3: "[HL]", 4: "[DE]+", 5: "[HL]+",
       6: "[DE]-", 7: "[HL]-", 0xC: "[HL+A]", 0xD: "[HL+B]", 0xE: "[HL+EA]"}
RPA3 = {2: "[DE]", 3: "[HL]", 4: "[DE]++", 5: "[HL]++", 0xC: "[HL+A]",
        0xD: "[HL+B]", 0xE: "[HL+EA]"}
SR = {0x00: "PA", 0x01: "PB", 0x02: "PC", 0x03: "PD", 0x05: "PF",
      0x06: "MKH", 0x07: "MKL", 0x08: "ANM", 0x09: "SMH", 0x0A: "SML",
      0x0B: "EOM", 0x0C: "ETMM", 0x0D: "TMM", 0x10: "MM", 0x11: "MCC",
      0x12: "MA", 0x13: "MB", 0x14: "MC", 0x17: "MF", 0x18: "TXB",
      0x19: "RXB", 0x1A: "TM0", 0x1B: "TM1", 0x20: "CR0", 0x21: "CR1",
      0x22: "CR2", 0x23: "CR3", 0x28: "ZCM"}
SR_W = {k for k in SR if k not in (0x19, 0x20, 0x21, 0x22, 0x23)}
SR1 = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0D,
       0x19, 0x20, 0x21, 0x22, 0x23}
SR2 = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0D}
IRF = {0: "INTFNMI", 1: "INTFT0", 2: "INTFT1", 3: "INTF1", 4: "INTF2",
       5: "INTFE0", 6: "INTFE1", 7: "INTFEIN", 8: "INTFAD", 9: "INTFSR",
       10: "INTFST", 11: "ER", 12: "OV", 16: "AN4", 17: "AN5", 18: "AN6",
       19: "AN7", 20: "SB"}
# ALU operation field (bits 6..3)
ALU = [None, "ana", "xra", "ora", "addnc", "gta", "subnb", "lta", "add",
       "ona", "adc", "offa", "sub", "nea", "sbb", "eqa"]
ALU_I = [None, "ani", "xri", "ori", "adinc", "gti", "suinb", "lti", "adi",
         "oni", "aci", "offi", "sui", "nei", "sbi", "eqi"]
ALU_D = [None, "dan", "dxr", "dor", "daddnc", "dgt", "dsubnb", "dlt", "dadd",
         "don", "dadc", "doff", "dsub", "dne", "dsbb", "deq"]


def _w(lo, hi):
    return "0x%04X" % (lo | hi << 8)


def _b(v):
    return "0x%02X" % v


def disasm(read, addr):
    """(text, length) for the instruction at addr; '.byte 0xNN' if undefined."""
    b = [read((addr + i) & 0xFFFF) for i in range(4)]
    op = b[0]
    undef = (".byte " + _b(op), 1)

    def rpa2(m, nxt):
        if m in (0xB, 0xF):
            return "[%s+%s]" % ("DE" if m == 0xB else "HL", _b(nxt)), 1
        return RPA.get(m), 0

    if op == 0x00:
        return "nop", 1
    if op == 0x01:
        return "ldaw " + _b(b[1]), 2
    if op & 0xCF == 0x02:
        return "inx " + RP[op >> 4], 1
    if op & 0xCF == 0x03:
        return "dcx " + RP[op >> 4], 1
    if op in (0x04, 0x14, 0x24, 0x34, 0x44):
        return "lxi %s, %s" % (RP[op >> 4], _w(b[1], b[2])), 3
    if op & 0x8F == 0x05:
        k = ((op >> 4) << 1) | 1
        return "%siw %s, %s" % (ALU_I[k][:-1], _b(b[1]), _b(b[2])), 3
    if op & 0x8E == 0x06 and op != 0x06:
        k = ((op >> 4) << 1) | (op & 1)
        return "%s A, %s" % (ALU_I[k], _b(b[1])), 2
    if 0x08 <= op <= 0x0F:
        return "mov A, " + R1[op & 7], 1
    if op == 0x10:
        return "exa", 1
    if op == 0x11:
        return "exx", 1
    if 0x18 <= op <= 0x1F:
        return "mov %s, A" % R1[op & 7], 1
    if op == 0x20:
        return "inrw " + _b(b[1]), 2
    if op == 0x21:
        return "jmp BC", 1
    if op == 0x30:
        return "dcrw " + _b(b[1]), 2
    if op == 0x31:
        return "block", 1
    if (0x29 <= op <= 0x2F) or (0xAB <= op <= 0xAF):
        s, n = rpa2((op & 7) | (op >> 4 & 8), b[1])
        return "ldax " + s, 1 + n
    if (0x39 <= op <= 0x3F) or (0xBB <= op <= 0xBF):
        s, n = rpa2((op & 7) | (op >> 4 & 8), b[1])
        return "stax " + s, 1 + n
    if op == 0x40:
        return "call " + _w(b[1], b[2]), 3
    if 0x41 <= op <= 0x43:
        return "inr " + R2[op & 3], 1
    if 0x49 <= op <= 0x4B:
        return "mvix %s, %s" % (RPA[op & 3], _b(b[1])), 2
    if op == 0x4C:
        s = b[1] & 0x3F
        if b[1] & 0xC0 != 0xC0 or s not in SR1:
            return undef
        return "mov A, " + SR[s], 2
    if op == 0x4D:
        s = b[1] & 0x3F
        if b[1] & 0xC0 != 0xC0 or s not in SR_W:
            return undef
        return "mov %s, A" % SR[s], 2
    if op in (0x4E, 0x4F):
        d = (op & 1) << 8 | b[1]
        if d & 0x100:
            d -= 0x200
        return "jre 0x%04X" % ((addr + 2 + d) & 0xFFFF), 2
    if op == 0x50:
        return "exh", 1
    if 0x51 <= op <= 0x53:
        return "dcr " + R2[op & 3], 1
    if op == 0x54:
        return "jmp " + _w(b[1], b[2]), 3
    if 0x58 <= op <= 0x5F:
        return "bit %d, %s" % (op & 7, _b(b[1])), 2
    if op == 0x61:
        return "daa", 1
    if op == 0x62:
        return "reti", 1
    if op == 0x63:
        return "staw " + _b(b[1]), 2
    if 0x68 <= op <= 0x6F:
        return "mvi %s, %s" % (R[op & 7], _b(b[1])), 2
    if op == 0x71:
        return "mviw %s, %s" % (_b(b[1]), _b(b[2])), 3
    if op == 0x72:
        return "softi", 1
    if 0x78 <= op <= 0x7F:
        return "calf 0x%04X" % (0x800 | (op & 7) << 8 | b[1]), 2
    if 0x80 <= op <= 0x9F:
        return "calt %d" % (op & 0x1F), 1
    if 0xA0 <= op <= 0xA4:
        return "pop " + RP1[op & 7], 1
    if 0xA5 <= op <= 0xA7:
        return "dmov EA, " + RP[op & 3], 1
    if op == 0xA8:
        return "inx EA", 1
    if op == 0xA9:
        return "dcx EA", 1
    if op == 0xAA:
        return "ei", 1
    if 0xB0 <= op <= 0xB4:
        return "push " + RP1[op & 7], 1
    if 0xB5 <= op <= 0xB7:
        return "dmov %s, EA" % RP[op & 3], 1
    if op == 0xB8:
        return "ret", 1
    if op == 0xB9:
        return "rets", 1
    if op == 0xBA:
        return "di", 1
    if op >= 0xC0:
        d = op & 0x3F
        if d & 0x20:
            d -= 0x40
        return "jr 0x%04X" % ((addr + 1 + d) & 0xFFFF), 1

    o = b[1]
    if op == 0x48:
        r = o & 3
        t = {0x0A: "sk CY", 0x0B: "sk HC", 0x0C: "sk Z", 0x1A: "skn CY",
             0x1B: "skn HC", 0x1C: "skn Z", 0x28: "jmp EA", 0x29: "call BC",
             0x2A: "clc", 0x2B: "stc", 0x38: "rld", 0x39: "rrd",
             0x3A: "nega", 0x3B: "hlt", 0xA0: "dslr EA", 0xA4: "dsll EA",
             0xA8: "table", 0xB0: "drlr EA", 0xB4: "drll EA", 0xBB: "stop",
             0xC0: "dmov EA, ECNT", 0xC1: "dmov EA, ECPT",
             0xD2: "dmov ETM0, EA", 0xD3: "dmov ETM1, EA"}
        if o in t:
            return t[o], 2
        if r:
            g = {0x00: "slrc", 0x04: "sllc", 0x20: "slr", 0x24: "sll",
                 0x2C: "mul", 0x30: "rlr", 0x34: "rll", 0x3C: "div"}
            if o & 0xFC in g:
                return "%s %s" % (g[o & 0xFC], R2[r]), 2
        if 0x40 <= o < 0x80 and (o & 0x1F) in IRF:
            return "%s %s" % ("skit" if o < 0x60 else "sknit", IRF[o & 0x1F]), 2
        if o & 0xE0 == 0x80:
            m = o & 0x0F
            name = "steax" if o & 0x10 else "ldeax"
            if m in (0xB, 0xF):
                return "%s [%s+%s]" % (name, "DE" if m == 0xB else "HL", _b(b[2])), 3
            if m in RPA3:
                return "%s %s" % (name, RPA3[m]), 2
        return undef
    if op == 0x60:
        k = (o >> 3) & 15
        if not k:
            return undef
        if o & 0x80:
            return "%s A, %s" % (ALU[k], R[o & 7]), 2
        if k in (9, 11):
            return undef
        return "%s %s, A" % (ALU[k], R[o & 7]), 2
    if op == 0x64:
        k = (o >> 3) & 15
        s = (o & 7) | (o >> 4 & 8)
        if s not in SR2:
            return undef
        name = "mvi" if k == 0 else ALU_I[k]
        return "%s %s, %s" % (name, SR[s], _b(b[2])), 3
    if op == 0x70:
        t = {0x0E: "sspd", 0x0F: "lspd", 0x1E: "sbcd", 0x1F: "lbcd",
             0x2E: "sded", 0x2F: "lded", 0x3E: "shld", 0x3F: "lhld"}
        if o in t:
            return "%s [%s]" % (t[o], _w(b[2], b[3])), 4
        if 0x41 <= o <= 0x43:
            return "eadd EA, " + R2[o & 3], 2
        if 0x61 <= o <= 0x63:
            return "esub EA, " + R2[o & 3], 2
        if 0x68 <= o <= 0x6F:
            return "mov %s, [%s]" % (R[o & 7], _w(b[2], b[3])), 4
        if 0x78 <= o <= 0x7F:
            return "mov [%s], %s" % (_w(b[2], b[3]), R[o & 7]), 4
        k = (o >> 3) & 15
        if o & 0x80 and o & 7 and k:
            return "%sx %s" % (ALU[k], RPA[o & 7]), 2
        return undef
    if op == 0x74:
        k = (o >> 3) & 15
        if not k:
            return undef
        if not o & 0x80:
            return "%s %s, %s" % (ALU_I[k], R[o & 7], _b(b[2])), 3
        if o & 7 == 0:
            return "%sw %s" % (ALU[k], _b(b[2])), 3
        if o & 7 >= 5:
            return "%s EA, %s" % (ALU_D[k], RP[o & 3]), 2
        return undef
    return undef
