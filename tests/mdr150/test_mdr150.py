"""
MDR150 scenarios: OH5NXO's bootstrap and HaMDR 174 (tests/mdr150/roms.py)
on the emulated board.  The console, the configuration saved to flash,
APRS out (the beacon decoded from the PWM audio), APRS in (AFSK into the
receiver, decoded by HaMDR's software modem), digipeating, and KISS both
ways.

    python3 -m unittest discover -s tests/mdr150       # or: make test-mdr150
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, HERE)

import afsk        # noqa: E402
import roms        # noqa: E402

try:
    IMAGE = roms.flash_image()
except roms.Unavailable as e:
    IMAGE = None
    WHY = str(e)

if IMAGE is not None:
    from mdr150emu import Radio   # noqa: E402

# a digipeater's settings (reference/mdr150/mdr150_digipeater_configuration.txt)
DIGI = ["personality tracker", "freq 144.800", "mycall OH3RDX", "use_softdcd on",
        "tracker_digi wide", "gps_lat 6141.21,N", "gps_lon 02444.64,E",
        "mysymbol /#", "ttylines 0"]
KISS = ["personality kiss", "freq 144.800", "mycall OH3RDR", "use_softdcd on",
        "ttylines 0"]


def boot(img=IMAGE):
    r = Radio(img)
    r.run(0.6)
    return r


def cmd(r, line, t=0.5):
    r.send(line + "\r")
    r.run(t)
    out = r.serial_text()
    while out.rstrip().endswith("-More-"):
        r.send(" ")
        r.run(t)
        out += r.serial_text()
    return out


def configured(lines):
    """flash image with the settings saved by 'perm' on the console"""
    r = boot()
    r.serial_text()
    for line in lines:
        cmd(r, line)
    out = cmd(r, "perm", 1.0)
    assert "OK" in out, out
    return r.flash()


def kiss(frame):
    esc = frame.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
    return b"\xc0\x00" + esc + b"\xc0"


@unittest.skipIf(IMAGE is None, IMAGE is None and WHY)
class Console(unittest.TestCase):
    def test_boot_prompt(self):
        r = boot()
        out = r.serial_text()
        self.assertIn("Some parameters have defaulted", out)   # erased configuration
        self.assertTrue(out.rstrip().endswith("hamdr $"), out)
        self.assertAlmostEqual(r.baud(), 19418, delta=5)       # SCBR 27 at 16.777 MHz

    def test_version(self):
        r = boot()
        r.serial_text()
        self.assertIn("174", cmd(r, "version"))

    def test_perm_survives_reboot(self):
        img = configured(["mycall OH0EMU-7"])
        r = boot(img)
        self.assertNotIn("defaulted", r.serial_text())
        r.send("\r")                     # no DTR: personality Voice runs the console
        r.run(0.3)
        self.assertIn("OH0EMU-7", cmd(r, "disp"))

    def test_dtr_selects_command_mode(self):
        """with valid parameters, HaMDR runs its command mode (echo,
        prompt) only if DTR on port 1 is on when it starts; without,
        commands still run, silently.  DTR rises 50 ms after power-on,
        past the bootstrap's own check (which would wait for S-records)"""
        img = configured(["factorydefaults"])
        outs = []
        for dtr in (False, True):
            r = Radio(img, power=False)
            r.power(True)
            r.run(0.05)
            r.lines(0, dtr=dtr)
            r.run(1.0)
            r.serial_text()
            outs.append(cmd(r, "version"))
        self.assertNotIn("hamdr $", outs[0])
        self.assertIn("HaMDR 174", outs[0])
        self.assertTrue(outs[1].startswith("version\r\n"), outs[1])     # echoed
        self.assertTrue(outs[1].rstrip().endswith("hamdr $"), outs[1])

    def test_dtr_stops_operation(self):
        """the digipeater configuration does not beacon in command mode"""
        img = configured(DIGI)
        r = Radio(img, power=False)
        r.power(True)
        r.run(0.05)
        r.lines(0, dtr=True)
        r.run(4)
        self.assertEqual(r.transmitted(), [])
        self.assertTrue(r.serial_text().rstrip().endswith("hamdr $"))


