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
    return decode_tx_bits([(b >> k) & 1 for b in data for k in range(8)])


def decode_tx_bits(bits):
    bits = [b for b in bits if b in (0, 1)]
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



def md50_image():
    """the MD50 original, na0 at 0000 and na1 at 4000, each 8 KB mirrored
    in its 16 KB window; None if the dumps are not there"""
    for c in CANDS:
        d = os.path.dirname(c)
        a, b = os.path.join(d, "na0"), os.path.join(d, "na1")
        if os.path.isfile(a) and os.path.isfile(b):
            x, y = open(a, "rb").read(), open(b, "rb").read()
            return x + x + y + y
    return None


@unittest.skipUnless(md50_image(), "original MD50 dumps not found (make refs)")
class TestMD50Original(unittest.TestCase):

    def radio(self, prom):
        import tempfile
        img = md50_image()
        f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        f.write(img)
        f.close()
        self.addCleanup(os.unlink, f.name)
        r = Radio(f.name, model="MD50", cu="CU53", power=False)
        r.prom(prom)
        r.power(True)
        r.run(4.0)
        return r

    def test_id_prom_last_page(self):
        """the last page whose first nibble is not 0 is the number (page
        select from latch 7 bits 1-3 and latch 6 bit 1)"""
        p0 = [8, 1, 2, 3, 4, 5, 6, 1, 2, 3, 4, 0, 0, 0, 0, 0]
        p1 = [9, 7, 6, 5, 4, 3, 2, 4, 3, 2, 1, 0, 0, 0, 0, 0]
        r = self.radio(p0 + p1)
        self.assertEqual(list(r.read(0x83F6, 7)), p1[:7])
        self.assertEqual(list(r.read(0x83D0, 4)), [4, 3, 2, 1])
        r = self.radio(p0)
        self.assertEqual(list(r.read(0x83F6, 7)), p0[:7])

    def test_na1_checksum_short(self):
        """the firmware's ROM test (0x56BE) sums 0000-1FFF and 4000-5EFF
        against 5FF0-5FF5; the na1 dump is 0x33 short"""
        img = md50_image()
        self.assertEqual(sum(img[0:0x2000]) & 0xFFFFFF,
                         int.from_bytes(img[0x5FF0:0x5FF3], "big"))
        self.assertEqual((int.from_bytes(img[0x5FF3:0x5FF6], "big")
                          - sum(img[0x4000:0x5F00])) & 0xFFFFFF, 0x33)

    def test_corrected_image_scans(self):
        """nmtlink.rom_image restores the LOCAL* read (b3 at 0x4E58) and
        the stored ROM sum: the phone then scans all 180 channels
        (dividers 18176-18355)"""
        import nmtlink
        d = next(os.path.dirname(c) for c in CANDS
                 if os.path.isfile(os.path.join(os.path.dirname(c), "na0")))
        path = nmtlink.rom_image(os.path.join(d, "na0") + "+" + os.path.join(d, "na1"), "MD50")
        self.addCleanup(os.unlink, path)
        img = open(path, "rb").read()
        self.assertEqual(img[0x4E58], 0x36)
        r = Radio(path, model="MD50", cu="CU53", power=False)
        r.prom(nmtlink.parse_prom("8123456 1234 00000"))
        r.power(True)
        r.run(3.0)
        divs = set()
        for _ in range(500):
            r.run(0.05)
            divs.add(r.synth()["div"])
        self.assertEqual((min(divs), max(divs), len(divs)), (18176, 18355, 180))


if __name__ == "__main__":
    unittest.main()
