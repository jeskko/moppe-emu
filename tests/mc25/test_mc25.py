"""
MC25 TVL/PTL scenarios on OH5NXO/OH3NWQ mc25.asm v3.6, built by roms.py
from the reference sources.  Skipped when those (or a 32-bit runtime for
as06) are not available.

    python3 -m unittest discover -s tests/mc25
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, HERE)
import roms  # noqa: E402
from mc25emu import Radio, first_time_setup  # noqa: E402

_nv = {}


def radio(name="mc25-25k", nv=None, **kw):
    try:
        b, lst, step = roms.build(name)
    except roms.Unavailable as e:
        raise unittest.SkipTest(str(e))
    return Radio(b, lst, nv=nv, step_khz=step, **kw)


def sane_nv(name="mc25-25k"):
    """RAM+NOVRAM after the README's first-time procedure (bASEF 14540045,
    TX limits 145.400-146.000, 145.500)."""
    if name not in _nv:
        _nv[name] = first_time_setup(radio(name))
    return _nv[name]


def wait_for(r, cond, timeout=6.0, step=0.05):
    t = r.time() + timeout
    while r.time() < t:
        if cond():
            return True
        r.run(step)
    return False


def faults(r):
    return [e for e in r.events if e[1] in ("WDRESET", "ILLEGAL")]


class Boot(unittest.TestCase):
    def test_display_test_and_banner(self):
        r = radio()
        self.assertTrue(wait_for(r, lambda: r.display() == "8" * 14))   # tube test
        self.assertTrue(wait_for(r, lambda: len(r.leds()) == 8))        # all LEDs
        self.assertTrue(wait_for(r, lambda: r.display().strip() == "3.6"))  # BANNER 3n6
        self.assertEqual(faults(r), [])

    def test_blank_novram_shows_pepa(self):
        # README: an unset EEPROM shows "PEPA", MAN/E/A lit
        r = radio()
        r.run(3)
        self.assertIn("PEPA", r.display())

    def test_ticks_1ms(self):
        r = radio()
        r.run(1.0)
        t = r.L.mc25api_ticks(r.m)
        r.run(1.0)
        self.assertEqual(r.L.mc25api_ticks(r.m) - t, 1000)

    def test_checksum_halt_and_watchdog(self):
        # a corrupted ROM byte: the firmware halts with a 50 % square
        # wave on Q until the watchdog resets it
        r = radio(power=False)
        r.poke(r.sym("tables") + 3, r.peek(r.sym("tables") + 3) ^ 1)
        r.power(True)
        r.run(1.0)
        self.assertTrue(r.take_events("WDRESET"))
        self.assertEqual(r.display().strip(), "")


class Operation(unittest.TestCase):
    def booted(self, name="mc25-25k"):
        r = radio(name, nv=sane_nv(name))
        self.assertTrue(wait_for(r, lambda: "145" in r.display()))
        r.run(0.3)
        return r

    def test_main_screen(self):
        r = self.booted()
        self.assertEqual(r.display(), "0L      145500")
        self.assertEqual(r.leds(), ["ON"])
        self.assertEqual(r.out(2), 4)                       # (145.5 - 145.4) / 25k
        self.assertAlmostEqual(r.rx_hz(), 145.5e6)

    def test_frequency_entry(self):
        r = self.booted()
        r.type("145725#")
        r.run(0.5)
        self.assertEqual(r.display()[-6:], "145725")
        self.assertEqual(r.out(2), 13)
        self.assertAlmostEqual(r.rx_hz(), 145.725e6)
        r.type("512#")             # short form: base's 1st and last two digits
        r.run(0.5)
        self.assertEqual(r.display()[-6:], "145512")
        self.assertTrue(r.out(1) & 0x20)                    # P0 half step
        self.assertAlmostEqual(r.rx_hz(), 145.5125e6)

    def test_12k5_build(self):
        r = self.booted("mc25-12k")
        r.type("145600#")
        r.run(0.5)
        self.assertEqual(r.out(2), 16)                      # 12.5 kHz units
        self.assertAlmostEqual(r.rx_hz(), 145.6e6)

    def test_ptt_on_hook_sends_1750(self):
        # handset on the hook (cradle switch pressed) + PTT: CCIR tone 8
        # (1747 Hz, repeater access) instead of the mic
        r = self.booted()
        r.set_ptt(True)
        r.run(0.3)
        self.assertTrue(r.tx())
        self.assertEqual(r.out(5) & 0xf8, 0x80)
        r.set_ptt(False)
        r.run(0.3)
        self.assertFalse(r.tx())

    def test_ptt(self):
        r = self.booted()
        r.set_hook(True)
        r.run(0.3)
        r.set_ptt(True)
        r.run(0.3)
        self.assertTrue(r.tx())
        self.assertTrue(r.out(5) & 0x08)                    # MIC on
        r.set_ptt(False)
        r.run(0.3)
        self.assertFalse(r.tx())
        self.assertEqual([e[1] for e in r.take_events() if e[1].startswith("TX")],
                         ["TX_ON", "TX_OFF"])

    def test_tx_limits(self):
        r = self.booted()
        r.type("147000#")
        r.run(0.5)
        r.set_ptt(True)
        r.run(0.3)
        self.assertFalse(r.tx())

    def test_squelch_busy_led_and_volume(self):
        r = self.booted()
        r.set_squelch(True)
        r.run(0.4)
        self.assertIn("BUSY", r.leds())
        r.set_squelch(False)
        r.run(0.4)
        self.assertNotIn("BUSY", r.leds())
        r.type("++")
        r.run(0.4)
        self.assertEqual(r.display()[0], "2")
        self.assertEqual(r.out(3) & 0x2f, 0x21)             # voltbl[2]

    def test_ccir_send(self):
        r = self.booted()
        r.take_events()
        r.type("12345*")
        r.run(1.5)
        tones = [(t, a) for t, n, a in r.take_events() if n == "CCIR_TX"]
        self.assertEqual([a for _, a in tones], [1, 2, 3, 4, 5, 0xF])
        for (t0, _), (t1, _) in zip(tones, tones[1:]):
            self.assertAlmostEqual(t1 - t0, 0.100, delta=0.002)   # BEEPTICKS

    def test_ccir_receive(self):
        r = self.booted()
        r.ccir_rx("12345")
        r.run(1.5)
        self.assertEqual(len(r.take_events("CCIR_ACK")), 5)
        self.assertEqual(r.peek("ev_ccirlen"), 5)
        self.assertEqual(r.read("ev_ccirbuf", 5), bytes([1, 2, 3, 4, 5]))

    def test_serial_keys_and_enq(self):
        r = self.booted()
        r.serial_keys("145525#")
        r.run(0.6)
        self.assertEqual(r.display()[-6:], "145525")
        r.serial_rx(b"\0\5")                                # ENQ: report
        r.run(1.0)
        self.assertEqual(r.serial_tx(), b"800L      145525\n")

    def test_lock_enters_setup(self):
        r = self.booted()
        r.button("lock")
        r.run(0.5)
        self.assertEqual(r.display().strip(), "SEtUP")

    def test_nv_survives_power_cycle(self):
        r = self.booted()
        r.type("145650#")
        r.run(0.5)
        r.power(False)
        r.run(0.2)
        r.power(True)
        self.assertTrue(wait_for(r, lambda: r.display()[-6:] == "145650"))
        self.assertEqual(faults(r), [])


if __name__ == "__main__":
    unittest.main()
