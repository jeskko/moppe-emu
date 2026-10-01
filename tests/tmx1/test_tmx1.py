"""
TMF-1 / TMN-1 scenarios on OH5NXO / OH3NWQ tmx1.asm v5.0 with the HSN-2
v1.6 and HSF-2 v0.2 handset firmware, both CPUs emulated and linked by
MBUS.  Skips when the reference zip or a 32-bit runtime is missing
(roms.py).
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))

import roms  # noqa: E402

try:
    BUILT = {n: roms.build(n) for n in roms.BUILDS}
    SKIP = None
except roms.Unavailable as e:
    BUILT, SKIP = {}, str(e)

from tmx1emu import Radio  # noqa: E402

_ram = {}


def radio(band="tmf1", hs="HSN2", boot=True):
    """A unit past its first-boot defaults reset, at the main screen
    (boot=False: fresh RAM, at power-on)."""
    rom, lst = BUILT[band]
    hs_rom = BUILT[hs.lower()][0] if hs else None
    key = (band, hs)
    r = Radio(rom, lst, hs_rom=hs_rom, handset=hs or "HSN2",
              ram=_ram.get(key) if boot else None, power=True)
    if boot:
        r.run(6.0)
        _ram.setdefault(key, r.ram())
        r.take_events()
    return r


def packets(r):
    return r.mbus_packets(r.mbus_bytes())


@unittest.skipIf(SKIP, SKIP)
class Boot(unittest.TestCase):

    def test_first_boot_tmf1(self):
        r = radio(boot=False)
        r.run(0.3)
        self.assertEqual(r.display()[:2], ("HSN2 1.6", "14.03.00"))
        r.run(1.2)
        self.assertEqual(r.display()[:2], ("Settings", "insane  "))
        r.run(2.5)
        self.assertEqual(r.display(), ("TMF1 5.0", "18.06.06", "99"))
        r.run(2.0)
        self.assertEqual(r.display(), ("    30 2", "  433500", " 0"))
        self.assertEqual(r.leds(), {"ON", "BACKLIGHT"})
        # RX VCO = 433.5 MHz + 21.4 MHz IF, 25 kHz reference
        self.assertEqual(r.pll("rx")["r"], 512)
        self.assertAlmostEqual(r.vco_hz(), 454.9e6, delta=1)
        self.assertFalse(r.take_events("ILLEGAL"))
        self.assertFalse(r.take_events("WDNMI"))

    def test_mbus_traffic(self):
        r = radio(boot=False)
        r.run(6.0)
        b = r.mbus_bytes()
        self.assertTrue(len(b) > 100)
        self.assertFalse([x for x in b if x[2]], "parity / framing errors")
        p = r.mbus_packets(b)
        self.assertIn(("ANY1", "TMx1", b"!", 1), p)
        self.assertIn(("HSN2", "TMF1", b"f  433500", 1), p)

    def test_tmn1(self):
        r = radio("tmn1")
        self.assertEqual(r.display()[1], " 1297500")
        self.assertEqual(r.pll("rx")["r"], 1024)	# 12.5 kHz steps

    def test_hsf2(self):
        r = radio(hs="HSF2", boot=False)
        r.run(0.3)
        self.assertEqual(r.display(), ("HSF2 0.2", "10.04.00", "--"))
        r.run(5.7)
        self.assertEqual(r.display()[:2], ("    30 2", "  433500"))
        r.type("433550#")
        r.run(0.5)
        self.assertEqual(r.display()[1], "  433550")
        self.assertAlmostEqual(r.vco_hz(), 454.95e6, delta=1)

    def test_battery_ram(self):
        r = radio()
        r.type("433550#")
        r.run(0.5)
        img = r.ram()
        r2 = radio(boot=False)
        r2.ram(img)
        r2.power(False)
        r2.power(True)
        r2.run(6.0)
        self.assertEqual(r2.display()[1], "  433550")
        self.assertFalse(any(b"insane" in p[2] for p in packets(r2)))


@unittest.skipIf(SKIP, SKIP)
class Keys(unittest.TestCase):

    def test_frequency_entry(self):
        r = radio()
        r.type("43")
        self.assertEqual(r.display()[1], "43      ")
        r.type("3550#")
        r.run(0.3)
        self.assertEqual(r.display()[1], "  433550")
        self.assertAlmostEqual(r.vco_hz(), 454.95e6, delta=1)
        # each key: make and break from the handset, an echo of the buffer back
        p = packets(r)
        self.assertIn(("ANY1", "HSN2", b"M#", 2), p)
        self.assertIn(("ANY1", "HSN2", b"B#", 2), p)

    def test_icons(self):
        r = radio()
        r.press("F")
        r.run(0.2)
        self.assertEqual(r.icons(), {"FCN"})
        r.press("A")
        r.run(0.2)
        self.assertEqual(r.icons(), {"ALPHA"})

    def test_keyclick_tone(self):
        r = radio()
        r.press("5")
        codes = [a for _, _, a in r.take_events("DTMF")]
        self.assertEqual(codes[:2], [0x3F, 0x00])	# click: 1750 tone on and off

    def test_host_as_handset(self):
        r = radio(hs=None)
        r.type("434000#", hold=0.1, gap=0.1)	# make / break packets from the host
        r.run(0.5)
        self.assertIn(("HSN2", "TMF1", b"f  434000", 1), packets(r))
        self.assertAlmostEqual(r.vco_hz(), 455.4e6, delta=1)


@unittest.skipIf(SKIP, SKIP)
class Radio_(unittest.TestCase):

    def test_squelch(self):
        r = radio()
        self.assertFalse(r.audio_open())
        r.set_rssi(200)
        r.run(1.0)
        self.assertTrue(r.audio_open())
        self.assertEqual(r.display()[2], "87")	# (200 - 30) * 100 / 195
        self.assertIn("SERV", r.leds())
        r.set_rssi(0)
        r.run(1.0)
        self.assertFalse(r.audio_open())

    def test_ptt(self):
        r = radio()
        r.set_ptt(True)
        r.run(0.6)
        self.assertTrue(r.tx())
        self.assertAlmostEqual(r.tx_hz(), 433.5e6, delta=1)
        self.assertIn("ROAM", r.leds())		# ON-AIR
        kinds = [e[1] for e in r.events]
        self.assertLess(kinds.index("SYNTH"), kinds.index("TX_ON"))
        r.set_ptt(False)
        r.run(0.6)
        self.assertFalse(r.tx())
        self.assertTrue(r.take_events("TX_OFF"))

    def test_ptt_key(self):
        r = radio()
        r.press("P")
        r.run(0.4)
        self.assertTrue(r.tx())
        r.press("P")
        r.run(0.4)
        self.assertFalse(r.tx())

    def test_ctcss(self):
        r = radio()
        r.poke("ctcss_tone", 123)
        r.audio_capture(True)
        t0 = r.time()
        r.set_ptt(True)
        r.run(1.0)
        # 921.6 kHz / 7492 from i8253 counter 2 (the CLK2 mod)
        self.assertAlmostEqual(r.tone_hz(t0 + 0.3), 921600 / 7492, delta=0.05)

    def test_power_key(self):
        r = radio()
        r.power_key(True)
        r.run(0.4)
        r.power_key(False)
        r.run(3.0)
        self.assertFalse(r.powered())
        self.assertTrue(r.take_events("POWEROFF"))
        self.assertIn(("HSN2", "TMF1", b"d73 Bye !", 1), packets(r))
        r.power_key(True)
        r.run(0.3)
        r.power_key(False)
        r.run(6.0)
        self.assertTrue(r.powered())
        self.assertEqual(r.display()[1], "  433500")

    def test_watchdog(self):
        r = radio()
        a = r.sym("mainloop")
        r.poke(a, 0xBA)		# DI
        r.poke(a + 1, 0xFF)	# JR .
        r.run(1.0)
        nmi = r.take_events("WDNMI")
        self.assertEqual(len(nmi), 1)
        # systick is the only WDC pulse: 400 ms after the last one
        self.assertTrue(nmi[0][2] in (a + 1, a + 2))

    def test_watchdog_power_off(self):
        r = radio()
        a = r.sym("mainloop")
        r.poke(a, 0xBA)		# DI
        r.poke(a + 1, 0xFF)	# JR .
        r.poke(0x0004, 0xFF)	# NMI vector: JR . too, so it never recovers
        r.run(13.0)
        (t_nmi, _, _), = r.take_events("WDNMI")
        (t_off, _, _), = r.take_events("POWEROFF")
        self.assertAlmostEqual(t_off - t_nmi, 12.0, delta=0.01)
        self.assertFalse(r.powered())


if __name__ == "__main__":
    unittest.main()
