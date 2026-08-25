"""
Sender and receiver state machines.  SPEC.md 4 and 5.

Written as generators that yield Send / Recv / Sleep so the same code can
be driven by the simulator's virtual clock or, in principle, by a real
radio. Nothing here knows how long a millisecond is.
"""
import zlib
from dataclasses import dataclass, field

from . import frames as F
from . import gf256, phy
from .governor import Governor

import sys
import pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from engine import Recv, Send, Sleep  # noqa: E402

MODE_INTERACTIVE = "interactive"
MODE_BROADCAST = "broadcast"


def parity_for_loss(loss, margin=1.5):
    """
    Parity percentage needed to survive a given packet loss rate.

    The trap: `parity_percent` is relative to k, but what the code
    tolerates is r/(k+r) of the *transmitted* frames. So 30% parity
    survives only 23% loss, not 30% - which is exactly how the first run
    of broadcast_loss20 failed. Solving r/(k+r) = L gives r/k = L/(1-L).
    """
    loss = min(0.95, loss * margin)
    return int(round(100.0 * loss / (1.0 - loss)))


@dataclass
class Config:
    mode: str = MODE_INTERACTIVE
    region: str = "EU868_G3"
    frequency_hz: int = 869_525_000
    sf: int = 10
    bandwidth_hz: int = 125_000
    coding_rate: int = 1
    tx_power_dbm: float = 27.0
    chunk: int | None = None          # None -> derive from max_toa_ms
    max_toa_ms: float = 2000.0
    parity_percent: int = 0
    block_size: int = 128
    key: bytes | None = None
    mac_data: bool = False
    callsign: str | None = None
    encrypted: bool = False
    probe: bool = False
    max_rounds: int = 8
    eob_retry: int = 3
    meta_repeat: int = 3              # interactive; broadcast uses meta_every
    meta_every: int = 16              # broadcast: repeat META every N frames
    meta_burst: int = 3               # broadcast: META repeats at the start
    session_timeout_ms: float = 4 * 3600_000

    def chunk_len(self):
        if self.chunk:
            return self.chunk
        return max(1, phy.max_payload_for_toa(
            self.sf, self.max_toa_ms, self.bandwidth_hz,
            self.coding_rate) - F.Data.HDR)


@dataclass
class Stats:
    frames_tx: int = 0
    frames_rx: int = 0
    airtime_ms: float = 0.0
    rounds: int = 0
    retransmits: int = 0
    idents: int = 0
    wall_ms: float = 0.0
    result: str = ""
    chunks_have: int = 0
    chunks_total: int = 0
    stat_bytes: int = 0
    mac_rejects: int = 0
    blocks_lost: list = field(default_factory=list)

    @property
    def completeness(self):
        return self.chunks_have / self.chunks_total if self.chunks_total else 0.0


# --------------------------------------------------------------- helpers

def build_meta(cfg, image, sid=1, img_id=1, layer=1, codec=2,
               width=320, height=240, nonce=b"\x00\x00\x00\x00"):
    chunk = cfg.chunk_len()
    n_chunks = (len(image) + chunk - 1) // chunk
    block = min(cfg.block_size, 256)
    n_parity = 0
    if cfg.parity_percent:
        k = min(block, n_chunks)
        n_parity = max(1, round(k * cfg.parity_percent / 100))
        if k + n_parity > 255:
            n_parity = 255 - k
    flags = 0
    if n_parity:
        flags |= F.F_PARITY_PRESENT
    if cfg.mode == MODE_BROADCAST:
        flags |= F.F_BROADCAST
    if cfg.mac_data:
        flags |= F.F_MAC_DATA
    if cfg.callsign:
        flags |= F.F_AMATEUR
    tlvs = [(0x01, cfg.callsign.upper().encode())] if cfg.callsign else []
    return F.Meta(sid=sid, img_id=img_id, layer=layer, img_len=len(image),
                  chunk=chunk, codec=codec, crc32=zlib.crc32(image) & 0xFFFFFFFF,
                  width=width, height=height, flags=flags, block=block,
                  n_parity=n_parity, nonce=nonce, tlvs=tlvs)


def split(image, meta):
    """Source chunks; the last one is short and is NOT padded on the wire."""
    return [image[i * meta.chunk:(i + 1) * meta.chunk]
            for i in range(meta.n_chunks)]


def _pad(chunk, n):
    return chunk + b"\x00" * (n - len(chunk))


def frame_ordinal(meta, seq, is_parity):
    """
    Position of a frame in the sender's transmission order (SPEC.md 5.3).

    Blocks are sent in order, each as its source chunks followed by its
    parity. The receiver uses this to work out how much of the
    transmission is still to come.
    """
    if is_parity:
        blk = seq // meta.n_parity
        within = meta.block_k(blk) + (seq % meta.n_parity)
    else:
        blk = seq // meta.block
        within = seq % meta.block
    before = sum(meta.block_k(b) + meta.n_parity for b in range(blk))
    return before + within


