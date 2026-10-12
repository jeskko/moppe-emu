"""
Link an emulated Talkman to osmocom-analog's NMT base station.  The link
carries 48 kHz mono audio both ways through two FIFOs: the base
station's transmitter (osmocom --write-tx-wave) into the phone's
receiver, and the phone's transmitter into the base station's receiver
(--read-rx-wave).  osmocom runs in a pseudo-terminal (its console needs
one), started by this script:

    python3 nmtlink.py --nmt ~/src/osmo/osmocom-analog/src/nmt/nmt ROM \\
        --eeprom 8123,456F,... --keys 26:0123456,30:offhook,50:onhook

Options after a lone "--" go to osmocom (default -k 1 -Y DK,1).

Calls: --keys for the handset (e.g. 26:0123456,30:offhook,50:onhook),
--bs-keys for osmocom's console (26:d calls the phone after its roaming
update, h hangs up).  Times are emulated seconds, a warm-boot lead-in of
about 4.5 s included.

Lockstep (default): osmocom's 'null' sound device does not pace it, so
its wave reader waits for our data
(tools/osmocom-analog/wave-fifo-lockstep.patch) and the two programs run
in step, as fast as the emulator goes.  osmocom's own timers run on the
wall clock, so its timeouts (e.g. 3 s without SAT) do not fire.

--realtime paces the emulator to the wall clock, and with it osmocom.
Speech then works through sound devices (ALSA names; with PipeWire
"pipewire", or "pipewire:NODE=<node name or serial>" for one node):
--net-audio is osmocom's call audio (the fixed network's end of a call,
its -c option; -e among osmocom's options echoes the phone's speech
instead), --radio-out the phone's earpiece/loudspeaker and --radio-in its
microphone.  --hold-music FILE makes the network play FILE into calls
(through a private PipeWire sink; --net-audio then only plays).  A call
from the phone hears --rings N rings of ringback (425 Hz) before the
network's audio; osmocom itself answers at once.

The phone's audio paths are modelled here, not in the emulator: FFSK
from the modem's bits, SAT looped back while the SAT gate is closed,
earpiece and microphone gates, and the 2:1 compandor (latch bits from the
ME59 manual's output table).
"""
import argparse
import collections
import fcntl
import os
import pty
import re
import shlex
import shutil
import signal as sig
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

import numpy as np
from scipy import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from md5xemu import Radio  # noqa: E402
from ffsk import Modulator, Demodulator  # noqa: E402
import nmtframe as nf  # noqa: E402

RATE = 48000
BLOCK = 960                 # 20 ms
# synth divider of channel 1 (NMT-450, 25 kHz steps, 180 channels up):
# MD59 from its RX LO (channel + 21.4 MHz), MD50 from its scan (18176-18355)
CH1_DIV = {"MD59": 19376, "MD50": 18176}

# output latch gates of the audio paths: (latch, bit, level when on), from
# the ME59 manual's output table and the ham sources' MD50/MD59 defines
GATES = {
    "MD59": {
        "sat": (1, 2, 1),       # "Ö" (PHI): SAT from the receiver to the modulator
        "ear": (1, 1, 1),       # H: earpiece
        "mic": (1, 3, 1),       # S (HSM): handset microphone (also pulses at idle)
        "lsp": (3, 3, 1),       # HF: loudspeaker amplifier
        "cc": (7, 3, 1),        # ME59 compandor on; never set by the MD59 firmware
        "alo": (5, 2, 1),       # alert tone generator
        "kv": (5, 1, 1),        # its level
    },
    "MD50": {                   # AP1: no compandor; NPHI, NMIC active low
        "sat": (1, 2, 0),
        "ear": (1, 1, 1),
        "mic": (1, 3, 0),
        "lsp": (3, 3, 1),
        "alo": (5, 2, 1),
        "kv": (5, 1, 1),
    },
}
ALERT_HZ = 2000.0           # a guess: set by R13/C10 on AP1, not documented

# osmocom's wave samples are deviation / 4700 Hz: speech at its nominal
# 3 kHz deviation (its compandor's unity level) is 0.64.  Its call audio
# has that level at 0.112 of full scale (SPEECH_LEVEL * 0.7079); our
# sound devices have it at about 0.5 (ear and mic gains).
NOMINAL = 3000.0 / 4700.0
CALL_NOMINAL = 0.1585 * 0.7079
DEFAULT_NMT_OPTS = ["-k", "1", "-Y", "DK,1"]


