"""
RB58VY L8M board (r58.c with R58_L8M): small hand-made programs for the
memory map and the watchdog, OH5NXO's R58bis built for L8M (keypad,
display, setup menu, 6 m synthesizer frames, TX keying), and the original
Nokia RB58VY EPROM0 (its EEPROM default copy).

    python3 -m unittest discover -s tests/l8m       # or: make test-l8m
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, HERE)

import roms                                    # noqa: E402
from r58emu import Radio, L8M, CU53AN          # noqa: E402


def rom_file(code):
    """a temporary ROM image holding `code` at 0"""
    f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    f.write(bytes(code) + b"\xff" * (0x8000 - len(code)))
    f.close()
    return f.name


def firmware(name):
    try:
        return roms.path(name)
    except roms.Unavailable as e:
        raise unittest.SkipTest(str(e))


class Board(unittest.TestCase):
    def test_eeprom_window(self):
        # PIO B mode 3, PB1..3 inputs; SMEM=0 EEA10=1, write C123;
        # SMEM=1, write C123 (RAM); E123 with SMEM=0 EEA10=0
        code = [0x3E, 0xCF, 0xD3, 0x03, 0x3E, 0x0E, 0xD3, 0x03,
                0x3E, 0x01, 0xD3, 0x01, 0x3E, 0x5A, 0x32, 0x23, 0xC1,
                0x3E, 0x00, 0xD3, 0x01, 0x3E, 0x3C, 0x32, 0x23, 0xE1,
                0x3E, 0x20, 0xD3, 0x01, 0x3E, 0xA5, 0x32, 0x23, 0xC1,
                0x76]
        p = rom_file(code)
        try:
            r = Radio(p, card=L8M, cu=CU53AN)
            r.run(0.01)
            nv = r.nv()
            self.assertEqual(nv[0x523], 0x5A)    # A10 from PB0, A9..0 from the bus
            self.assertEqual(nv[0x123], 0x3C)    # mirrored over the RAM area
            self.assertEqual(r.peek(0xC123), 0xA5)
        finally:
            os.unlink(p)

    def test_local_stops_watchdog(self):
        p = rom_file([0x18, 0xFE])          # JR $, never kicks port 0x70
        try:
            for local, resets in ((0, True), (1, False)):
                r = Radio(p, card=L8M, cu=CU53AN)
                r.local(local)
                r.run(2)
                self.assertEqual(bool(r.take_events("WDRESET")), resets, local)
        finally:
            os.unlink(p)


class OH5NXO(unittest.TestCase):
    """R58bis for L8M: 6 m defaults (setup.c), S8M synthesizer (MC145156,
    40/41 prescaler, 12.5 kHz), 45 MHz IF"""

    def setUp(self):
        self.r = Radio(firmware(roms.NXO), card=L8M, cu=CU53AN, prescaler=40)
        self.r.run(2)

    def test_boot_and_keypad(self):
        r = self.r
        self.assertEqual(r.display(), ("1  125", "         0"))
        self.assertFalse(r.take_events("WDRESET"))
        r.type("145")
        self.assertEqual(r.display()[1].strip(), "145")

    def test_setup_menu(self):
        r = self.r
        r.press("E")
        r.run(0.3)
        seen = [r.display()[0]]
        for _ in range(4):
            r.press("#")
            r.run(0.3)
            seen.append(r.display()[0])
        self.assertEqual(seen, ["tPc   ", "t 0FF5", "CtC55 ", "5qUELC", "5CAnnE"])

    def test_6m_tx(self):
        r = self.r
        r.type("50510")
        r.press("#")
        r.run(0.5)
        self.assertEqual(r.display()[1].strip(), "50512")     # 12.5 kHz raster
        self.assertAlmostEqual(r.vco_hz(), 95.5125e6)         # + 45 MHz IF
        r.take_events()
        r.ptt(1)
        r.run(0.5)
        self.assertTrue(r.take_events("TX_ON"))
        self.assertAlmostEqual(r.vco_hz(tx=True), 50.5125e6)
        r.ptt(0)
        r.run(0.5)
        self.assertTrue(r.take_events("TX_OFF"))

    def test_out_of_band_no_tx(self):
        r = self.r
        r.type("145500")
        r.press("#")
        r.run(0.5)
        r.take_events()
        r.ptt(1)
        r.run(0.5)
        self.assertFalse(r.take_events("TX_ON"))


class Nokia(unittest.TestCase):
    """original RB58VY EPROM0 only"""

    def test_eeprom_defaults_then_eprom1(self):
        r = Radio(firmware(roms.NOKIA), card=L8M, cu=CU53AN)
        r.breakpoint(0x98A3)                # task 4 entry, in EPROM1
        self.assertEqual(r.run(60), "break")
        self.assertEqual(r.display(), ("888888", "8888888888"))   # lamp test
        nv = r.nv()
        # both 1 KB blocks hold the same parameter list, with its markers
        self.assertEqual(nv[0x000:0x13E], nv[0x400:0x53E])
        self.assertEqual(nv[0x132:0x134], b"\xaa\x55")
        self.assertGreater(sum(1 for b in nv[:0x140] if b), 100)


if __name__ == "__main__":
    unittest.main()