# ---------------------------------------------------------------- sender

def sender(cfg, image, clock, sid=1, img_id=1, nonce=b"\x00\x00\x00\x04",
           stats=None):
    st = stats or Stats()
    gov = Governor(cfg.region, clock, frequency_hz=cfg.frequency_hz,
                   bandwidth_hz=cfg.bandwidth_hz,
                   tx_power_dbm=cfg.tx_power_dbm, callsign=cfg.callsign,
                   encrypted=cfg.encrypted)
    t0 = clock.now_ms

    def tx(frame_obj, _depth=0):
        """Governor-gated transmission. Nothing else may call Send."""
        # SPEC.md 6.4: the governor injects IDENT; callers cannot suppress it.
        if _depth == 0 and gov.ident_due():
            gov.ident_sent()
            st.idents += 1
            yield from tx(F.Ident(sid=sid, callsign=gov.callsign), _depth=1)
        buf = F.seal(frame_obj.encode(), cfg.key, nonce, cfg.mac_data)
        toa = phy.time_on_air_ms(len(buf), cfg.sf, cfg.bandwidth_hz,
                                 cfg.coding_rate)
        delay = gov.delay_before_ms(toa)
        if delay > 0:
            yield Sleep(delay)
        yield Send(buf, toa)
        gov.record(toa)
        st.frames_tx += 1
        st.airtime_ms += toa

    state = {"stat_base": 0}

    def rx(timeout_ms):
        got = yield Recv(timeout_ms)
        if got.frame is None:
            return None
        try:
            # A STAT bitmap is relative to the block it reports on, so the
            # base matters. It only bites for block > 0 and only when the
            # bitmap encoding wins, which is why it survived the first run.
            fr = F.decode(got.frame, cfg.key, nonce, cfg.mac_data,
                          stat_base=state["stat_base"])
        except F.FrameError:
            st.mac_rejects += 1
            return None
        st.frames_rx += 1
        return fr

    # ---- optional link probe (SPEC.md 9)
    if cfg.probe and cfg.mode == MODE_INTERACTIVE:
        probe_sf, cfg.sf = cfg.sf, 12
        yield from tx(F.Probe(sid=sid, sf=12))
        cfg.sf = probe_sf
        ans = yield from rx(4000)
        if isinstance(ans, F.ProbeAck):
            cfg.sf = phy.choose_sf(ans.snr_qdb / 4.0)

    meta = build_meta(cfg, image, sid=sid, img_id=img_id, nonce=nonce)
    chunks = split(image, meta)
    st.chunks_total = meta.n_chunks

    # ---- parity, computed per block over zero-padded chunks
    parity = {}
    if meta.n_parity:
        for b in range(meta.n_blocks):
            k = meta.block_k(b)
            src = [_pad(chunks[b * meta.block + i], meta.chunk) for i in range(k)]
            parity[b] = gf256.encode(src, meta.n_parity)

    sent_since_meta = 0

    # ================================================== broadcast mode
    if cfg.mode == MODE_BROADCAST:
        # META is the single point of failure here: lose every copy and
        # nothing else is interpretable, and there is nobody to ask.
        for _ in range(cfg.meta_burst):
            yield from tx(meta)
        for b in range(meta.n_blocks):
            k = meta.block_k(b)
            for i in range(k):
                seq = b * meta.block + i
                yield from tx(F.Data(sid=sid, seq=seq, payload=chunks[seq]))
                sent_since_meta += 1
                if sent_since_meta >= cfg.meta_every:
                    # SPEC.md 3.2: META is the only frame that makes the
                    # others interpretable, and there is nobody to ask.
                    yield from tx(meta)
                    sent_since_meta = 0
            for p in range(meta.n_parity):
                yield from tx(F.Data(sid=sid, seq=b * meta.n_parity + p,
                                     payload=parity[b][p], is_parity=True))
                sent_since_meta += 1
                if sent_since_meta >= cfg.meta_every:
                    yield from tx(meta)
                    sent_since_meta = 0
        for _ in range(3):
            yield from tx(F.Fin(sid=sid))
        st.result = "sent"
        st.wall_ms = clock.now_ms - t0
        return st

    # ================================================ interactive mode
    for _ in range(cfg.meta_repeat):
        yield from tx(meta)

    stat_toa = phy.time_on_air_ms(48, cfg.sf, cfg.bandwidth_hz, cfg.coding_rate)
    t_stat = 4 * stat_toa + 500

    for b in range(meta.n_blocks):
        k = meta.block_k(b)
        base = b * meta.block
        todo = list(range(base, base + k))
        state["stat_base"] = base
        for rnd in range(cfg.max_rounds):
            for seq in todo:
                yield from tx(F.Data(sid=sid, seq=seq, payload=chunks[seq]))
                if rnd > 0:
                    st.retransmits += 1
            st.rounds += 1

            report = None
            for _ in range(cfg.eob_retry):
                yield from tx(F.Eob(sid=sid, block=b, round=rnd))
                fr = yield from rx(t_stat)
                if isinstance(fr, F.Stat) and fr.block == b and fr.round == rnd:
                    report = fr
                    break
            if report is None:
                break                      # no feedback; move on, FIN anyway
            if report.enc == F.ENC_COMPLETE or not report.missing:
                break
            todo = [s for s in report.missing if base <= s < base + k]
            if not todo:
                break

    yield from tx(F.Fin(sid=sid))
    ack = yield from rx(t_stat * 2)
    st.result = {0: "complete", 1: "crc_mismatch", 2: "incomplete"}.get(
        ack.status, "unknown") if isinstance(ack, F.FinAck) else "no_finack"
    st.wall_ms = clock.now_ms - t0
    st.budget = gov.budget()
    return st


