"""
NMT signalling helpers: Hagelbarger coding (python/nmtframe.py) and the
FFSK modem (python/ffsk.py); with the original MD59 ROM, its roaming
seizure through FFSK audio.  Needs numpy.
"""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "python"))
sys.path.insert(0, HERE)

import nmtframe as nf  # noqa: E402

try:
    import numpy  # noqa: F401
    from ffsk import Modulator, Demodulator
except ImportError:
    Modulator = None


class TestHagelbarger(unittest.TestCase):

    def test_roundtrip_and_bursts(self):
        rnd = random.Random(1)
        for _ in range(200):
            d = [rnd.randrange(16) for _ in range(16)]
            c = nf.encode(d)
            self.assertEqual(len(c), 140)
            bad = list(c)
            s = rnd.randrange(0, 134)
            for k in range(s, s + rnd.randint(1, 6)):
                bad[k] ^= 1
            self.assertEqual(nf.hagelbarger_decode(bad), nf.digits_to_bits(d))

    def test_idle_frame(self):
        # channel 1, MS power 1 (osmocom's default), area 51, its H info
        d = nf.frame(1, nf.P_1B, [5, 1, 0xE, 0xB, 0, 0, 0, 2, 8, 0, 0, 0], power=1)
        self.assertEqual("".join("%X" % x for x in d), "201451EB00028000")
        self.assertEqual(nf.decode_channel_450(nf.channel_450(173, 2)), (173, 2))


@unittest.skipUnless(Modulator, "numpy not installed")
class TestFFSK(unittest.TestCase):

    def test_roundtrip_any_blocks(self):
        rnd = random.Random(2)
        bits = [rnd.randrange(2) for _ in range(3000)]
        x = Modulator(48000).bits(bits)
        dem, got = Demodulator(48000), []
        for i in range(0, len(x), 777):
            got += dem.feed(x[i:i + 777])
        s = "".join(map(str, got))
        self.assertGreaterEqual(s.find("".join(map(str, bits[40:2900]))), 0)

    def test_phone_seizure_as_audio(self):
        import test_md59orig as t
        if not t.ROM:
            self.skipTest("original MD59 dump not found")
        from md5xemu import Radio
        r = Radio(t.ROM, model="MD59", cu="CU59", power=False)
        r.eeprom(t.EEPROM)
        feed = t.base_station(r, 1, nf.P_1B, 0x51)
        r.power(True)
        mod, slots = Modulator(48000), []
        while r.time() < 16.0:
            feed()
            r.run(0.5)
            slots += r.tx_take()
        on = [s for s in slots if s != Radio.TX_OFF]
        audio = numpy.concatenate([mod.bits([s]) if s in (0, 1) else mod.silence(1)
                                   for s in on])
        dem = Demodulator(48000)
        bits = dem.feed(audio)
        self.assertIn("001E812345600000", t.decode_tx_bits(bits))


if __name__ == "__main__":
    unittest.main()
