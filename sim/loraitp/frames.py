"""
LoRaITP wire format.  SPEC.md 3.

Encoding and decoding are kept deliberately explicit - offsets spelled
out, no struct format strings shared between types - because this file
is meant to be read side by side with the specification. The C core has
to agree with it byte for byte, and tests/vectors/ exists to prove that.

All multi-byte fields are little-endian.
"""
from dataclasses import dataclass, field

from . import aescmac

VERSION = 0

META, DATA, EOB, STAT, FIN, FINACK, PROBE, PROBEACK, IDENT, PARITY, ABORT = range(1, 12)

TYPE_NAMES = {
    META: "META", DATA: "DATA", EOB: "EOB", STAT: "STAT", FIN: "FIN",
    FINACK: "FINACK", PROBE: "PROBE", PROBEACK: "PROBEACK",
    IDENT: "IDENT", PARITY: "PARITY", ABORT: "ABORT",
}

#: Frames that carry a MAC by default (SPEC.md 11.2). DATA and PARITY
#: are authenticated only when the MAC_DATA flag is set.
CONTROL_FRAMES = {META, EOB, STAT, FIN, FINACK, PROBE, PROBEACK, ABORT}

# META FLAGS bits
F_PARITY_PRESENT = 1 << 0
F_ENCRYPTED      = 1 << 1
F_LAST_LAYER     = 1 << 2
F_AMATEUR        = 1 << 3
F_BROADCAST      = 1 << 4
F_MAC_DATA       = 1 << 5

# STAT ENC values
ENC_BITMAP, ENC_LIST, ENC_COMPLETE = 0, 1, 2

MAC_LEN = 4


class FrameError(Exception):
    """Malformed frame. In the field these are simply dropped."""


def _ctrl(ftype):
    return (VERSION << 5) | ftype


def frame_type(buf):
    if len(buf) < 2:
        raise FrameError("frame shorter than the common header")
    ver = buf[0] >> 5
    if ver != VERSION:
        raise FrameError(f"unsupported version {ver}")
    return buf[0] & 0x1F


# --------------------------------------------------------------- META

@dataclass
class Meta:
    sid: int
    img_id: int
    layer: int
    img_len: int
    chunk: int
    codec: int
    crc32: int
    width: int = 0
    height: int = 0
    flags: int = 0
    block: int = 128
    n_parity: int = 0
    nonce: bytes = b"\x00\x00\x00\x00"
    tlvs: list = field(default_factory=list)   # list of (type, bytes)

    HDR = 26

    def encode(self):
        b = bytearray(self.HDR)
        b[0] = _ctrl(META)
        b[1] = self.sid & 0xFF
        b[2:4] = self.img_id.to_bytes(2, "little")
        b[4] = self.layer
        b[5:8] = self.img_len.to_bytes(3, "little")
        b[8] = self.chunk
        b[9] = self.codec
        b[10:14] = self.crc32.to_bytes(4, "little")
        b[14:16] = self.width.to_bytes(2, "little")
        b[16:18] = self.height.to_bytes(2, "little")
        b[18] = self.flags
        b[19] = 0 if self.block == 256 else self.block
        b[20:22] = self.n_parity.to_bytes(2, "little")
        b[22:26] = self.nonce
        for t, v in self.tlvs:
            b += bytes([t, len(v)]) + v
        return bytes(b)

    @classmethod
    def decode(cls, b):
        if len(b) < cls.HDR:
            raise FrameError("META truncated")
        tlvs, i = [], cls.HDR
        while i + 2 <= len(b):
            t, ln = b[i], b[i + 1]
            if i + 2 + ln > len(b):
                raise FrameError("TLV overruns frame")
            tlvs.append((t, bytes(b[i + 2:i + 2 + ln])))
            i += 2 + ln
        return cls(
            sid=b[1],
            img_id=int.from_bytes(b[2:4], "little"),
            layer=b[4],
            img_len=int.from_bytes(b[5:8], "little"),
            chunk=b[8],
            codec=b[9],
            crc32=int.from_bytes(b[10:14], "little"),
            width=int.from_bytes(b[14:16], "little"),
            height=int.from_bytes(b[16:18], "little"),
            flags=b[18],
            block=b[19] if b[19] else 256,
            n_parity=int.from_bytes(b[20:22], "little"),
            nonce=bytes(b[22:26]),
            tlvs=tlvs,
        )

    @property
    def n_chunks(self):
        return (self.img_len + self.chunk - 1) // self.chunk

    @property
    def n_blocks(self):
        return (self.n_chunks + self.block - 1) // self.block

    def block_k(self, blk):
        """Source chunks in block `blk` - the last one is usually short."""
        return min(self.block, self.n_chunks - blk * self.block)

    @property
    def total_frames(self):
        """Every DATA and PARITY frame the sender will emit. SPEC.md 3.2."""
        return self.n_chunks + self.n_blocks * self.n_parity


