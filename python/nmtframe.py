"""
NMT signalling frames (NMT Doc 450-1): 16 hex digits (64 bits), coded
with the Hagelbarger (6,19) convolutional code to 140 bits, sent after
15 bits of bit sync and the 11-bit frame sync.

    from nmtframe import frame_1a, air_bits, to_bytes
    digits = frame_1a(channel=1, traffic_area=0x81)
    coded = encode(digits)                  # 140 bits
    bits = air_bits(digits)                 # 166 bits as sent

Written for the emulator from the format description; bit order as on
the air (MSB of each digit first).
"""

BIT_SYNC = [1, 0] * 7 + [1]                             # 15 bits
FRAME_SYNC = [1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0]          # 11 bits

# prefix P of the frames used so far (MTX -> MS)
P_1A = 12       # calling channel indication
P_1B = 4        # combined calling and traffic channel indication
P_11A = 14      # MS: roaming updating seizure and identity


def channel_450(channel, power=0):
    """12-bit NMT-450 channel field: BCD units and tens, bit 8 = 100,
    bit 11 = 200, bits 9-10 = MS power."""
    v = 0
    if channel >= 200:
        v |= 0x800
        channel -= 200
    if channel >= 100:
        v |= 0x100
        channel -= 100
    v |= (channel // 10) << 4 | channel % 10
    return v | power << 9


def decode_channel_450(v):
    return ((v & 0xF) + ((v >> 4) & 0xF) * 10 + ((v >> 8) & 1) * 100
            + ((v >> 11) & 1) * 200, (v >> 9) & 3)


def digits_to_bits(digits):
    """16 nibbles -> 64 bits, MSB first."""
    out = []
    for d in digits:
        out += [(d >> k) & 1 for k in (3, 2, 1, 0)]
    return out


def bits_to_digits(bits):
    return [bits[i] << 3 | bits[i + 1] << 2 | bits[i + 2] << 1 | bits[i + 3]
            for i in range(0, len(bits) - 3, 4)]


def hagelbarger_encode(data):
    """64 data bits -> 140 coded bits.  Six zero bits flush the register;
    each step sends a check bit (d(n) xor d(n-3) xor 1) and then the data
    bit delayed by six."""
    reg = []
    out = []
    for b in list(data) + [0] * 6:
        reg.insert(0, b)                    # reg[k] = d(n-k)
        d6 = reg[6] if len(reg) > 6 else 0
        d3 = reg[3] if len(reg) > 3 else 0
        out += [b ^ d3 ^ 1, d6]
    return out


def hagelbarger_decode(coded):
    """140 coded bits -> 64 data bits, correcting bursts of up to 6.
    The syndromes of two consecutive checks point at a wrong data bit."""
    n = len(coded) // 2
    chk = [coded[2 * i] for i in range(n)]
    dat = [coded[2 * i + 1] for i in range(n)]
    # dat[i] carries d(i-6); check i covers d(i) and d(i-3)
    d = [0] * (n - 6)                       # d[k] = data bit k

    def data(k):
        return d[k] if 0 <= k < len(d) else 0

    for k in range(len(d)):
        d[k] = dat[k + 6]
    for k in range(len(d)):
        # checks k and k+3 both involve d(k)
        s1 = chk[k] ^ data(k) ^ data(k - 3) ^ 1 if k < n else 0
        s2 = chk[k + 3] ^ data(k + 3) ^ data(k) ^ 1 if k + 3 < n else 0
        if s1 and s2:
            d[k] ^= 1
    return d


def encode(digits):
    return hagelbarger_encode(digits_to_bits(digits))


def air_bits(digits):
    return BIT_SYNC + FRAME_SYNC + encode(digits)


def to_bytes(bits):
    """MSB-first bytes, the last one padded with zeros."""
    out = []
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
        v = 0
        for b in chunk:
            v = v << 1 | b
        out.append(v)
    return out


def frame(channel, prefix, rest, power=0):
    """digits N1 N2 N3 P + 12 more"""
    c = channel_450(channel, power)
    assert len(rest) == 12
    return [(c >> 8) & 15, (c >> 4) & 15, c & 15, prefix] + list(rest)


def frame_1a(channel, traffic_area, info=(0,) * 10, power=0):
    """calling channel indication: N N N P Y Y H*10"""
    return frame(channel, P_1A, [traffic_area >> 4, traffic_area & 15] + list(info),
                 power)