def wav_header(rate=RATE):
    data = 0x7FFFF000
    return (b"RIFF" + struct.pack("<I", data + 36) + b"WAVE" +
            b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) +
            b"data" + struct.pack("<I", data))


def read_exact(fd, n):
    out = b""
    while len(out) < n:
        b = os.read(fd, n - len(out))
        if not b:
            raise EOFError("osmocom closed its audio")
        out += b
    return out


def gate(r, name):
    """a gate of GATES for this radio's model: 1 when on (0 if it has none)"""
    g = GATES[r.model].get(name)
    return int((r.latch(g[0]) >> g[1] & 1) == g[2]) if g else 0


# ---- osmocom

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")
LOGLINE = re.compile(r"(\w+\.c):(\d+) (.*)")


class Osmocom:
    """osmocom-analog's NMT base station in a pseudo-terminal, its wave
    FIFOs in a temporary directory.  log_lines holds its log lines
    ("nmt.c:697 Received ..."), newest last; quiet lines (levels printed
    twice a second) are left out unless verbose."""

    QUIET = ("RX Level", "Supervisory level")

    def __init__(self, binary, opts=None, call_device=None, log_path=None,
                 verbose=False, on_line=None):
        self.dir = tempfile.mkdtemp(prefix="nmtlink")
        self.tx_path = os.path.join(self.dir, "bs_tx.wav")
        self.rx_path = os.path.join(self.dir, "bs_rx.wav")
        os.mkfifo(self.tx_path)
        os.mkfifo(self.rx_path)
        opts = list(opts or DEFAULT_NMT_OPTS)
        if not {"-b", "--buffer"} & set(opts):
            # it processes this far ahead, in chunks this long; a radio
            # link has no delay, and the phone's answers must come within
            # osmocom's frame counts (8 identity requests): keep it short
            opts += ["-b", "10"]
        argv = [binary] + opts + [
            "-a", "null", "--write-tx-wave", self.tx_path,
            "--read-rx-wave", self.rx_path]
        if call_device:
            argv += ["-c", call_device]
            if "--call-buffer" not in argv:
                # osmocom writes call audio in bursts as it reads ours:
                # 50 ms (its default) underruns
                argv += ["--call-buffer", "200"]
        self.master, slave = pty.openpty()
        # a wide terminal, or osmocom wraps (and crashes on no size at all)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                                     start_new_session=True, close_fds=True)
        os.close(slave)
        self.log_file = open(log_path, "wb") if log_path else None
        self.verbose = verbose
        self.listeners = [on_line] if on_line else []
        self.log_lines = collections.deque(maxlen=200)
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.tx = self.rx = None

    def _read(self):
        """drain the terminal (osmocom blocks when it fills up)"""
        buf = b""
        while True:
            try:
                b = os.read(self.master, 4096)
            except OSError:
                break
            if not b:
                break
            if self.log_file:
                self.log_file.write(b)
                self.log_file.flush()
            buf += b
            *lines, buf = re.split(rb"[\r\n]", buf)
            for raw in lines:
                m = LOGLINE.search(ANSI.sub(b"", raw).decode("utf-8", "replace"))
                if not m:
                    continue
                text = "%s:%s %s" % m.groups()
                if not self.verbose and any(q in text for q in self.QUIET):
                    continue
                with self.lock:
                    self.log_lines.append(text)
                for f in self.listeners:
                    f(text)

    def open_audio(self, prefill, timeout=10.0):
        """Open the FIFOs as osmocom expects (it opens its TX recording,
        then the RX playback and reads its header at once); returns
        after writing the RX header and prefill and skipping the TX
        placeholder header (zeros, filled in only when the file is
        closed, which a FIFO cannot do)."""
        self.tx = os.open(self.tx_path, os.O_RDONLY | os.O_NONBLOCK)
        t_end = time.monotonic() + timeout
        while True:
            try:
                self.rx = os.open(self.rx_path, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError:
                if self.proc.poll() is not None or time.monotonic() > t_end:
                    raise RuntimeError("osmocom did not start; its log:\n" +
                                       "\n".join(self.log_lines))
                time.sleep(0.02)
        for fd in (self.tx, self.rx):
            fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) & ~os.O_NONBLOCK)
        os.write(self.rx, wav_header())
        os.write(self.rx, b"\0" * int(prefill * RATE) * 2)
        read_exact(self.tx, 44)

    def read(self, n):
        """n samples of the base station's transmitter"""
        raw = read_exact(self.tx, n * 2)
        return np.frombuffer(raw, "<i2").astype(float) / 32768.0

    def write(self, x):
        os.write(self.rx, (np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())

    def console(self, keys):
        os.write(self.master, keys.encode())

    def lines(self, n):
        with self.lock:
            return list(self.log_lines)[-n:]

    def close(self):
        if self.proc.poll() is None:
            self.proc.send_signal(sig.SIGINT)
            try:
                self.proc.wait(2.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.reader.join(1.0)   # its last lines (EIO once it is gone)
        for fd in (self.tx, self.rx, self.master):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        if self.log_file:
            self.log_file.close()
        shutil.rmtree(self.dir, ignore_errors=True)


# ---- sound devices (ALSA through aplay/arecord)

def stop(p):
    """end aplay/arecord (aplay may sit draining after SIGTERM)"""
    for f in (p.stdin, p.stdout):
        if f:
            try:
                f.close()
            except OSError:
                pass
    p.terminate()
    try:
        p.wait(1.0)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


class SoundOut:
    """mono 48 kHz playback; drops audio rather than wait for the device"""

    def __init__(self, device):
        self.p = subprocess.Popen(
            ["aplay", "-q", "-D", device, "-t", "raw", "-f", "S16_LE",
             "-r", str(RATE), "-c", "1", "--buffer-time=200000"],
            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
        fd = self.p.stdin.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)

    def write(self, x):
        try:
            os.write(self.p.stdin.fileno(),
                     (np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
        except (BlockingIOError, BrokenPipeError):
            pass

    def close(self):
        stop(self.p)


class SoundIn:
    """mono 48 kHz capture; read() gives what has come (zeros if late)
    and never lets more than max_s of it pile up"""

    def __init__(self, device, max_s=0.2):
        self.p = subprocess.Popen(
            ["arecord", "-q", "-D", device, "-t", "raw", "-f", "S16_LE",
             "-r", str(RATE), "-c", "1", "--buffer-time=200000"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.buf = np.zeros(0)
        self.max = int(max_s * RATE)
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            b = self.p.stdout.read(BLOCK * 2)
            if not b:
                break
            x = np.frombuffer(b[:len(b) & ~1], "<i2").astype(float) / 32768.0
            with self.lock:
                self.buf = np.concatenate([self.buf, x])[-self.max:]

    def read(self, n):
        with self.lock:
            x, self.buf = self.buf[:n], self.buf[n:]
        return np.concatenate([x, np.zeros(n - len(x))])

    def close(self):
        self.p.terminate()
        self.p.wait()


class NetworkEnd:
    """The fixed network's end of a call, as osmocom's call capture: a
    private PipeWire null sink with its monitor remapped to a source
    (unmuted: it comes up muted), fed by pw-play from here.  It plays
    silence when idle; for a call from the phone, ringback (rings x 425 Hz
    1 s on / 4 s off, then 1 s) before the call audio; for a call to the
    phone the call audio at once.  The call audio is a file looped (hold,
    anything ffmpeg reads, else WAV) or a capture device (mic), else
    silence.  device is the ALSA capture name of the source, for -c.
    Call progress comes from osmocom's log lines (on_line)."""

    RING_HZ, RING_ON, RING_OFF = 425.0, 1.0, 4.0
    AHEAD = 0.1                 # seconds written ahead of the wall clock

    def __init__(self, hold=None, mic=None, rings=2, level=1.3 * CALL_NOMINAL):
        self.music = None
        if hold:
            x = self._load(hold)
            self.music = x / max(1e-9, np.abs(x).max()) * level
        self.mic = SoundIn(mic, max_s=0.3) if mic else None
        self.rings = rings
        self.ring_level = 0.7 * CALL_NOMINAL
        self.lock = threading.Lock()
        self.state = "idle"     # idle, from_phone (call set up), ringback, connected
        self.t_state = 0        # samples since the state began
        self.pos = 0            # position in the music
        self._unload_stale()
        name = "nmtlink_net_%d" % os.getpid()
        self.modules = []
        self.modules.append(self._pactl("load-module", "module-null-sink",
                                        "sink_name=" + name, "channel_map=mono",
                                        "rate=%d" % RATE,
                                        "sink_properties=device.description=" + name))
        self.modules.append(self._pactl("load-module", "module-remap-source",
                                        "master=%s.monitor" % name,
                                        "source_name=%s_src" % name, "channel_map=mono",
                                        "source_properties=device.description=%s_src" % name))
        self._pactl("set-source-mute", name + "_src", "0")
        self.device = "pipewire:NODE=%s_src" % name
        # pw-play, never moved to another output (the speakers) if the
        # sink goes away while it plays
        self.p = subprocess.Popen(
            ["pw-play", "--target", name, "-P",
             "{ node.dont-fallback = true node.dont-reconnect = true }",
             "--raw", "--rate", str(RATE), "--channels", "1", "--format", "s16", "-"],
            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.stop = False
        threading.Thread(target=self._play, daemon=True).start()

    # call progress from osmocom's log
    def on_line(self, text):
        with self.lock:
            if "Incoming call from" in text:            # the phone calls out
                self._set("from_phone")
            elif "Connect audio" in text:
                self._set("ringback" if self.state == "from_phone" and self.rings
                          else "connected")
            elif ("Call disconnected" in text or "Call hangup" in text
                  or "Entering IDLE state" in text):
                self._set("idle")

    def _set(self, state):
        if state != self.state:
            self.state, self.t_state = state, 0
            if state == "connected":
                self.pos = 0

    def _block(self, n):
        with self.lock:
            state, t = self.state, self.t_state
            if state == "ringback":
                cycle = self.RING_ON + self.RING_OFF
                end = (self.rings - 1) * cycle + self.RING_ON + 1.0
                k = np.arange(t, t + n) / RATE
                if k[0] >= end:
                    self._set("connected")
                    state = "connected"
                else:
                    on = (k % cycle < self.RING_ON) & (k < end - 1.0)
                    x = np.sin(2 * np.pi * self.RING_HZ * k) * self.ring_level * on
            if state == "connected":
                if self.music is not None:
                    idx = (self.pos + np.arange(n)) % len(self.music)
                    x = self.music[idx]
                    self.pos = (self.pos + n) % len(self.music)
                elif self.mic:
                    # 0.5 of full scale to osmocom's nominal, like the radio
                    x = self.mic.read(n) * (CALL_NOMINAL / 0.5)
                else:
                    x = np.zeros(n)
            elif state != "ringback":
                x = np.zeros(n)
                if self.mic:
                    self.mic.read(n)        # keep it current
            self.t_state += n
        return x

    def _play(self):
        """20 ms at a time, at most AHEAD in front of the wall clock"""
        t0, sent = time.monotonic(), 0
        while not self.stop:
            ahead = sent / RATE - (time.monotonic() - t0)
            if ahead > self.AHEAD:
                time.sleep(ahead - self.AHEAD)
                continue
            if ahead < -0.2:                # fell behind (stalled): resync
                t0, sent = time.monotonic(), 0
            x = self._block(BLOCK)
            try:
                self.p.stdin.write((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
                self.p.stdin.flush()
            except (BrokenPipeError, ValueError, OSError):
                break
            sent += BLOCK

    @classmethod
    def _unload_stale(cls):
        """modules left by an nmtlink that was killed"""
        for line in cls._pactl("list", "short", "modules").splitlines():
            m = re.search(r"nmtlink_(?:hold|net)_(\d+)", line)
            if m and not os.path.exists("/proc/" + m.group(1)):
                subprocess.run(["pactl", "unload-module", line.split()[0]],
                               capture_output=True)

    @staticmethod
    def _pactl(*args):
        return subprocess.run(["pactl"] + list(args), check=True, capture_output=True,
                              text=True).stdout.strip()

    @staticmethod
    def _load(path):
        if shutil.which("ffmpeg"):
            raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "s16le",
                                  "-ac", "1", "-ar", str(RATE), "-"],
                                 check=True, capture_output=True).stdout
            return np.frombuffer(raw, "<i2").astype(float)
        from scipy.io import wavfile
        rate, x = wavfile.read(path)
        x = x.astype(float)
        if x.ndim > 1:
            x = x.mean(axis=1)
        return signal.resample_poly(x, RATE, rate) if rate != RATE else x

    def close(self):
        self.stop = True
        stop(self.p)
        if self.mic:
            self.mic.close()
        for m in reversed(self.modules):
            subprocess.run(["pactl", "unload-module", m], capture_output=True)


def call_device(net_audio=None, net=None):
    """osmocom's -c: the network's speaker/microphone (ALSA, "play/rec"
    for two devices), or with a NetworkEnd its playback part (null if
    none) and the NetworkEnd as capture"""
    if not net:
        return net_audio
    play = (net_audio or "null").split("/")[0]
    return "%s/%s" % (play, net.device)


# ---- the phone's audio

class Compandor:
    """2:1 syllabic compressor or expander around unity level ref (a
    rectified-average envelope, about 20 ms)"""

    def __init__(self, expand, ref=NOMINAL, tau=0.02):
        self.expand = expand
        self.ref = ref
        a = np.exp(-1.0 / (tau * RATE))
        self.b, self.a = [1.0 - a], [1.0, -a]
        self.zi = np.array([ref * 2 / np.pi])

    def __call__(self, x):
        env, self.zi = signal.lfilter(self.b, self.a, np.abs(x), zi=self.zi)
        env = np.maximum(env * (np.pi / 2), 1e-4)
        if self.expand:
            return x * (env / self.ref)
        return x / np.sqrt(env / self.ref)


class Link:
    """The phone's side: base station audio in, the phone's transmitter
    audio out, one block at a time."""

    def __init__(self, radio, channel, level=0.5, ear_gain=0.8, mic_gain=1.3,
                 compandor=True, log=print):
        self.r = radio
        self.div = CH1_DIV[radio.model] + channel - 1
        self.r.air_channel(self.div)
        self.mod = Modulator(RATE)
        self.dem = Demodulator(RATE)
        self.level = level
        self.ear_gain = ear_gain
        self.mic_gain = mic_gain
        self.compandor = compandor
        self.alert_phase = 0.0
        self.log = log
        self.owed = 0.0
        self.rx_bits = []       # demodulated BS bits (for logging)
        self.tx_on = False
        self.tx_slots = []
        self.pending = []
        # the receiver's audio split by AP1's FX316J: the SAT band
        # (osmocom: 3955-3985 Hz), looped to the modulator while the SAT
        # gate is closed, and the speech band for the earpiece; the
        # microphone's speech band.  Emphasis is left out: osmocom's
        # audio is the flat speech, and the phone's own pre- and
        # de-emphasis would cancel.
        self.sat_sos = signal.butter(2, [3850, 4100], "bandpass", fs=RATE, output="sos")
        self.sat_zi = np.zeros((self.sat_sos.shape[0], 2))
        self.ear_sos = signal.butter(4, [300, 3400], "bandpass", fs=RATE, output="sos")
        self.ear_zi = np.zeros((self.ear_sos.shape[0], 2))
        self.mic_sos = signal.butter(4, [300, 3000], "bandpass", fs=RATE, output="sos")
        self.mic_zi = np.zeros((self.mic_sos.shape[0], 2))
        self.expander = Compandor(True)
        self.compressor = Compandor(False)
        self.sat = np.zeros(0)
        self.speech = np.zeros(0)
        self.state = {}
        self.rng = np.random.default_rng(1)

    def watch(self, name, on):
        if self.state.get(name) != on:
            self.state[name] = on
            self.log("%8.3f %s %s" % (self.r.time(), name, "on" if on else "off"))

    def bs_audio(self, samples):
        """base station TX audio -> bits into the phone's air, SAT and
        speech bands kept for this block"""
        sat, self.sat_zi = signal.sosfilt(self.sat_sos, samples, zi=self.sat_zi)
        sp, self.ear_zi = signal.sosfilt(self.ear_sos, samples, zi=self.ear_zi)
        self.sat = np.concatenate([self.sat, sat])
        self.speech = np.concatenate([self.speech, sp])
        bits = self.dem.feed(samples)
        # the carrier is the base station transmitting at all (SAT alone
        # is 0.06), not FFSK: with speech or silence on the traffic
        # channel the modem gets noise bits and SQ stays open
        carrier = float(np.sqrt(np.mean(samples ** 2))) > 0.005
        self.watch("BS carrier", carrier)
        self.r.air_push([(b if b >= 0 else int(self.rng.integers(2))) if carrier else 2
                         for b in bits])
        self.rx_bits += bits

    def companding(self):
        """The ME59 switches its compandor with CC; the MD59 firmware never
        sets that bit, and how it switches is not known: with compandor
        on, compand while the SAT loop is closed (a call in speech)"""
        return bool(gate(self.r, "cc")) or (self.compandor and bool(gate(self.r, "sat")))

    def ear_audio(self, n):
        """n samples for the earpiece/loudspeaker (after phone_audio):
        received speech through the H/HF gates, plus the alert tone"""
        x, self.speech = self.speech[:n], self.speech[n:]
        x = np.concatenate([x, np.zeros(n - len(x))])
        if self.companding():
            x = self.expander(x)
        on = gate(self.r, "ear") or gate(self.r, "lsp")
        x = x * self.ear_gain if on else np.zeros(n)
        if gate(self.r, "alo"):
            ph = self.alert_phase + 2 * np.pi * ALERT_HZ / RATE * np.arange(1, n + 1)
            x = x + np.sin(ph) * (0.4 if gate(self.r, "kv") else 0.15)
            self.alert_phase = ph[-1] % (2 * np.pi)
        return x

    def phone_audio(self, n, mic=None):
        """n samples of the phone's TX audio (mic: n microphone samples)"""
        slots = self.r.tx_take()
        data = any(s in (0, 1) for s in slots)
        for s in slots:
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
        x = x[:n].copy()
        sat, self.sat = self.sat[:n], self.sat[n:]
        sat_on = bool(gate(self.r, "sat"))
        # S also pulses at idle (the ham sources' HSM); speech only goes
        # out in a call (SAT loop closed) and never over data
        mic_on = (bool(gate(self.r, "mic")) and sat_on
                  and not data)
        self.watch("microphone", mic_on)
        cc = self.companding()
        self.watch("SAT loop", sat_on)
        self.watch("earpiece", bool(gate(self.r, "ear")))
        self.watch("loudspeaker", bool(gate(self.r, "lsp")))
        if mic is None:
            mic = np.zeros(n)
        sp, self.mic_zi = signal.sosfilt(self.mic_sos, mic * self.mic_gain, zi=self.mic_zi)
        if cc:
            sp = self.compressor(sp)
        if self.tx_on:
            if sat_on:
                x[:len(sat)] += sat
            if mic_on:
                x += sp
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


def rom_image(spec, model):
    """Path of the ROM image to load: a file, or "A+B" for the MD50's two
    8 KB EPROMs (na0 at 0000, na1 at 4000, each mirrored in its 16 KB
    window).  The original MD50 dump (na1) is corrected in a temporary
    copy, never the file: OH5NXO's dump has a `br` at 0x4E58 where the
    4021 read of LOCAL* (bit 1 of 0x8338) would be `b3`, which keeps the
    phone in LOCAL mode, and its ROM test (0x56BE) then fails on the
    stored sum of 4000-5EFF (0x5FF3-F5), which is set to the actual sum
    (still 0x2D off after the `b3`: more changes than this one)."""
    if "+" not in spec and model != "MD50":
        return spec
    parts = [open(p, "rb").read() for p in spec.split("+")]
    if len(parts) == 2 and all(len(p) == 8192 for p in parts):
        img = bytearray(parts[0] * 2 + parts[1] * 2)
    else:
        img = bytearray(b"".join(parts))
    if model == "MD50" and len(img) == 0x8000:
        if img[0x4E56:0x4E5A] == bytes([0x7B, 0x7A, 0x30, 0x5C]):
            for base in (0x4000, 0x6000):
                img[base + 0x0E58] = 0x36
        total = sum(img[0x4000:0x5F00]) & 0xFFFFFF
        for base in (0x4000, 0x6000):
            img[base + 0x1FF3:base + 0x1FF6] = total.to_bytes(3, "big")
    f = tempfile.NamedTemporaryFile(prefix="md5x_", suffix=".bin", delete=False)
    f.write(img)
    f.close()
    return f.name


def parse_prom(text):
    """MD50 ID PROM nibbles from hex digits (spaces ignored), e.g.
    "8123456 1234 00000": Z X1..X6, 4-digit code, flags (page 0)"""
    return [int(c, 16) for c in text if not c.isspace()]


def nmt_defaults(model, opts):
    """osmocom's options for this model: the MD50 has no compandor (-C 0)"""
    opts = list(opts or DEFAULT_NMT_OPTS)
    if model == "MD50" and not {"-C", "--compandor"} & set(opts):
        opts += ["-C", "0"]
    return opts


def warm_boot(r):
    """Boot cold, power off, boot warm (RAM kept).  The original MD59's
    cold start (fresh RAM) calls 0x39AC with X = rB (scal pushes there)
    and returns with X = r2 (sret pops two bytes): the stack then sits 2
    bytes high and overwrites the first two digits of the number copy at
    0x8300 when dialling.  A real phone has battery-backed RAM and boots
    warm."""
    r.power(True)
    r.run(4.0)
    r.set_input("poweroff", 1)
    t = r.time() + 6.0
    while r.powered() and r.time() < t:
        r.run(0.1)
    r.set_input("poweroff", 0)
    r.run(0.5)
    r.tx_take()             # not on the air: no link yet


class Session:
    """A radio linked to an osmocom base station, one 20 ms block per
    step().  The radio is powered on by start()."""

    def __init__(self, radio, osmo, channel=1, prefill=0.02, ear=None, mic=None,
                 hold=None, log=print, **link_opts):
        self.r = radio
        self.osmo = osmo
        self.link = Link(radio, channel, log=log, **link_opts)
        self.ear = ear
        self.mic = mic
        self.hold = hold
        self.log = log
        self.prefill = prefill
        self.last = None
        self.leds = None

    def start(self):
        # osmocom reads its buffer (-b, 10 ms) at a time: the phone's
        # audio must be that far ahead (it is also the extra delay the
        # base station sees)
        self.osmo.open_audio(self.prefill)
        self.r.power(True)

    def step(self, after_run=None):
        """one block: base station audio in, run the radio, its audio out;
        after_run(radio) is called between running and taking its audio
        (handset scripts, key releases)"""
        self.link.bs_audio(self.osmo.read(BLOCK))
        self.r.run(BLOCK / RATE)
        if after_run:
            after_run(self.r)
        mic = self.mic.read(BLOCK) if self.mic else None
        self.osmo.write(self.link.phone_audio(BLOCK, mic))
        ear = self.link.ear_audio(BLOCK)
        if self.ear:
            self.ear.write(ear)
        for fr in self.link.bs_frames():
            if fr != self.last:
                self.log("%8.3f BS: %s" % (self.r.time(), fr))
                self.last = fr
        leds = self.r.led_names()
        if leds != self.leds:
            self.leds = leds
            self.log("%8.3f leds %s" % (self.r.time(), " ".join(leds)))

    def close(self):
        self.osmo.close()
        for d in (self.ear, self.mic, self.hold):
            if d:
                d.close()


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


def add_audio_args(ap):
    ap.add_argument("--net-audio",
                    help="osmocom's call audio device (ALSA, e.g. pipewire)")
    ap.add_argument("--radio-out", help="the phone's earpiece/loudspeaker (ALSA)")
    ap.add_argument("--radio-in", help="the phone's microphone (ALSA)")
    ap.add_argument("--hold-music", metavar="FILE",
                    help="the network plays FILE (looped) into calls (PipeWire)")
    ap.add_argument("--rings", type=int, default=3,
                    help="ringback (425 Hz) before a call from the phone is "
                    "answered; 0 answers at once (default %(default)s, the first one partly before the earpiece opens; with "
                    "--hold-music or --net-audio)")
    ap.add_argument("--ear-gain", type=float, default=0.8,
                    help="speech at nominal deviation comes out at 0.5 of full scale")
    ap.add_argument("--mic-gain", type=float, default=1.3,
                    help="0.5 of full scale goes out at nominal deviation")
    ap.add_argument("--no-compandor", action="store_true",
                    help="the phone's audio linear (osmocom: -C 0)")


def open_audio(a):
    """(ear, mic, network end, osmocom call device) from add_audio_args
    options.  The network's microphone (or hold music) always goes
    through a NetworkEnd: ringback, and the same level scale as the
    radio's devices; its speaker is osmocom's own (nominal speech at
    0.112 of full scale)."""
    ear = SoundOut(a.radio_out) if a.radio_out else None
    mic = SoundIn(a.radio_in) if a.radio_in else None
    net = None
    if a.hold_music or a.net_audio:
        rec = a.net_audio.split("/")[-1] if a.net_audio and not a.hold_music else None
        net = NetworkEnd(a.hold_music, rec, a.rings)
    return ear, mic, net, call_device(a.net_audio, net)


def main():
    argv = sys.argv[1:]
    nmt_opts = None
    if "--" in argv:
        i = argv.index("--")
        argv, nmt_opts = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", help="ROM image, or na0+na1 for the MD50's two EPROMs")
    ap.add_argument("--nmt", required=True, help="osmocom-analog's nmt binary")
    ap.add_argument("--model", default="MD59", type=str.upper, choices=["MD59", "MD50"])
    ap.add_argument("--cu", type=str.upper, help="handset (default CU59, MD50: CU53)")
    ap.add_argument("--eeprom", help="MD59 93C06: 16 hex words, comma separated")
    ap.add_argument("--prom", help="MD50 ID PROM: hex nibbles, e.g. '8123456 1234 00000'")
    ap.add_argument("--channel", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--prefill", type=float, default=0.02,
                    help="seconds of phone audio ahead (osmocom -b 10 needs 0.01)")
    ap.add_argument("--keys", default="",
                    help="handset script, T:ACTION,... with ACTION digits/keys "
                    "to type, 'offhook' or 'onhook' (e.g. 30:0123456,32:offhook)")
    ap.add_argument("--bs-keys", default="",
                    help="console script, T:KEYS,... (osmocom: digits, d dial, "
                    "h hang up; after a roaming update d calls the phone)")
    ap.add_argument("--cold", action="store_true",
                    help="link straight after a cold start (the original MD59 "
                    "then dials the first two digits as 0)")
    ap.add_argument("--realtime", action="store_true",
                    help="run at wall-clock speed (needed for speech and osmocom's timeouts)")
    ap.add_argument("--osmo-log", help="osmocom's raw terminal output to this file")
    ap.add_argument("--verbose", action="store_true",
                    help="also osmocom's level lines (twice a second)")
    add_audio_args(ap)
    a = ap.parse_args(argv)
    a.cu = a.cu or ("CU53" if a.model == "MD50" else "CU59")
    if (a.net_audio or a.radio_out or a.radio_in or a.hold_music) and not a.realtime:
        ap.error("sound devices need --realtime")

    rom = rom_image(a.rom, a.model)
    r = Radio(rom, model=a.model, cu=a.cu, power=False)
    if rom != a.rom:
        os.unlink(rom)
    if a.eeprom:
        r.eeprom([int(w, 16) for w in a.eeprom.split(",")])
    if a.prom:
        r.prom(parse_prom(a.prom))
    nmt_opts = nmt_defaults(a.model, nmt_opts)
    if not a.cold:
        warm_boot(r)
    t_wall = time.monotonic()
    ear, mic, net, calldev = open_audio(a)
    osmo = Osmocom(a.nmt, nmt_opts, calldev, a.osmo_log, a.verbose,
                   on_line=lambda s: print("%8.3f osmo %s" % (r.time(), s), flush=True))
    if net:
        osmo.listeners.append(net.on_line)
    s = Session(r, osmo, a.channel, a.prefill, ear=ear, mic=mic, hold=net,
                log=lambda m: print(m, flush=True),
                ear_gain=a.ear_gain, mic_gain=a.mic_gain,
                compandor=not a.no_compandor and a.model != "MD50")
    script = handset_script(a.keys)
    bs_script = sorted((float(t), k) for t, k in
                       (item.split(":", 1) for item in filter(None, a.bs_keys.split(","))))

    def actions(r):
        while script and script[0][0] <= r.time():
            _, what, act = script.pop(0)
            act(r)
            if what:
                s.log("%8.3f %s" % (r.time(), what))
        while bs_script and bs_script[0][0] <= r.time():
            _, keys = bs_script.pop(0)
            s.log("%8.3f console %s" % (r.time(), keys))
            osmo.console(keys)

    def terminate(signum, frame):
        raise KeyboardInterrupt
    sig.signal(sig.SIGTERM, terminate)
    sig.signal(sig.SIGHUP, terminate)
    try:
        s.start()
        t0, e0 = time.monotonic(), r.time()
        while r.time() < a.seconds:
            s.step(actions)
            if a.realtime:
                ahead = (r.time() - e0) - (time.monotonic() - t0)
                if ahead > 0:
                    time.sleep(ahead)
    except (EOFError, KeyboardInterrupt) as e:
        s.log("stopped: %s" % (e or "interrupted"))
    finally:
        s.close()
    s.log("done: %.1f s emulated in %.1f s" % (r.time(), time.monotonic() - t_wall))


if __name__ == "__main__":
    main()
