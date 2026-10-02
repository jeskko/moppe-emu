"""
uPD7810 decoder cross-check against as7810: every instruction form the
assembler's grammar (uPD7810.y) accepts is assembled, and both the Python
disassembler (python/upd7810dis.py) and the C core's length decoder
(upd7810_oplen) must give the same text and length as the assembler.
Uses the as7810 from tmx1_v50.zip (roms.py); skips without it.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))

import roms  # noqa: E402
from upd7810dis import disasm  # noqa: E402

R = ["V", "A", "B", "C", "D", "E", "H", "L"]
R1 = ["EAH", "EAL", "B", "C", "D", "E", "H", "L"]
R2 = ["A", "B", "C"]
SR = ["PA", "PB", "PC", "PD", "PF", "MKH", "MKL", "ANM", "SMH", "SML", "EOM",
      "ETMM", "TMM", "MM", "MCC", "MA", "MB", "MC", "MF", "TXB", "TM0", "TM1",
      "ZCM"]
SR1 = ["PA", "PB", "PC", "PD", "PF", "MKH", "MKL", "ANM", "SMH", "EOM", "TMM",
       "RXB", "CR0", "CR1", "CR2", "CR3"]
SR2 = ["PA", "PB", "PC", "PD", "PF", "MKH", "MKL", "ANM", "SMH", "EOM", "TMM"]
RPA = ["[BC]", "[DE]", "[HL]", "[DE]+", "[HL]+", "[DE]-", "[HL]-"]
RPA2 = RPA + ["[DE+0x12]", "[HL+A]", "[HL+B]", "[HL+EA]", "[HL+0x34]"]
RPA3 = ["[DE]", "[HL]", "[DE]++", "[HL]++", "[DE+0x12]", "[HL+A]", "[HL+B]",
        "[HL+EA]", "[HL+0x34]"]
IRF = ["INTFNMI", "INTFT0", "INTFT1", "INTF1", "INTF2", "INTFE0", "INTFE1",
       "INTFEIN", "INTFAD", "INTFSR", "INTFST", "ER", "OV", "AN4", "AN5",
       "AN6", "AN7", "SB"]
# instructions after which as7810 wants the next line flagged '?'
SKIPPERS = set("""addnc subnb gta lta nea eqa ona offa addncx subnbx gtax ltax
    neax eqax onax offax adinc gti lti nei eqi oni offi suinb addncw subnbw
    gtaw ltaw neaw eqaw onaw offaw gtiw ltiw neiw eqiw oniw offiw daddnc
    dsubnb dgt dlt dne deq don doff inr inrw dcr dcrw sllc slrc bit sk skn
    skit sknit""".split())


def forms():
    """Every operand form of every instruction in as7810's grammar."""
    L = []
    for r in R1:
        L += ["mov %s, A" % r, "mov A, %s" % r]
    L += ["mov %s, A" % s for s in SR]
    L += ["mov A, %s" % s for s in SR1]
    for r in R:
        L += ["mov %s, [0x1234]" % r, "mov [0x1234], %s" % r, "mvi %s, 0x56" % r]
    L += ["mvi %s, 0x56" % s for s in SR2]
    L += ["mviw 0x12, 0x34"] + ["mvix %s, 0x56" % x for x in RPA[:3]]
    L += ["staw 0x12", "ldaw 0x12"]
    for x in RPA2:
        L += ["stax " + x, "ldax " + x]
    for x in RPA3:
        L += ["steax " + x, "ldeax " + x]
    L += ["exx", "exa", "exh", "block"]
    for p in ["BC", "DE", "HL"]:
        L += ["dmov %s, EA" % p, "dmov EA, %s" % p]
    L += ["dmov ETM0, EA", "dmov ETM1, EA", "dmov EA, ECNT", "dmov EA, ECPT"]
    L += [n + " [0x1234]" for n in
          ["sspd", "sbcd", "sded", "shld", "lspd", "lbcd", "lded", "lhld"]]
    for p in ["VA", "BC", "DE", "HL", "EA"]:
        L += ["push " + p, "pop " + p]
    L += ["lxi %s, 0x1234" % p for p in ["SP", "BC", "DE", "HL", "EA"]]
    L.append("table")
    for op in ["add", "adc", "addnc", "sub", "sbb", "subnb", "ana", "ora",
               "xra", "gta", "lta", "nea", "eqa"]:
        for r in R:
            L += ["%s A, %s" % (op, r), "%s %s, A" % (op, r)]
    for op in ["ona", "offa"]:
        L += ["%s A, %s" % (op, r) for r in R]
    for op in ["addx", "adcx", "addncx", "subx", "sbbx", "subnbx", "anax",
               "orax", "xrax", "gtax", "ltax", "neax", "eqax", "onax", "offax"]:
        L += ["%s %s" % (op, x) for x in RPA]
    for op in ["adi", "aci", "adinc", "sui", "sbi", "suinb", "ani", "ori",
               "xri", "gti", "lti", "nei", "eqi", "oni", "offi"]:
        L += ["%s %s, 0x56" % (op, r) for r in R]
        L += ["%s %s, 0x56" % (op, s) for s in SR2]
    L += [op + " 0x12" for op in
          ["addw", "adcw", "addncw", "subw", "sbbw", "subnbw", "anaw", "oraw",
           "xraw", "gtaw", "ltaw", "neaw", "eqaw", "onaw", "offaw"]]
    L += [op + " 0x12, 0x34" for op in
          ["aniw", "oriw", "gtiw", "ltiw", "neiw", "eqiw", "oniw", "offiw"]]
    for r in R2:
        L += [x % r for x in ["eadd EA, %s", "esub EA, %s", "mul %s", "div %s",
                              "inr %s", "dcr %s", "rll %s", "rlr %s", "sll %s",
                              "slr %s", "sllc %s", "slrc %s"]]
    for op in ["dadd", "dadc", "daddnc", "dsub", "dsbb", "dsubnb", "dan", "dor",
               "dxr", "dgt", "dlt", "dne", "deq", "don", "doff"]:
        L += ["%s EA, %s" % (op, p) for p in ["BC", "DE", "HL"]]
    L += ["inrw 0x12", "dcrw 0x12"]
    for p in ["SP", "BC", "DE", "HL", "EA"]:
        L += ["inx " + p, "dcx " + p]
    L += ["daa", "stc", "clc", "nega", "rld", "rrd", "drll EA", "drlr EA",
          "dsll EA", "dslr EA"]
    L += ["jmp 0x1234", "call 0x1234", "jmp BC", "call BC", "jmp EA", "jr .",
          "jr .+32", "jr .-31", "jre .+257", "jre .-254", "calf 0x0812",
          "calf 0x0FFE", "calt 0", "calt 31", "softi", "ret", "rets", "reti"]
    L += ["bit %d, 0x12" % b for b in range(8)]
    for f in ["CY", "HC", "Z"]:
        L += ["sk " + f, "skn " + f]
    for i in IRF:
        L += ["skit " + i, "sknit " + i]
    L += ["nop", "ei", "di", "hlt", "stop"]
    return L


