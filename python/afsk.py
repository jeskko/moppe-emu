"""
Bell 202 AFSK (1200/2200 Hz, 1200 baud) + AX.25/HDLC decoder, used to
check the firmware's cycle-counted APRS transmitter in the emulator, and
the matching encoder (ax25_frame, modulate, encode) to feed packets into
an emulated receiver.
"""
import math


def tone_energy(x, rate, f, win):
    """Sliding quadrature energy of tone f over `win` samples."""
    try:
        import numpy as np
    except ImportError:
        np = None
    if np is not None:
        x = np.asarray(x, dtype=float)
        ph = 2 * np.pi * f / rate * np.arange(len(x))
        ic = np.cumsum(x * np.cos(ph))
        qs = np.cumsum(x * np.sin(ph))
        ic[win:] = ic[win:] - ic[:-win].copy()
        qs[win:] = qs[win:] - qs[:-win].copy()
        return (ic * ic + qs * qs).tolist()
    w = 2 * math.pi * f / rate
    c = [math.cos(w * i) for i in range(len(x))]
    s = [math.sin(w * i) for i in range(len(x))]
    ic = [x[i] * c[i] for i in range(len(x))]
    qs = [x[i] * s[i] for i in range(len(x))]
    out = [0.0] * len(x)
    ai = aq = 0.0
    for i in range(len(x)):
        ai += ic[i]
        aq += qs[i]
        if i >= win:
            ai -= ic[i - win]
            aq -= qs[i - win]
        out[i] = ai * ai + aq * aq
    return out


def demod_bits(x, rate=48000, baud=1200):
    """Tone decisions sampled at bit centres with transition resync.
    Returns list of 0 (2200 Hz, "space") / 1 (1200 Hz, "mark")."""
    spb = rate / baud
    win = int(spb)
    m = tone_energy(x, rate, 1200, win)
    s = tone_energy(x, rate, 2200, win)
    d = [1 if m[i] > s[i] else 0 for i in range(len(x))]
    # decision at i reflects the window ending at i; shift by half a window
    half = win // 2
    bits = []
    t = float(win)
    last = d[0]
    i = win
    while i < len(d):
        if d[i] != last:            # transition: re-centre sampling point
            last = d[i]
            t = i + spb / 2
        if i >= t:
            bits.append(d[min(len(d) - 1, i)])
            t += spb
        i += 1
    del half
    return bits


def nrzi(bits):
    """AX.25 NRZI: no change = 1, change = 0."""
    out = []
    prev = bits[0] if bits else 1
    for b in bits:
        out.append(1 if b == prev else 0)
        prev = b
    return out


def crc16_x25(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def hdlc_frames(bits):
    """Split on 0x7E flags, remove stuffing, return frames (bytes) whose
    FCS checks."""
    frames = []
    ones = 0
    cur = None
    for b in bits:
        if b:
            ones += 1
            if cur is not None:
                cur.append(1)
            if ones > 6:
                cur = None          # abort
            continue
        # b == 0
        if ones == 6:               # flag 01111110
            if cur is not None:
                data = cur[:-7]     # drop the flag's leading 0 and six 1s
                if len(data) >= 8 * 18 and len(data) % 8 == 0:
                    by = bytes(sum(data[i + k] << k for k in range(8))
                               for i in range(0, len(data), 8))
                    if crc16_x25(by[:-2]) == (by[-2] | by[-1] << 8):
                        frames.append(by[:-2])
            cur = []
        elif ones == 5:
            pass                    # stuffed zero: drop
        elif cur is not None:
            cur.append(0)
        ones = 0
    return frames


def ax25_text(frame):
    """Render an AX.25 UI frame as TNC2 text: SRC>DST,PATH:info."""
    def call(b):
        c = "".join(chr(x >> 1) for x in b[:6]).strip()
        ssid = (b[6] >> 1) & 15
        return c + ("-%d" % ssid if ssid else "")
    addrs = []
    i = 0
    while i + 7 <= len(frame):
        addrs.append(frame[i:i + 7])
        last = frame[i + 6] & 1
        i += 7
        if last:
            break
    dst, src = call(addrs[0]), call(addrs[1])
    path = [call(a) + ("*" if a[6] & 0x80 else "") for a in addrs[2:]]
    info = frame[i + 2:].decode("latin-1")
    return "%s>%s%s:%s" % (src, dst, "".join("," + p for p in path), info)


def decode(samples, rate=48000):
    if not samples:
        return []
    return [ax25_text(f) for f in hdlc_frames(nrzi(demod_bits(samples, rate)))]


# ------------------------------------------------------------------ encoder

def ax25_addr(call, last=False, repeated=False):
    """7-byte AX.25 address field for "CALL-SSID" (a trailing * marks a
    digipeater that has repeated the frame)"""
    if call.endswith("*"):
        call, repeated = call[:-1], True
    name, _, ssid = call.partition("-")
    b = bytes((ord(c) << 1) for c in name.upper().ljust(6)[:6])
    return b + bytes([0x60 | (int(ssid or 0) & 15) << 1
                      | (0x80 if repeated else 0) | (1 if last else 0)])


def ax25_frame(text):
    """UI frame (without FCS) from TNC2 text "SRC>DST,PATH1,PATH2:info"."""
    head, _, info = text.partition(":")
    src, _, rest = head.partition(">")
    calls = rest.split(",")
    dst, path = calls[0], calls[1:]
    addrs = [dst, src] + path
    out = b"".join(ax25_addr(c, last=(i == len(addrs) - 1))
                   for i, c in enumerate(addrs))
    if isinstance(info, str):
        info = info.encode("latin-1")
    return out + b"\x03\xF0" + info


def hdlc_bits(frame, preamble=32, tail=4):
    """Bits (LSB first) of flags, the frame plus its FCS with zero
    stuffing, and closing flags; before NRZI."""
    fcs = crc16_x25(frame)
    data = frame + bytes([fcs & 0xFF, fcs >> 8])
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    bits = flag * preamble
    ones = 0
    for byte in data:
        for k in range(8):
            b = byte >> k & 1
            bits.append(b)
            if b:
                ones += 1
                if ones == 5:
                    bits.append(0)
                    ones = 0
            else:
                ones = 0
    return bits + flag * tail


def nrzi_encode(bits, level=1):
    """AX.25 NRZI: a 0 toggles the tone, a 1 keeps it (1 = mark)."""
    out = []
    for b in bits:
        if not b:
            level ^= 1
        out.append(level)
    return out


def modulate(tones, rate=48000, baud=1200, amp=1.0, phase=0.0):
    """Phase-continuous Bell 202: 1 = mark 1200 Hz, 0 = space 2200 Hz."""
    out = []
    t = 0.0
    spb = rate / baud
    n = 0
    for tone in tones:
        f = 1200.0 if tone else 2200.0
        end = round((n + 1) * spb)
        while t < end:
            out.append(amp * math.sin(phase))
            phase += 2 * math.pi * f / rate
            t += 1
        n += 1
    return out


def encode(text, rate=48000, preamble=32, amp=1.0):
    """Audio samples (-amp..amp) of one AX.25 UI frame given as TNC2 text."""
    return modulate(nrzi_encode(hdlc_bits(ax25_frame(text), preamble)), rate, amp=amp)
