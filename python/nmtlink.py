"""
Link an emulated Talkman to osmocom-analog's NMT base station through two
FIFOs carrying 48 kHz mono WAVE audio: the base station's transmitter
(osmocom --write-tx-wave) into the phone's receiver, and the phone's
transmitter into the base station's receiver (--read-rx-wave).

    mkfifo bs_tx.wav bs_rx.wav
    nmt -k 1 -Y DK,1 -a null --write-tx-wave bs_tx.wav --read-rx-wave bs_rx.wav &
    python3 nmtlink.py ROM bs_tx.wav bs_rx.wav --eeprom 8123,456F,... --channel 1

(tools/osmocom-analog/run-nmt.sh does all of it.)  Calls: --keys for the
handset (e.g. 26:0123456,30:offhook,50:onhook), --bs-keys for osmocom's
console (26:d calls the phone after its roaming update, h hangs up).
Times are emulated seconds, a warm-boot lead-in of about 4.5 s included.
osmocom's own timers run on the wall clock, so its timeouts (e.g. 3 s
without SAT) do not fire in a lockstep run.  osmocom runs as fast
as its sound device lets it; with the 'null' device that is unlimited,
so its wave reader must wait for data instead of padding with silence,
and read a FIFO without waiting for a full second of it
(tools/osmocom-analog/wave-fifo-lockstep.patch) for the two programs to
run in step.  The phone's audio is FFSK and the looped-back SAT (no speech yet).
"""
import argparse
import os
import struct
import sys
import time

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from md5xemu import Radio  # noqa: E402
from ffsk import Modulator, Demodulator  # noqa: E402
import nmtframe as nf  # noqa: E402

RATE = 48000
BLOCK = 960                 # 20 ms
CH1_DIV = 19376
SAT_BIT = 2                 # latch 1 bit 2: the SAT switch (manual: "Ö")


def wav_header(rate=RATE):
    data = 0x7FFFF000
    return (b"RIFF" + struct.pack("<I", data + 36) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) +
            b"data" + struct.pack("<I", data))


def read_header(f):
    """osmocom writes a 44-byte placeholder (zeros) and fills it in only
    when it closes the file, which a FIFO cannot do: skip it"""
    read_exact(f, 44)


def read_exact(f, n):
    out = b""
    while len(out) < n:
        b = f.read(n - len(out))
        if not b:
            raise EOFError
        out += b
    return out


class Link:
    def __init__(self, radio, channel, level=0.5, log=print):
        self.r = radio
        self.div = CH1_DIV + channel - 1
        self.r.air_channel(self.div)
        self.mod = Modulator(RATE)
        self.dem = Demodulator(RATE)
        self.level = level
        self.log = log
        self.slots = []         # phone TX slots not yet turned into audio
        self.owed = 0.0         # fractional samples of the slot clock
        self.rx_bits = []       # demodulated BS bits (for logging)
        self.tx_on = False
        self.tx_slots = []
        self.pending = []
        # the SAT band (osmocom: 3955-3985 Hz), filtered out of the
        # receiver's audio and looped to the modulator while the SAT
        # switch is closed, as AP1's FX316J and gate do
        self.sat_sos = signal.butter(2, [3850, 4100], "bandpass", fs=RATE,
                                     output="sos")
        self.sat_zi = signal.sosfilt_zi(self.sat_sos) * 0.0
        self.sat = np.zeros(0)
        self.sat_on = False

    def bs_audio(self, samples):
        """base station TX audio -> bits into the phone's air"""
        sat, self.sat_zi = signal.sosfilt(self.sat_sos, samples, zi=self.sat_zi)
        self.sat = np.concatenate([self.sat, sat])
        bits = self.dem.feed(samples)
        self.r.air_push([2 if b < 0 else b for b in bits])
        self.rx_bits += bits

    def phone_audio(self, n):
        """n samples of the phone's TX audio"""
        for s in self.r.tx_take():
            if s in (0, 1):
                a = self.mod.bits([s]) * self.level
            else:
                a = self.mod.silence(1)
            if s != Radio.TX_OFF and not self.tx_on:
                self.tx_on = True
                self.tx_slots = []
                self.log("%8.3f phone TX on" % self.r.time())
            if s == Radio.TX_OFF and self.tx_on:
                self.tx_on = False
                self.log("%8.3f phone TX off: %s" % (
                    self.r.time(), " ".join(decode(self.tx_slots)) or "(no frame)"))
            if self.tx_on:
                self.tx_slots.append(s)
            self.pending.append(a)
        x = np.concatenate(self.pending) if self.pending else np.zeros(0)
        if len(x) < n:
            x = np.concatenate([x, np.zeros(n - len(x))])
        self.pending = [x[n:]]
        x, sat, self.sat = x[:n], self.sat[:n], self.sat[n:]
        on = bool(self.r.latch(1) >> SAT_BIT & 1)
        if on != self.sat_on:
            self.sat_on = on
            self.log("%8.3f SAT loop %s" % (self.r.time(), "on" if on else "off"))
        if on and self.tx_on:
            x = x.copy()
            x[:len(sat)] += sat
        return x

    def bs_frames(self):
        """frames decoded from the base station since the last call"""
        f, cut = frames(self.rx_bits)
        self.rx_bits = self.rx_bits[cut:]
        return f


def decode(slots):
    s = "".join(str(v) if v in (0, 1) else "i" for v in slots)
    sync = "".join(map(str, nf.BIT_SYNC[-4:] + nf.FRAME_SYNC))
    out, i = [], s.find(sync)
    while i >= 0 and i + len(sync) + 140 <= len(s):
        seg = s[i + len(sync):i + len(sync) + 140]
        if "i" not in seg:
            out.append("".join("%X" % d for d in nf.bits_to_digits(
                nf.hagelbarger_decode([int(c) for c in seg]))))
        i = s.find(sync, i + len(sync) + 140)
    return out


