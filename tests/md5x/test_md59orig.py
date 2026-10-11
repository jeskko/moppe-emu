"""
Original Nokia MD59 NMT-450 firmware (OH5NXO's 1997 dump, fetched by
`make refs` or read from the firmware repo's reference/): 93C06 ID
EEPROM, the OK 59 / TK 59 programming module, LOCAL-mode programming as
in the ME59 service manual, and the calling-channel scan in NMT mode.
Skipped when the dump is not there.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))

from md5xemu import Radio  # noqa: E402
import nmtframe as nf  # noqa: E402

REL = os.path.join("oh5nxo", "mods", "Mx5x", "orig", "MD59SB.raw.orig")
CANDS = [os.path.join(HERE, "..", "..", "reference", REL),
         os.path.join(HERE, "..", "..", "..", "reference", REL)]
ROM = next((c for c in CANDS if os.path.isfile(c)), None)

# CU53AN glyph bits: 0 f, 1 e, 2 c, 3 b, 4 a, 5 g, 6 d
SEG = {0x5F: "0", 0x0C: "1", 0x7A: "2", 0x7C: "3", 0x2D: "4", 0x75: "5",
       0x77: "6", 0x1C: "7", 0x7F: "8", 0x7D: "9", 0x73: "E", 0x22: "r",
       0x66: "o", 0x43: "L", 0x53: "C", 0x3F: "A", 0x00: " "}

# NMT-450 channel n: LO = 463.000 MHz + (n-1) * 25 kHz + 21.4 MHz IF
CH1_DIV = 19376

# programmed as in test_program_and_scan: Z = 8, number 123456
EEPROM = [0x8123, 0x456F, 0x1234, 0, 0x111A, 0x26FF] + [0xFFFF] * 9 + [5340]
# osmocom-analog's idle additional info (no battery saving, group 8)
IDLE_INFO = [0xE, 0xB, 0, 0, 0, 2, 8, 0, 0, 0]


def base_station(r, channel, prefix, ta):
    """queue idle frames on channel; returns a feeder to call while running"""
    r.air_channel(CH1_DIV + channel - 1)
    bits = nf.air_bits(nf.frame(channel, prefix, [ta >> 4, ta & 15] + IDLE_INFO))

    def feed():
        while r.air_buffered() < 20000:
            r.air_push(bits)
    return feed


def decode_tx(data):
    """frames in the modem's TX bytes (LSB first): list of digit strings"""
    bits = [(b >> k) & 1 for b in data for k in range(8)]
    s = "".join(map(str, bits))
    sync = "".join(map(str, nf.BIT_SYNC[-4:] + nf.FRAME_SYNC))
    out, i = [], s.find(sync)
    while i >= 0 and i + len(sync) + 140 <= len(bits):
        coded = bits[i + len(sync):i + len(sync) + 140]
        out.append("".join("%X" % d for d in
                           nf.bits_to_digits(nf.hagelbarger_decode(coded))))
        i = s.find(sync, i + len(sync) + 140)
    return out


def lower(r):
    return "".join(SEG.get(g, "?") for g in r.glyphs()[1]).strip()


def upper(r):
    return "".join(SEG.get(g, "?") for g in r.glyphs()[0]).strip()


def local_radio(module=True):
    r = Radio(ROM, model="MD59", cu="CU59", power=False)
    r.set_input("local", 1)
    if module:
        r.progmodule()
    r.power(True)
    r.run(3.0)
    return r


def cmd(r, keys):
    r.type(keys)
    r.run(0.8)
    return lower(r)


@unittest.skipUnless(ROM, "original MD59 dump not found (make refs)")
class TestMD59Original(unittest.TestCase):

    def test_unprogrammed_powers_off(self):
        r = Radio(ROM, model="MD59", cu="CU59")
        r.run(3.0)
        self.assertIn("POWEROFF", [e[1] for e in r.take_events()])

    def test_local_error10(self):
        r = local_radio(module=False)
        self.assertEqual(upper(r), "L0CAL")
        self.assertEqual(lower(r), "Error 10")      # EEPROM unprogrammed

    def test_no_rights_without_module(self):
        r = local_radio(module=False)
        cmd(r, "S")
        self.assertEqual(cmd(r, "72#"), "Error 11")

    def test_program_and_scan(self):
        r = local_radio()
        for c, data in (("71#", "111026"), ("72#", "8123456"), ("73#", "1234")):
            cmd(r, "S")
            cmd(r, c)
            self.assertEqual(cmd(r, data), data)
            cmd(r, "S")
        ee = r.eeprom()
        self.assertEqual(ee[0:3], [0x8123, 0x456F, 0x1234])
        cmd(r, "82#")
        self.assertEqual(lower(r), "8123456")
        cmd(r, "S")

        # normal power-on with that EEPROM: stays on, scans channels
        n = Radio(ROM, model="MD59", cu="CU59", power=False)
        n.eeprom(ee)
        n.power(True)
        n.run(6.0)              # scanning from about 5 s
        n.take_events()
        n.run(2.0)
        ev = n.take_events()
        self.assertNotIn("POWEROFF", [e[1] for e in ev])
        self.assertGreater(len([e for e in ev if e[1] == "SYNTH"]), 100)
        ch = n.synth()["div"] - CH1_DIV + 1
        self.assertTrue(1 <= ch <= 200, ch)


    def run_bs(self, prefix, ta, seconds):
        r = Radio(ROM, model="MD59", cu="CU59", power=False)
        r.eeprom(EEPROM)
        feed = base_station(r, 1, prefix, ta)
        r.power(True)
        ev = []
        while r.time() < seconds:
            feed()
            r.run(0.5)
            ev += r.take_events()
        return r, ev

    def test_camps_on_home_area(self):
        # cold start: country 5, area 0 (0x83F4); a calling channel of
        # area 50 is taken at once and kept (every frame decoded)
        r, ev = self.run_bs(nf.P_1A, 0x50, 12.0)
        self.assertEqual(r.synth()["div"], CH1_DIV)
        self.assertIn("SERV", r.led_names())
        late = [e for e in ev if e[1] == "SYNTH" and e[0] > 6.0]
        self.assertEqual(late, [])

    def test_other_country_not_taken(self):
        r, ev = self.run_bs(nf.P_1A, 0x81, 12.0)
        self.assertNotIn("SERV", r.led_names())
        self.assertNotIn("TX_ON", [e[1] for e in ev])

    def test_roaming_seizure(self):
        # new area 51 on a combined calling/traffic channel: after two scan
        # rounds the phone sends roaming updating seizures (11a), gets no
        # answer and lights ROAM
        r, ev = self.run_bs(nf.P_1B, 0x51, 20.0)
        self.assertIn("TX_ON", [e[1] for e in ev])
        frames = decode_tx(bytes(e[2] for e in ev if e[1] == "MODEM_TX"))
        self.assertTrue(frames)
        # N=001 P=E(11a) Z=8 X=123456 T=0 Y=0 K=000
        self.assertEqual(frames[0], "001E812345600000")
        self.assertIn("ROAM", r.led_names())
        self.assertIn("SERV", r.led_names())


if __name__ == "__main__":
    unittest.main()