# -------------------------------------------------------------- receiver

class Assembly:
    """Per-image reassembly state, shared by both receiver modes."""

    def __init__(self, meta):
        self.meta = meta
        self.src = {}        # seq -> chunk bytes
        self.par = {}        # (block, index) -> chunk bytes
        self.max_ordinal = -1

    # --- accounting -------------------------------------------------
    def have_in_block(self, b):
        k = self.meta.block_k(b)
        base = b * self.meta.block
        n = sum(1 for i in range(k) if base + i in self.src)
        n += sum(1 for p in range(self.meta.n_parity) if (b, p) in self.par)
        return n

    def block_complete(self, b):
        return self.have_in_block(b) >= self.meta.block_k(b)

    def frames_sent_for_block(self, b):
        """How many of block b's frames the sender has already emitted."""
        m = self.meta
        start = sum(m.block_k(i) + m.n_parity for i in range(b))
        total = m.block_k(b) + m.n_parity
        elapsed = self.max_ordinal + 1
        return max(0, min(total, elapsed - start))

    def recoverable(self, b):
        """
        SPEC.md 5.3 - exactly decidable, not a heuristic.

        Note the estimate of `frames_sent_for_block` comes from the last
        frame we actually received, so it under-estimates how much has
        gone by. That errs towards keeping the receiver listening, which
        is the safe direction: we never abandon a block that was still
        recoverable.
        """
        m = self.meta
        total = m.block_k(b) + m.n_parity
        remaining = total - self.frames_sent_for_block(b)
        return self.have_in_block(b) + remaining >= m.block_k(b)

    def any_recoverable(self):
        return any(not self.block_complete(b) and self.recoverable(b)
                   for b in range(self.meta.n_blocks))

    def all_complete(self):
        return all(self.block_complete(b) for b in range(self.meta.n_blocks))

    def missing_in_block(self, b):
        k = self.meta.block_k(b)
        base = b * self.meta.block
        return [base + i for i in range(k) if base + i not in self.src]

    # --- output -----------------------------------------------------
    def decode_blocks(self):
        """Run RS where it is both needed and possible."""
        m = self.meta
        for b in range(m.n_blocks):
            k = m.block_k(b)
            base = b * m.block
            if all(base + i in self.src for i in range(k)):
                continue
            if not m.n_parity or self.have_in_block(b) < k:
                continue
            recv = {}
            for i in range(k):
                if base + i in self.src:
                    recv[i] = _pad(self.src[base + i], m.chunk)
            for p in range(m.n_parity):
                if (b, p) in self.par:
                    recv[k + p] = self.par[(b, p)]
            out = gf256.decode(k, m.n_parity, recv)
            if out:
                for i in range(k):
                    if base + i not in self.src:
                        self.src[base + i] = out[i]

    def image(self):
        """
        Best effort. Missing chunks become zeros - with chunk-aligned
        restart markers that is a grey band, not a decoder error.
        """
        m = self.meta
        buf = bytearray()
        for i in range(m.n_chunks):
            c = self.src.get(i, b"\x00" * m.chunk)
            buf += _pad(c, m.chunk) if i < m.n_chunks - 1 else c
        out = bytes(buf[:m.img_len])
        if len(out) < m.img_len:
            out += b"\x00" * (m.img_len - len(out))
        return out

    @property
    def n_have(self):
        return len(self.src)