@unittest.skipIf(IMAGE is None, IMAGE is None and WHY)
class Aprs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = configured(DIGI)

    def test_beacon(self):
        """the startup report goes out as AFSK from PWMA, on 144.800"""
        r = boot(self.img)
        r.run(4)
        self.assertEqual(r.transmitted(), ["OH3RDX>APZMDR:!6141.21N/02444.64E#"])
        ev = r.take_events()
        on = [e for e in ev if e[1] == "TX_ON"]
        self.assertEqual(len(on), 1)
        # the synthesizer was set to the channel (N*64+A at 6.25 kHz) before keying
        na = [e[2] for e in ev if e[1] == "PLL" and e[2] >= 0 and e[0] < on[0][0]]
        self.assertEqual(na[-1] * 6250, 144800000)
        # receiving: the VCO 21.4 MHz below the channel
        self.assertEqual(r.radio()["vco_hz"], 123.4e6)

    def test_receive_and_digipeat(self):
        r = boot(self.img)
        r.run(4)
        r.transmitted()
        r.serial_text()
        r.receive("OH5ABC-9>APRS,WIDE1-1:!6140.00N/02445.00E>near", after=0.5)
        self.assertEqual(r.serial_text(), "OH5ABC-9>APRS,WIDE1-1:!6140.00N/02445.00E>near\r\n")
        r.run(4)
        self.assertEqual(r.transmitted(), ["OH5ABC-9>APRS,OH3RDX*:!6140.00N/02445.00E>near"])

    def test_distant_not_digipeated(self):
        """digi_area (10 km by default) keeps far stations off the air"""
        r = boot(self.img)
        r.run(4)
        r.transmitted()
        r.receive("OH5ABC-9>APRS,WIDE1-1:!6130.00N/02500.00E>far", after=4)
        self.assertEqual(r.transmitted(), [])

    def test_bad_fcs_ignored(self):
        """one data bit flipped in the frame: HaMDR drops it"""
        r = boot(self.img)
        r.run(4)
        r.serial_text()
        bits = afsk.hdlc_bits(afsk.ax25_frame("OH5ABC-9>APRS:>broken"), preamble=40)
        bits[40 * 8 + 120] ^= 1
        x = afsk.modulate(afsk.nrzi_encode(bits), amp=0.9)
        r.rx_audio([0.0] * 2400 + x)
        r.run(len(x) / 48000 + 0.5)
        self.assertEqual(r.serial_text(), "")


@unittest.skipIf(IMAGE is None, IMAGE is None and WHY)
class Kiss(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = configured(KISS)

    def test_kiss_to_air(self):
        r = boot(self.img)
        r.send(kiss(afsk.ax25_frame("OH3RDR>APRS:>kiss test")))
        r.run(3)
        self.assertEqual(r.transmitted(), ["OH3RDR>APRS:>kiss test"])

    def test_air_to_kiss(self):
        r = boot(self.img)
        r.serial_text()
        r.receive("OH5ABC>APRS,WIDE2-2:>hello kiss", after=0.5)
        self.assertEqual(r.serial_text().encode("latin-1"),
                         kiss(afsk.ax25_frame("OH5ABC>APRS,WIDE2-2:>hello kiss")))


@unittest.skipIf(IMAGE is None, IMAGE is None and WHY)
class Tui(unittest.TestCase):
    """the terminal UI's session, headless: preset, packets on the air,
    KISS and transmitted frames in its AX.25 log"""

    def test_digi_preset(self):
        import mdr150tui as T

        class Args:
            flash = None
            preset = "digi"
            rssi, audio, noise, busy = 600, 300, 150, False
        s = T.Session(Args())
        s.run(3)
        s.canned()
        s.canned(far=True)
        s.run(9)
        log = [(tag, text) for at, tag, text in s.log]
        self.assertIn(("<rf", "OH3RDX>APZMDR:!6141.21N/02444.64E#"), log)
        self.assertIn(("rf>", "OH5ABC-9>APRS,WIDE1-1:!6140.00N/02445.00E>near station #1"), log)
        self.assertIn(("<rf", "OH5ABC-9>APRS,OH3RDX*:!6140.00N/02445.00E>near station #1"), log)
        self.assertEqual([t for tag, t in log if tag == "<rf"],
                         ["OH3RDX>APZMDR:!6141.21N/02444.64E#",
                          "OH5ABC-9>APRS,OH3RDX*:!6140.00N/02445.00E>near station #1"])
        self.assertIn("RX 144.8000 MHz", "\n".join(s.state_lines()))

    def test_kiss_preset(self):
        import mdr150tui as T

        class Args:
            flash = None
            preset = "kiss"
            rssi, audio, noise, busy = 600, 300, 150, False
        s = T.Session(Args())
        s.run(1)
        s.send_kiss("OH3RDR>APRS:>from the host")
        s.send_rf("OH5ABC>APRS:>to the host")
        s.run(6)
        log = [(tag, text) for at, tag, text in s.log]
        self.assertIn(("<rf", "OH3RDR>APRS:>from the host"), log)
        self.assertIn(("<kiss", "OH5ABC>APRS:>to the host"), log)


if __name__ == "__main__":
    unittest.main()
