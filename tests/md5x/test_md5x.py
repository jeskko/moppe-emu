"""
Talkman MD50/MD59/ME59 scenarios on the OH3NWQ (mx5x.asm v3.183) and OH1E
(md50.asm #42) firmware, built by roms.py from the reference sources.
Skipped when those (or a 32-bit runtime for as06) are not available.

    python3 -m unittest discover -s tests/md5x
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, HERE)
import roms  # noqa: E402
from md5xemu import Radio, KEYS  # noqa: E402


def radio(name, **kw):
    try:
        b, lst, model, cu = roms.build(name)
    except roms.Unavailable as e:
        raise unittest.SkipTest(str(e))
    return Radio(b, lst, model=model, cu=cu, **kw)


def faults(r):
    return [e for e in r.events if e[1] in ("WDRESET", "ILLEGAL")]


class Mx5x(unittest.TestCase):
    """OH3NWQ firmware: polled (no interrupts), timing loops."""

    def booted(self, name):
        r = radio(name)
        r.run(2.5)
        return r

    def test_boot_screens(self):
        for name, main in (("mx5x-md50", "400000"), ("mx5x-md59", "400000"),
                           ("mx5x-me59", "1200000")):
            with self.subTest(name):
                r = radio(name)
                r.run(0.5)
                up, lo = r.display()
                self.assertTrue(up.startswith("3183"), up)     # VNUMBER 3.183
                self.assertTrue(lo.endswith("020416"), lo)     # DATE
                r.run(2.0)
                self.assertEqual(r.display()[1].strip(), main)
                self.assertEqual(faults(r), [])

    def test_boot_synth(self):
        # zeroed RAM: channel 0 = 400.000 / 1200.000 MHz
        r = self.booted("mx5x-md59")
        s = r.synth()
        self.assertEqual((s["n"], s["a"], s["nbits"]), (210, 56, 19))   # (400+21.4)/25k = 210*80+56
        self.assertAlmostEqual(r.rx_hz(), 400.0e6)
        r = self.booted("mx5x-me59")
        s = r.synth()
        self.assertEqual(s["div"], (12000000 - 810000) // 125)          # BASEDIV
        self.assertAlmostEqual(r.rx_hz(), 1200.0e6)
        self.assertEqual((s["off_n"], s["off_a"]), (25, 11))            # 81.1 MHz RX offset osc

    def test_keycodes(self):
        # every key reaches the firmware's keycode with the raw code
        for name in ("mx5x-md50", "mx5x-md59"):
            r = self.booted(name)
            for k, code in KEYS[r.cu].items():
                if k in "PB*#XD+-SRCE":
                    continue        # commands; digits are enough to check codes
                with self.subTest(name=name, key=k):
                    r.key(k)
                    r.run(0.1)
                    self.assertEqual(r.peek("keycode"), code)
                    r.key(None)
                    r.run(0.2)
                    r.press("C")    # clear the digit buffer

    def test_frequency_entry(self):
        r = self.booted("mx5x-md59")
        r.type("33500")             # 5 digits; the 100 MHz digit is implied
        r.run(0.5)
        self.assertEqual(r.display()[1][-6:], "433500")
        self.assertAlmostEqual(r.rx_hz(), 433.5e6)

    def test_ptt_tx_offset(self):
        r = self.booted("mx5x-md59")
        r.type("33500")
        r.run(0.5)
        r.set_ptt(True)
        r.run(0.3)
        self.assertTrue(r.tx())
        self.assertEqual(len(r.take_events("TX_ON")), 1)
        self.assertAlmostEqual(r.vco_hz(), 443.5e6 + 21.4e6)   # X314: +10 MHz NMT shift
        r.set_ptt(False)
        r.run(0.3)
        self.assertFalse(r.tx())
        self.assertAlmostEqual(r.rx_hz(), 433.5e6)

    def test_call_tone(self):
        # '*' sends 1750 Hz from a cycle-counted PHI loop: 132 machine
        # cycles per half period at 3.6864 MHz, 171 at 4.8 MHz
        for name, hz in (("mx5x-md59", 3686400 / 8 / 264), ("mx5x-me59", 4800000 / 8 / 342)):
            with self.subTest(name):
                r = self.booted(name)
                r.type("96500" if name == "mx5x-me59" else "33500")   # in band
                r.run(0.5)
                r.audio_capture(True)
                r.key("*")
                r.run(0.5)
                self.assertTrue(r.tx())
                r.key(None)
                r.run(0.3)
                self.assertFalse(r.tx())
                self.assertAlmostEqual(r.tone_hz(), hz, delta=hz * 0.003)

    def test_squelch(self):
        r = self.booted("mx5x-md59")
        r.set_squelch(True)
        r.run(0.3)
        self.assertTrue(r.leds() & 0x04)        # BIT_SERV
        self.assertTrue(r.latch(1) & 0x02)      # EAR
        r.set_squelch(False)
        r.run(0.3)
        self.assertFalse(r.leds() & 0x04)
        self.assertFalse(r.latch(1) & 0x02)

    def test_power_switch(self):
        r = self.booted("mx5x-md59")
        r.set_input("poweroff", True)
        r.run(0.5)
        self.assertEqual(r.display()[1], "BYE-BYE 73")
        r.run(3.0)
        self.assertFalse(r.powered())
        self.assertEqual(len(r.take_events("POWEROFF")), 1)

    def test_watchdog(self):
        r = self.booted("mx5x-md59")
        top = r.sym("top")
        r.poke(top, 0x30)           # BR top: a loop without FLIP(WDR)
        r.poke(top + 1, top & 0xff)
        r.run(0.5)
        self.assertTrue(r.take_events("WDRESET"))

    def test_battery_ram_survives_power_cycle(self):
        r = self.booted("mx5x-md59")
        r.type("33500")
        r.run(0.5)
        r.power(False)
        r.run(0.1)
        r.power(True)
        r.run(2.5)
        self.assertEqual(r.display()[1][-6:], "433500")


class Oh1e(unittest.TestCase):
    """OH1E firmware: 100 Hz modem timer interrupt, IDL, R0 tick count."""

    def booted(self, name):
        r = radio(name)
        r.run(2.5)
        s = r.syms
        # setup defaults for the TX band limits (zeroed RAM forbids TX)
        r.write("tx_start", s["TX_START"].to_bytes(2, "little"))
        r.write("tx_end", s["TX_END"].to_bytes(2, "little"))
        if r.cu == "CU59":
            r.press("1")            # first key: handset type autodetect
        return r

    def test_timer_interrupt(self):
        for name in ("oh1e-md50", "oh1e-md59", "oh1e-me59"):
            with self.subTest(name):
                r = radio(name)
                r.run(1.0)
                t0 = r.cpu()["r"][0] & 0xff     # Rtim: 10 ms ticks
                r.run(1.0)
                self.assertEqual(((r.cpu()["r"][0] & 0xff) - t0) & 0xff, 100)
                self.assertEqual(faults(r), [])

    def test_entry_and_tx(self):
        for name, digits, f in (("oh1e-md50", "33500", 433.5e6),
                                ("oh1e-md59", "33500", 433.5e6),
                                ("oh1e-me59", "96500", 1296.5e6)):
            with self.subTest(name):
                r = self.booted(name)
                r.type(digits + "#")
                r.run(1.0)
                self.assertAlmostEqual(r.rx_hz(), f)
                r.set_ptt(True)
                r.run(0.5)
                self.assertTrue(r.tx())
                r.set_ptt(False)
                r.run(0.5)
                self.assertFalse(r.tx())        # txoff waited for TOFF
                self.assertEqual([e[1] for e in r.take_events() if e[1].startswith("TX")],
                                 ["TX_ON", "TX_OFF"])
                self.assertAlmostEqual(r.rx_hz(), f)
                self.assertEqual(faults(r), [])

    def test_tx_refused_out_of_band(self):
        r = self.booted("oh1e-me59")
        r.type("29650#")            # 1229.650: below the 1240 MHz band edge
        r.run(1.0)
        r.set_ptt(True)
        r.run(0.5)
        self.assertFalse(r.tx())


if __name__ == "__main__":
    unittest.main()