def assemble(lines):
    """[(address, bytes)] per line, from as7810 -d's listing."""
    try:
        src = roms._unpack(roms.reference() or "")
    except (OSError, TypeError) as e:
        raise roms.Unavailable(str(e))
    out, prev = [], False
    for x in lines:
        out.append(("\t?" if prev else "\t") + x + "\n")
        prev = x.split()[0] in SKIPPERS
    with tempfile.TemporaryDirectory() as d:
        asm = os.path.join(d, "all.asm")
        with open(asm, "w") as f:
            f.write("\t.text\n\t.org 0x1000\n" + "".join(out))
        try:
            p = subprocess.run([os.path.join(src, "as7810"), "-d", "-o",
                                os.path.join(d, "all.bin"), asm], cwd=d,
                               capture_output=True, text=True)
        except OSError as e:
            raise roms.Unavailable("as7810 does not run here (%s)" % e)
        if p.returncode:
            raise AssertionError("as7810 failed: " + p.stderr[-300:])
    chunks = []
    for l in p.stdout.splitlines():
        m = re.match(r"L([0-9A-F]{4}): ([0-9A-F ]+)", l)
        if m:
            chunks.append((int(m.group(1), 16),
                           [int(x, 16) for x in m.group(2).split()]))
    return sorted(chunks)


def norm(t):
    return t.replace(" ", "").upper()


class Encodings(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.lines = forms()
        try:
            if not roms.reference():
                raise roms.Unavailable("tmx1_v50.zip not found")
            cls.chunks = assemble(cls.lines)
        except roms.Unavailable as e:
            raise unittest.SkipTest(str(e))
        cls.mem = bytearray(65536)
        for a, bs in cls.chunks:
            cls.mem[a:a + len(bs)] = bytes(bs)

    def test_one_chunk_per_form(self):
        self.assertEqual(len(self.chunks), len(self.lines))
        self.assertGreater(len(self.lines), 980)

    def test_disassembler(self):
        bad = []
        for (a, bs), s in zip(self.chunks, self.lines):
            txt, n = disasm(lambda x: self.mem[x], a)
            exp = s
            m = re.match(r"(jr|jre) \.([+-]\d+)?", s)
            if m:
                exp = "%s 0x%04X" % (m.group(1), a + int(m.group(2) or 0))
            if n != len(bs) or norm(txt) != norm(exp):
                bad.append("%04X %s: got %s (%d of %d bytes)" % (a, s, txt, n, len(bs)))
        self.assertFalse(bad, "\n".join(bad[:20]))

    def test_core_lengths(self):
        from tmx1emu import lib
        L = lib()
        m = L.tmx1api_new(0)
        try:
            for a, bs in self.chunks:
                for i, b in enumerate(bs):
                    L.tmx1api_poke(m, a + i, b)
            bad = [(hex(a), len(bs), L.tmx1api_oplen(m, a))
                   for a, bs in self.chunks if L.tmx1api_oplen(m, a) != len(bs)]
        finally:
            L.tmx1api_free(m)
        self.assertFalse(bad, bad[:20])


if __name__ == "__main__":
    unittest.main()
