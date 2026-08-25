"""
AES-128-CMAC (RFC 4493), truncated.  SPEC.md 11.

Encryption-only AES is implemented here so the reference has no
dependencies. Every platform LoRaITP targets has AES in hardware - the
C core deliberately has no software fallback, so that nobody ships a
slow one by accident (see port/loraitp_port.h).
"""

_SBOX = None
_RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def _build_sbox():
    global _SBOX
    p = q = 1
    sbox = [0] * 256
    while True:
        # p *= 3 in GF(2^8)
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        # q /= 3
        q ^= (q << 1) & 0xFF
        q ^= (q << 2) & 0xFF
        q ^= (q << 4) & 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) \
              ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    _SBOX = sbox


_build_sbox()


def _xtime(a):
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


def _expand_key(key):
    assert len(key) == 16
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= _RCON[i // 4 - 1]
        w.append([a ^ b for a, b in zip(w[i - 4], t)])
    return w


def encrypt_block(key, block):
    """Single AES-128 block encryption. This is the whole port surface."""
    w = _expand_key(key)
    s = [list(block[i::4]) for i in range(4)]  # column-major -> rows

    def add_round_key(rnd):
        for c in range(4):
            for r in range(4):
                s[r][c] ^= w[rnd * 4 + c][r]

    add_round_key(0)
    for rnd in range(1, 11):
        for r in range(4):
            for c in range(4):
                s[r][c] = _SBOX[s[r][c]]
        for r in range(1, 4):
            s[r] = s[r][r:] + s[r][:r]
        if rnd != 10:
            for c in range(4):
                col = [s[r][c] for r in range(4)]
                t = col[0] ^ col[1] ^ col[2] ^ col[3]
                orig = list(col)
                for r in range(4):
                    s[r][c] = col[r] ^ t ^ _xtime(orig[r] ^ orig[(r + 1) % 4])
        add_round_key(rnd)

    out = bytearray(16)
    for c in range(4):
        for r in range(4):
            out[c * 4 + r] = s[r][c]
    return bytes(out)


def _shift_left(b):
    n = int.from_bytes(b, "big") << 1
    return (n & ((1 << 128) - 1)).to_bytes(16, "big")


def _subkeys(key):
    l = encrypt_block(key, b"\x00" * 16)
    k1 = _shift_left(l)
    if l[0] & 0x80:
        k1 = bytes(a ^ b for a, b in zip(k1, b"\x00" * 15 + b"\x87"))
    k2 = _shift_left(k1)
    if k1[0] & 0x80:
        k2 = bytes(a ^ b for a, b in zip(k2, b"\x00" * 15 + b"\x87"))
    return k1, k2


def cmac(key, msg):
    """Full 16-byte AES-128-CMAC."""
    k1, k2 = _subkeys(key)
    n = (len(msg) + 15) // 16

    if n == 0:
        last = bytes(a ^ b for a, b in
                     zip(b"\x80" + b"\x00" * 15, k2))
        n = 1
        blocks = []
    else:
        blocks = [msg[i * 16:(i + 1) * 16] for i in range(n)]
        tail = blocks.pop()
        if len(tail) == 16:
            last = bytes(a ^ b for a, b in zip(tail, k1))
        else:
            padded = tail + b"\x80" + b"\x00" * (15 - len(tail))
            last = bytes(a ^ b for a, b in zip(padded, k2))

    x = b"\x00" * 16
    for blk in blocks:
        x = encrypt_block(key, bytes(a ^ b for a, b in zip(x, blk)))
    return encrypt_block(key, bytes(a ^ b for a, b in zip(x, last)))


def tag(key, nonce, msg, length=4):
    """
    Truncated CMAC as LoRaITP uses it: the session nonce is prepended to
    the MAC input so a recorded session cannot be replayed into a later
    one (SPEC.md 11).
    """
    return cmac(key, nonce + msg)[:length]
