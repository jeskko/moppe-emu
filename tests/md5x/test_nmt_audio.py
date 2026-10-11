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


class TestAirWhileOff(unittest.TestCase):

    def test_slots_continue_while_off(self):
        """one TX slot per bit period while off too: no burst of stale
        slots at the next power-on (nmtlink's audio would lag)"""
        import roms
        from md5xemu import Radio
        try:
            rom, lst, model, cu = roms.build("mx5x-md59")
        except roms.Unavailable as e:
            self.skipTest(str(e))
        r = Radio(rom, lst, model=model, cu=cu)
        r.run(1.0)
        r.tx_take()
        r.power(False)
        r.run(1.0)
        off = r.tx_take()
        self.assertAlmostEqual(len(off), 1200, delta=2)
        self.assertEqual(set(off), {Radio.TX_OFF})
        r.power(True)
        r.run(0.1)
        self.assertAlmostEqual(len(r.tx_take()), 120, delta=2)


@unittest.skipUnless(Modulator, "needs numpy")
class TestCompandor(unittest.TestCase):

    def test_unity_and_roundtrip(self):
        try:
            import nmtlink
        except ImportError as e:
            self.skipTest(str(e))
        t = numpy.arange(48000) / 48000.0
        for a in (0.1, nmtlink.NOMINAL, 1.0):
            x = a * numpy.sin(2 * numpy.pi * 700 * t)
            c, e = nmtlink.Compandor(False), nmtlink.Compandor(True)
            y = numpy.concatenate([c(x[i:i + 960]) for i in range(0, 48000, 960)])
            z = numpy.concatenate([e(y[i:i + 960]) for i in range(0, 48000, 960)])
            # 2:1 around the nominal level, and back
            self.assertAlmostEqual(abs(y[-4800:]).max(),
                                   (a * nmtlink.NOMINAL) ** 0.5, delta=0.02)
            self.assertAlmostEqual(abs(z[-4800:]).max(), a, delta=0.02 * max(a, 0.3))


@unittest.skipUnless(Modulator, "needs numpy")
class TestRingback(unittest.TestCase):

    def end(self, rings):
        try:
            import nmtlink
        except ImportError as e:
            self.skipTest(str(e))
        import threading
        n = nmtlink.NetworkEnd.__new__(nmtlink.NetworkEnd)   # no PipeWire
        n.music = numpy.full(1000, 0.1)
        n.mic, n.rings, n.ring_level = None, rings, 0.1
        n.lock = threading.Lock()
        n.state, n.t_state, n.pos = "idle", 0, 0
        return n

    def audio(self, n, seconds):
        return numpy.concatenate([n._block(960) for _ in range(int(seconds * 50))])

    def test_call_from_phone(self):
        n = self.end(2)
        n.on_line("console.c:251 Incoming call from '8123456'")
        self.assertFalse(self.audio(n, 1.0).any())          # silent until connected
        n.on_line("nmt.c:1040 (chan 1) Connect audio.")
        x = self.audio(n, 11.0)
        on = [abs(x[i * 48000:(i + 1) * 48000]).max() > 0.05 for i in range(11)]
        # ring, 4 s off, ring, 1 s pause, then the call audio
        self.assertEqual(on, [1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1])
        self.assertEqual(n.state, "connected")
        n.on_line("console.c:321 Call disconnected with audio (hangup)")
        self.assertFalse(self.audio(n, 0.5).any())

    def test_call_to_phone_and_no_rings(self):
        for rings, first in ((2, "console.c:477 Outgoing call to '8123456'"),
                             (0, "console.c:251 Incoming call from '8123456'")):
            n = self.end(rings)
            n.on_line(first)
            n.on_line("nmt.c:1336 (chan 1) Connect audio.")
            self.assertTrue(numpy.allclose(self.audio(n, 0.1), 0.1))


if __name__ == "__main__":
    unittest.main()