# --------------------------------------------------- DATA and PARITY

@dataclass
class Data:
    sid: int
    seq: int
    payload: bytes
    is_parity: bool = False

    HDR = 4

    def encode(self):
        return (bytes([_ctrl(PARITY if self.is_parity else DATA),
                       self.sid & 0xFF])
                + self.seq.to_bytes(2, "little") + self.payload)

    @classmethod
    def decode(cls, b):
        if len(b) < cls.HDR:
            raise FrameError("DATA truncated")
        return cls(sid=b[1], seq=int.from_bytes(b[2:4], "little"),
                   payload=bytes(b[4:]), is_parity=frame_type(b) == PARITY)


# ---------------------------------------------------------- EOB, STAT

@dataclass
class Eob:
    sid: int
    block: int
    round: int

    def encode(self):
        return (bytes([_ctrl(EOB), self.sid & 0xFF])
                + self.block.to_bytes(2, "little") + bytes([self.round, 0]))

    @classmethod
    def decode(cls, b):
        if len(b) < 6:
            raise FrameError("EOB truncated")
        return cls(sid=b[1], block=int.from_bytes(b[2:4], "little"), round=b[4])


@dataclass
class Stat:
    sid: int
    block: int
    round: int
    enc: int
    rssi: int
    snr_qdb: int
    missing: list = field(default_factory=list)   # absolute seq numbers
    base: int = 0                                 # first seq of the block

    def encode(self):
        if self.enc == ENC_COMPLETE:
            body = b""
        elif self.enc == ENC_LIST:
            body = b"".join(s.to_bytes(2, "little") for s in self.missing)
        else:
            rel = [s - self.base for s in self.missing]
            nbytes = (max(rel) // 8 + 1) if rel else 0
            bm = bytearray(nbytes)
            for r in rel:
                bm[r // 8] |= 1 << (r % 8)
            body = bytes(bm)
        return (bytes([_ctrl(STAT), self.sid & 0xFF])
                + self.block.to_bytes(2, "little")
                + bytes([self.round, self.enc,
                         self.rssi & 0xFF, self.snr_qdb & 0xFF, len(body)])
                + body)

    @classmethod
    def decode(cls, b, base=0):
        if len(b) < 9:
            raise FrameError("STAT truncated")
        n = b[8]
        body = bytes(b[9:9 + n])
        if len(body) != n:
            raise FrameError("STAT body truncated")
        enc = b[5]
        missing = []
        if enc == ENC_LIST:
            missing = [int.from_bytes(body[i:i + 2], "little")
                       for i in range(0, len(body), 2)]
        elif enc == ENC_BITMAP:
            missing = [base + i * 8 + bit
                       for i, byte in enumerate(body)
                       for bit in range(8) if byte & (1 << bit)]
        return cls(sid=b[1], block=int.from_bytes(b[2:4], "little"),
                   round=b[4], enc=enc,
                   rssi=b[6] - 256 if b[6] > 127 else b[6],
                   snr_qdb=b[7] - 256 if b[7] > 127 else b[7],
                   missing=missing, base=base)

    @staticmethod
    def choose_encoding(missing, base, block_len):
        """
        Pick whichever encoding is smaller (SPEC.md 3.5). The crossover
        is a claim the simulator is meant to check, not assume.
        """
        if not missing:
            return ENC_COMPLETE
        rel_max = max(m - base for m in missing)
        return ENC_LIST if len(missing) * 2 < rel_max // 8 + 1 else ENC_BITMAP


# ------------------------------------------------------ small frames

@dataclass
class Fin:
    sid: int
    def encode(self):
        return bytes([_ctrl(FIN), self.sid & 0xFF])
    @classmethod
    def decode(cls, b):
        return cls(sid=b[1])


@dataclass
class FinAck:
    sid: int
    status: int          # 0 ok, 1 crc mismatch, 2 incomplete
    def encode(self):
        return bytes([_ctrl(FINACK), self.sid & 0xFF, self.status])
    @classmethod
    def decode(cls, b):
        if len(b) < 3:
            raise FrameError("FINACK truncated")
        return cls(sid=b[1], status=b[2])


@dataclass
class Probe:
    sid: int
    sf: int
    seq: int = 0
    def encode(self):
        return bytes([_ctrl(PROBE), self.sid & 0xFF, self.sf, self.seq])
    @classmethod
    def decode(cls, b):
        if len(b) < 4:
            raise FrameError("PROBE truncated")
        return cls(sid=b[1], sf=b[2], seq=b[3])


@dataclass
class ProbeAck:
    sid: int
    rssi: int
    snr_qdb: int
    def encode(self):
        return bytes([_ctrl(PROBEACK), self.sid & 0xFF,
                      self.rssi & 0xFF, self.snr_qdb & 0xFF])
    @classmethod
    def decode(cls, b):
        if len(b) < 4:
            raise FrameError("PROBEACK truncated")
        return cls(sid=b[1],
                   rssi=b[2] - 256 if b[2] > 127 else b[2],
                   snr_qdb=b[3] - 256 if b[3] > 127 else b[3])


@dataclass
class Ident:
    sid: int
    callsign: str
    def encode(self):
        return (bytes([_ctrl(IDENT), self.sid & 0xFF])
                + self.callsign.upper().encode("ascii"))
    @classmethod
    def decode(cls, b):
        return cls(sid=b[1], callsign=bytes(b[2:]).decode("ascii", "replace"))


@dataclass
class Abort:
    sid: int
    reason: int = 0
    def encode(self):
        return bytes([_ctrl(ABORT), self.sid & 0xFF, self.reason])
    @classmethod
    def decode(cls, b):
        return cls(sid=b[1], reason=b[2] if len(b) > 2 else 0)


_DECODERS = {
    META: Meta, DATA: Data, PARITY: Data, EOB: Eob, STAT: Stat,
    FIN: Fin, FINACK: FinAck, PROBE: Probe, PROBEACK: ProbeAck,
    IDENT: Ident, ABORT: Abort,
}


# ---------------------------------------------------- authentication

ZERO_NONCE = b"\x00" * 4


def _mac_nonce(ftype, nonce):
    """
    META is authenticated under a zero nonce, everything else under the
    session nonce META carries.

    This is not an aesthetic choice. META is the frame that *delivers*
    the nonce, so a receiver verifying META does not yet have it - the
    original design was circular and simply rejected every META. The
    nonce is inside META's own authenticated bytes, so a replayed META
    is bit-identical to the original and is caught by tracking seen
    (img_id, nonce) pairs instead.
    """
    return ZERO_NONCE if ftype == META else nonce


def seal(buf, key=None, nonce=b"\x00" * 4, mac_data=False):
    """Append a truncated CMAC where SPEC.md 11.2 calls for one."""
    if key is None:
        return buf
    t = frame_type(buf)
    nonce = _mac_nonce(t, nonce)
    if t in CONTROL_FRAMES or (mac_data and t in (DATA, PARITY)):
        return buf + aescmac.tag(key, nonce, buf, MAC_LEN)
    return buf


def unseal(buf, key=None, nonce=b"\x00" * 4, mac_data=False):
    """
    Strip and verify the MAC. Raises FrameError on a bad tag, which the
    caller treats exactly like a frame that never arrived - because from
    the protocol's point of view that is what it is.
    """
    if key is None:
        return buf
    t = frame_type(buf)
    nonce = _mac_nonce(t, nonce)
    if t in CONTROL_FRAMES or (mac_data and t in (DATA, PARITY)):
        if len(buf) < MAC_LEN + 2:
            raise FrameError("frame too short to carry a MAC")
        body, got = buf[:-MAC_LEN], buf[-MAC_LEN:]
        if aescmac.tag(key, nonce, body, MAC_LEN) != got:
            raise FrameError("MAC mismatch")
        return body
    return buf


def decode(buf, key=None, nonce=b"\x00" * 4, mac_data=False, stat_base=0):
    """Full inbound path: verify, dispatch, return a frame object."""
    body = unseal(buf, key, nonce, mac_data)
    t = frame_type(body)
    cls = _DECODERS.get(t)
    if cls is None:
        raise FrameError(f"unknown frame type 0x{t:02x}")
    if cls is Stat:
        return cls.decode(body, base=stat_base)
    return cls.decode(body)