def receiver(cfg, clock, listen_ms=None, stats=None):
    st = stats or Stats()
    listen_ms = listen_ms or cfg.session_timeout_ms
    t0 = clock.now_ms
    asm = None
    nonce = b"\x00\x00\x00\x00"

    gov = None
    if cfg.mode == MODE_INTERACTIVE:
        gov = Governor(cfg.region, clock, frequency_hz=cfg.frequency_hz,
                       bandwidth_hz=cfg.bandwidth_hz,
                       tx_power_dbm=cfg.tx_power_dbm, callsign=cfg.callsign,
                       encrypted=cfg.encrypted)

    def tx(frame_obj):
        buf = F.seal(frame_obj.encode(), cfg.key, nonce, cfg.mac_data)
        toa = phy.time_on_air_ms(len(buf), cfg.sf, cfg.bandwidth_hz,
                                 cfg.coding_rate)
        delay = gov.delay_before_ms(toa)
        if delay > 0:
            yield Sleep(delay)
        yield Send(buf, toa)
        gov.record(toa)
        st.frames_tx += 1
        st.airtime_ms += toa
        st.stat_bytes += len(buf) if isinstance(frame_obj, F.Stat) else 0

    last_rssi, last_snr = 0.0, 0.0

    def finish(result):
        st.result = result
        st.wall_ms = clock.now_ms - t0
        if asm:
            asm.decode_blocks()
            st.chunks_have = asm.n_have
            st.chunks_total = asm.meta.n_chunks
            st.blocks_lost = [b for b in range(asm.meta.n_blocks)
                              if not asm.block_complete(b)]
            img = asm.image()
            ok = (zlib.crc32(img) & 0xFFFFFFFF) == asm.meta.crc32
            if result == "complete" and not ok:
                st.result = "crc_mismatch"
            return st, asm, img
        return st, None, None

    while clock.now_ms - t0 < listen_ms:
        got = yield Recv(min(60_000.0, listen_ms - (clock.now_ms - t0)))
        if got.frame is None:
            if asm and cfg.mode == MODE_BROADCAST and not asm.any_recoverable():
                return finish("complete" if asm.all_complete()
                              else "unrecoverable")
            continue
        last_rssi, last_snr = got.rssi_dbm, got.snr_db
        try:
            base = asm.meta.block * 0 if asm else 0
            fr = F.decode(got.frame, cfg.key, nonce, cfg.mac_data,
                          stat_base=base)
        except F.FrameError:
            st.mac_rejects += 1
            continue
        st.frames_rx += 1

        if isinstance(fr, F.Meta):
            if asm is None or asm.meta.img_id != fr.img_id:
                asm = Assembly(fr)
                nonce = fr.nonce
            continue

        if isinstance(fr, F.Ident):
            continue

        if isinstance(fr, F.Probe):
            if gov:
                yield from tx(F.ProbeAck(sid=fr.sid, rssi=int(last_rssi),
                                         snr_qdb=int(last_snr * 4)))
            continue

        if asm is None:
            continue          # nothing is interpretable without META

        if isinstance(fr, F.Data):
            m = asm.meta
            asm.max_ordinal = max(asm.max_ordinal,
                                  frame_ordinal(m, fr.seq, fr.is_parity))
            if fr.is_parity:
                if not m.n_parity:
                    continue
                blk, idx = divmod(fr.seq, m.n_parity)
                if blk >= m.n_blocks:
                    continue          # out of range: corrupted or forged
                asm.par[(blk, idx)] = fr.payload
            else:
                if fr.seq >= m.n_chunks:
                    continue
                asm.src[fr.seq] = fr.payload
            if cfg.mode == MODE_BROADCAST:
                # Early completion: stop listening the moment the image is
                # in hand, even though parity is still being transmitted.
                if asm.all_complete():
                    return finish("complete")
                if not asm.any_recoverable():
                    return finish("unrecoverable")
            continue

        if isinstance(fr, F.Eob):
            b = fr.block
            missing = asm.missing_in_block(b)
            base = b * asm.meta.block
            enc = F.Stat.choose_encoding(missing, base, asm.meta.block_k(b))
            yield from tx(F.Stat(sid=fr.sid, block=b, round=fr.round, enc=enc,
                                 rssi=int(last_rssi), snr_qdb=int(last_snr * 4),
                                 missing=missing, base=base))
            continue

        if isinstance(fr, F.Fin):
            if cfg.mode == MODE_BROADCAST:
                return finish("complete" if asm.all_complete()
                              else ("unrecoverable" if not asm.any_recoverable()
                                    else "incomplete"))
            asm.decode_blocks()
            img = asm.image()
            ok = (zlib.crc32(img) & 0xFFFFFFFF) == asm.meta.crc32
            complete = asm.all_complete()
            status = 0 if (complete and ok) else (1 if complete else 2)
            yield from tx(F.FinAck(sid=fr.sid, status=status))
            return finish({0: "complete", 1: "crc_mismatch",
                           2: "incomplete"}[status])

    return finish("timeout")
