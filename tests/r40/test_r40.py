"""
R40 scenarios on the original Nokia RC40 firmware Cr 13.04-0 (roms.py).
Facts checked against the R40 service manual (error codes, LOCAL mode)
and PE1BVU's RD40 conversion guide (service key, service display).
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, HERE)

import roms                      # noqa: E402
from r40emu import Radio, font_from_rom   # noqa: E402
import r40nv                     # noqa: E402

try:
    ROM = roms.rom()
except roms.Unavailable as e:
    raise unittest.SkipTest(str(e))


def no_faults(r):
    return [e for e in r.events if e[1] in ("ILLEGAL", "EXC")]


class Boot(unittest.TestCase):
    def test_empty_nv_reports_error_6(self):
        # flat NV battery: self test, then "Own subscriber number is lost"
        r = Radio(ROM)
        r.run(4.0)
        self.assertEqual(r.display(), ["Self test", "Service necessary", "Error 6"])
        self.assertEqual(no_faults(r), [])

    def test_control_head_traffic(self):
        r = Radio(ROM)
        r.run(4.0)
        addrs = {b[0] for _, b in r.i2c_log()}
        # PCF8574s 0x40 / 0x42 / 0x44, LCD drivers 0x78, PCD3312 0x48;
        # no service key fitted, so its read at 0xA1 gets no answer
        self.assertTrue({0x40, 0x42, 0x43, 0x44, 0x45, 0x48, 0x78} <= addrs)
        init = [b for _, b in r.i2c_log() if b[:2] == bytes([0x78, 0xD7])]
        self.assertEqual(init[0], bytes([0x78, 0xD7, 0xFC, 0xE0, 0xF0, 0x00]))

    def test_font_found(self):
        with open(ROM, "rb") as f:
            font = font_from_rom(f.read())
        self.assertEqual(len(font), 86)


class Service(unittest.TestCase):
    def test_local_mode_with_service_key(self):
        # CU43PROG (24C02 = 3A 01 F0 ...), PWR held at power-on: all the
        # lost-NV errors, then the LOCAL display "spacing ch phys freq / rsl"
        r = Radio(ROM, service_head=True, power=False)
        d = r.service_mode()
        self.assertEqual(d[0], "0 000 00000 00000000")
        self.assertEqual(d[1], "rsl")
        reads = [b for _, b in r.i2c_log() if b[0] == 0xA1]
        self.assertTrue(reads and reads[0][1:3] == bytes([0x3A, 0x01]))
        self.assertEqual(no_faults(r), [])

    def test_digits_echo_and_ok_runs_test(self):
        r = Radio(ROM, service_head=True, power=False)
        r.service_mode()
        r.type("41")
        self.assertTrue(r.display()[2].endswith("41"))
        r.take_events()
        r.press("OK", hold=0.3, gap=1.0)
        self.assertEqual(r.display()[2].strip(), "075")    # entry consumed
        self.assertTrue(r.take_events("SR"))               # audio switches set

    def test_no_service_mode_without_key(self):
        r = Radio(ROM, power=False)
        r.power_key(True)
        r.power(True)
        r.run(5.0)
        r.power_key(False)
        r.run(20.0)
        self.assertNotEqual(r.display()[1], "rsl")


# PE1BVU's 70 cm set-up, in his order: 0-channels 400 / 445 MHz (test 18),
# simplex (16), 12.5 kHz and band D (15), calibration frequencies (10-12)
BAND_SETUP = ["18164000", "18271200", "16200000", "151", "155",
              "1043000000", "1143500000", "1244000000"]

# NV blocks (start, length, checksum byte), from the firmware's table at
# 0x36966; each has a copy at +0x2000
NV_BLOCKS = [(0x000, 0x12B, 0x12B), (0x12C, 0x09, 0x135), (0x136, 0x95, 0x1CB),
             (0x1CC, 0x225, 0x3F1), (0x3F2, 0x95, 0x487), (0x488, 0x5E5, 0xA6D),
             (0xA6E, 0x41A, 0xE88), (0xE89, 0x220, 0x10A9), (0x10AA, 0x1F4, 0x129E),
             (0x129F, 0x35C, 0x15FB), (0x15FC, 0x78, 0x1674)]


def service_radio(nv=None):
    r = Radio(ROM, nv=nv, service_head=True, power=False)
    r.service_mode()
    return r


def ok(r, keys, wait=1.5):
    r.type(keys)
    r.press("OK", hold=0.3, gap=wait)


def fnc_sto(r):
    r.press("FNC")
    r.press("STO", hold=0.3, gap=1.0)


class Calibration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        r = service_radio()
        for t in BAND_SETUP:
            ok(r, t)
        cls.nv = r.nv()

    def test_band_stored_as_physical_channels(self):
        # (f - 400 MHz) / 6.25 kHz, RX and TX words, at 0x56 / 0x5A / 0x5E
        nv = self.nv
        for off, ch in ((0x56, 4800), (0x5A, 5600), (0x5E, 6400)):
            self.assertEqual(nv[off:off + 4], (ch.to_bytes(2, "big") * 2))
        self.assertEqual(int.from_bytes(nv[0x64:0x68], "big"), 64000)
        self.assertEqual(int.from_bytes(nv[0x68:0x6C], "big"), 71200)
        self.assertEqual(nv[0x63], 0x80)     # 155: band D
        self.assertEqual(nv[0x71], 0x01)     # 151: 12.5 kHz
        self.assertEqual(nv[0x128], 0x02)    # 16 2...: simplex

    def test_block_checksums(self):
        # both copies equal; the band set-up lives in the first block (the
        # cold start fills several others; 0x12C and 0x15FC stay unwritten)
        nv = self.nv
        self.assertEqual(nv[0:0x1680], nv[0x2000:0x3680])
        start, n, cks = NV_BLOCKS[0]
        self.assertEqual(start + n, cks)
        self.assertEqual(sum(nv[start: cks + 1]) & 0xFF, 0xFF)

    def test_service_display_and_synthesizers(self):
        # test 11 tunes 435.000 MHz: physical channel 5600; RX VCO 45 MHz
        # above; TX parked 62.5 kHz off until PTT
        r = service_radio(self.nv)
        ok(r, "11")
        self.assertEqual(r.display()[0], "1     05600 43500000")
        self.assertEqual(r.pll(0)[-1], 480e6)
        self.assertEqual(r.pll(1)[-1], 435.0625e6)
        r.ptt(True)
        r.run(0.5)
        self.assertEqual(r.pll(1)[-1], 435e6)
        self.assertEqual(r.out(1) & 1, 1)                # TX ON
        r.ptt(False)
        r.run(0.5)
        self.assertEqual(r.pll(1)[-1], 435.0625e6)

    def test_up_down_and_rcl(self):
        r = service_radio(self.nv)
        ok(r, "31", wait=1.0)
        self.assertEqual(r.display()[2].split(), ["075", "10"])
        r.press("DOWN", hold=0.3, gap=1.0)
        self.assertEqual(r.display()[2].split(), ["075", "00"])
        r.press("*", hold=0.3, gap=1.0)
        ok(r, "36", wait=1.0)
        r.press("UP", hold=0.3, gap=1.0)
        self.assertEqual(r.display()[2].split(), ["075", "001"])
        # RCL: the next tuning frequency, 1.5 MHz up and then 1 MHz steps
        r.press("RCL", hold=0.3, gap=1.0)
        self.assertEqual(r.display()[0], "1     05840 43650000")
        r.press("RCL", hold=0.3, gap=1.0)
        self.assertEqual(r.display()[0], "1     06000 43750000")

    def test_rx_tuning_dac(self):
        # test 36 steps RFC; the firmware shifts two 6-bit values per
        # select (12 bits) and the DAC keeps the other 12, so channels 1
        # and 3 move together
        r = service_radio(self.nv)
        ok(r, "36", wait=1.0)
        self.assertEqual(r.dac(), [0, 0, 0, 0])
        r.press("UP", hold=0.3, gap=1.0)
        r.press("UP", hold=0.3, gap=1.0)
        self.assertEqual(r.display()[2].split(), ["075", "002"])
        self.assertEqual(r.dac(), [2, 0, 2, 0])

    def test_parameter_programming(self):
        # 70 OK 1234 FNC STO, then simplex channel 1 (parameter 030):
        # RX and TX physical channels and the status, stored on leaving
        r = service_radio(self.nv)
        ok(r, "70", wait=1.0)
        r.type("1234")
        fnc_sto(r)
        self.assertEqual(r.display()[:2], ["700", "Parameters programming"])
        ok(r, "030", wait=1.0)
        for v in ("4886", "4886", "000"):
            r.type(v)
            fnc_sto(r)
        r.press("*", hold=0.3, gap=2.0)
        self.assertEqual(r.display()[1], "rsl")
        nv = r.nv()
        for base in (0x0000, 0x2000):
            self.assertEqual(nv[base + 0x53C: base + 0x542], bytes.fromhex("131613160000"))
            self.assertEqual(nv[base + 0x542: base + 0x546], b"\xff" * 4)  # empty entry
            self.assertEqual(sum(nv[base + 0x488: base + 0xA6E]) & 0xFF, 0xFF)
        self.assertEqual(no_faults(r), [])


class DefaultNV(unittest.TestCase):
    """r40nv.py: the service-mode procedure gives an image that boots clean"""

    @classmethod
    def setUpClass(cls):
        cls.nv = r40nv.default_nv(ROM)

    def test_boots_without_errors(self):
        shown = r40nv.boot_messages(ROM, self.nv)
        self.assertIn(["Self test", "Self test OK", "325555"], shown)
        self.assertFalse([d for d in shown if d[0].startswith("Error")])
        self.assertEqual(shown[-1][:2], ["999_", "Number unobtainable"])

    def test_service_mode_reports_nothing_lost(self):
        r = Radio(ROM, nv=self.nv, service_head=True, power=False)
        r.power_key(True)
        r.power(True)
        shown = []
        while r.time() < 12:
            r.run(0.2)
            if r.time() > 5:
                r.power_key(False)
            shown.append(r.display()[0])
        self.assertFalse([d for d in shown if d.startswith("Error")])
        self.assertEqual(r.display()[0], "1     05600 43500000")

    def test_contents(self):
        nv = self.nv
        for base in (0x0000, 0x2000):
            # simplex channel 1 = 433.500 MHz, RX and TX
            self.assertEqual(nv[base + 0x53C: base + 0x542], bytes.fromhex("14f014f00800"))
            # band calibration after 190002: 430 / 435 / 440 MHz
            self.assertEqual(nv[base + 0x56: base + 0x62],
                             bytes.fromhex("12c012c015e015e019001900"))
            for start, n, cks in NV_BLOCKS:
                if start not in (0x12C, 0x15FC):      # never written
                    self.assertEqual(sum(nv[base + start: base + cks + 1]) & 0xFF, 0xFF,
                                     hex(base + start))

    def test_simplex_channel(self):
        # *55*n# selects parameter record n if its st byte has bit 3 (the
        # check at 0x34B34); #55# leaves
        r = Radio(ROM, nv=self.nv)
        r.run(10)
        for _ in range(4):
            r.press("CLR", hold=0.2, gap=0.3)       # the entry starts with 999
        r.type("*55*30#")
        r.run(2)
        self.assertEqual(r.display()[1:], ["Simplex channel: 030", "Squelch ON  Mode OPEN"])
        self.assertEqual(r.pll(0)[-1], 478.5e6)              # 433.500 + 45 MHz
        r.ptt(True)
        r.run(1)
        self.assertEqual((r.pll(1)[-1], r.out(1) & 1), (433.5e6, 1))
        r.ptt(False)
        r.run(1)
        r.type("#55#")
        r.run(2)
        self.assertEqual(r.display()[1], "Call ended")

    def test_channel_numbers(self):
        self.assertEqual(r40nv.channel(430.5375), 4886)   # PE1BVU's table
        self.assertRaises(ValueError, r40nv.channel, 433.501)


if __name__ == "__main__":
    unittest.main()