def frames(bits):
    """(frames, index up to which bits are used)"""
    s = "".join("x" if b < 0 else str(b) for b in bits)
    sync = "".join(map(str, nf.BIT_SYNC[-6:] + nf.FRAME_SYNC))
    out, i, used = [], s.find(sync), 0
    while i >= 0 and i + len(sync) + 140 <= len(s):
        seg = s[i + len(sync):i + len(sync) + 140]
        if "x" not in seg:
            out.append("".join("%X" % d for d in nf.bits_to_digits(
                nf.hagelbarger_decode([int(c) for c in seg]))))
        used = i + len(sync) + 140
        i = s.find(sync, used)
    if i < 0:
        used = max(used, len(s) - len(sync) - 140)
    return out, max(0, used)


def handset_script(spec, hold=0.15, gap=0.15):
    """[(time, log text, action(radio))] from "T:ACTION,...", in time order;
    typed keys become key down/up events (run in 20 ms steps)"""
    out = []
    for item in filter(None, spec.split(",")):
        t, act = item.split(":", 1)
        t = float(t)
        if act in ("offhook", "onhook"):
            out.append((t, act, lambda r, v=act == "offhook": r.set_hook(v)))
            continue
        out.append((t, "type " + act, lambda r: None))
        for k in act:
            out.append((t, None, lambda r, k=k: r.key(k)))
            out.append((t + hold, None, lambda r: r.key(None)))
            t += hold + gap
    return sorted(out, key=lambda e: e[0])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("bs_tx", help="FIFO osmocom writes its TX audio to")
    ap.add_argument("bs_rx", help="FIFO osmocom reads its RX audio from")
    ap.add_argument("--model", default="MD59")
    ap.add_argument("--cu", default="CU59")
    ap.add_argument("--eeprom", help="16 hex words, comma separated")
    ap.add_argument("--channel", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=60.0)
    # osmocom reads 50 ms at a time: the phone's audio must be that far
    # ahead (it is also the extra delay the base station sees)
    ap.add_argument("--prefill", type=float, default=0.06)
    ap.add_argument("--keys", default="",
                    help="handset script, T:ACTION,... with ACTION digits/keys "
                    "to type, 'offhook' or 'onhook' (e.g. 30:0123456,32:offhook)")
    ap.add_argument("--console",
                    help="FIFO into osmocom's console (its terminal's input)")
    ap.add_argument("--bs-keys", default="",
                    help="console script, T:KEYS,... (osmocom: digits, d dial, "
                    "h hang up; after a roaming update d calls the phone)")
    ap.add_argument("--cold", action="store_true",
                    help="link straight after a cold start (the original MD59 "
                    "then dials the first two digits as 0)")
    a = ap.parse_args()

    r = Radio(a.rom, model=a.model, cu=a.cu, power=False)
    if a.eeprom:
        r.eeprom([int(w, 16) for w in a.eeprom.split(",")])
    if not a.cold:
        # The original MD59's cold start (fresh RAM) calls 0x39AC with X =
        # rB (scal pushes there) and returns with X = r2 (sret pops two
        # bytes): the stack then sits 2 bytes high and overwrites the
        # first two digits of the number copy at 0x8300 when dialling.
        # A real phone has battery-backed RAM and boots warm: do the same.
        r.power(True)
        r.run(4.0)
        r.set_input("poweroff", 1)
        while r.powered() and r.time() < 10:
            r.run(0.1)
        r.set_input("poweroff", 0)
        r.run(0.5)
        r.tx_take()             # not on the air: no link yet
    link = Link(r, a.channel)
    # run-nmt.sh starts osmocom with this FIFO as its terminal's input:
    # open it first, or osmocom never starts
    console = open(a.console, "wb", buffering=0) if a.console else None
    bs_script = []
    for item in filter(None, a.bs_keys.split(",")):
        t, keys = item.split(":", 1)
        bs_script.append((float(t), keys))
    bs_script.sort()
    # osmocom opens its TX recording first, then the RX playback (and
    # reads its header at once): open in the same order
    tx = open(a.bs_tx, "rb", buffering=0)
    rx = open(a.bs_rx, "wb", buffering=0)
    rx.write(wav_header())
    rx.write(b"\0" * int(a.prefill * RATE) * 2)
    read_header(tx)
    script = handset_script(a.keys)
    r.power(True)
    t0 = time.time()
    leds = None
    while r.time() < a.seconds:
        raw = read_exact(tx, BLOCK * 2)
        link.bs_audio(np.frombuffer(raw, "<i2").astype(float) / 32768.0)
        r.run(BLOCK / RATE)
        while script and script[0][0] <= r.time():
            _, what, act = script.pop(0)
            act(r)
            if what:
                link.log("%8.3f %s" % (r.time(), what))
        while bs_script and bs_script[0][0] <= r.time():
            _, keys = bs_script.pop(0)
            link.log("%8.3f console %s" % (r.time(), keys))
            if console:
                console.write(keys.encode())
        out = link.phone_audio(BLOCK)
        rx.write((np.clip(out, -1, 1) * 32767).astype("<i2").tobytes())
        for fr in link.bs_frames():
            if fr != getattr(link, "last", None):
                link.log("%8.3f BS: %s" % (r.time(), fr))
                link.last = fr
        l = r.led_names()
        if l != leds:
            leds = l
            link.log("%8.3f leds %s" % (r.time(), " ".join(l)))
    link.log("done: %.1f s emulated in %.1f s" % (r.time(), time.time() - t0))


if __name__ == "__main__":
    main()
