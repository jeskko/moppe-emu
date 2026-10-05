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
        self.assertEqual(d[0], "0 000 00000  00000 0")
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


if __name__ == "__main__":
    unittest.main()
