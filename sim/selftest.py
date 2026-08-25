#!/usr/bin/env python3
"""
Self-tests for the reference implementation.

    python3 sim/selftest.py

These are the checks that were run by hand while the code was written,
kept so they cannot quietly stop being true. Several of them exist
because they caught something - those are marked REGRESSION.
"""
import pathlib
import random
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import channel as ch
from loraitp import aescmac, frames as F, gf256, phy
from loraitp.governor import Governor, RegulatoryError, REGIONS
from loraitp.session import Assembly, Config, build_meta, parity_for_loss
import run as runner

PASS, FAIL = [], []


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + (f"  {detail}" if detail else ""))


# ------------------------------------------------------------------ crypto

def test_crypto():
    print("\ncrypto")
    check("AES-128 FIPS-197 vector",
          aescmac.encrypt_block(bytes.fromhex("000102030405060708090a0b0c0d0e0f"),
                                bytes.fromhex("00112233445566778899aabbccddeeff"))
          == bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a"))
    k = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
    for msg, want in [("", "bb1d6929e95937287fa37d129b756746"),
                      ("6bc1bee22e409f96e93d7e117393172a",
                       "070a16b46b4d4144f79bdd9dd04a287c")]:
        check(f"RFC 4493 CMAC len={len(msg)//2}",
              aescmac.cmac(k, bytes.fromhex(msg)).hex() == want)


# ---------------------------------------------------------------- erasures

def test_fec():
    print("\nreed-solomon")
    rng = random.Random(7)
    k, r = 24, 10
    data = [bytes(rng.randrange(256) for _ in range(32)) for _ in range(k)]
    par = gf256.encode(data, r)
    full = {i: data[i] for i in range(k)}
    full.update({k + i: par[i] for i in range(r)})

    ok = True
    for _ in range(25):
        lost = rng.sample(range(k + r), r)
        got = gf256.decode(k, r, {i: c for i, c in full.items() if i not in lost})
        ok &= (got == data)
    check(f"recovers from exactly r={r} erasures, 25 random patterns", ok)

    lost = rng.sample(range(k + r), r + 1)
    check("returns None at r+1 erasures",
          gf256.decode(k, r, {i: c for i, c in full.items() if i not in lost}) is None)
    check("k + r <= 255 is enforced",
          _raises(lambda: gf256.cauchy_matrix(200, 100), ValueError))


def _raises(fn, exc):
    try:
        fn()
    except exc:
        return True
    except Exception:
        return False
    return False


# ------------------------------------------------------------------ frames

def test_frames():
    print("\nwire format")
    m = F.Meta(sid=3, img_id=99, layer=1, img_len=10000, chunk=196, codec=2,
               crc32=0x12345678, width=320, height=240, flags=F.F_BROADCAST,
               block=128, n_parity=20, nonce=b"\x0a\x0b\x0c\x0d",
               tlvs=[(1, b"DL1ABC")])
    check("META round trip", F.decode(m.encode()) == m)
    check("META derived counts",
          (m.n_chunks, m.n_blocks, m.total_frames) == (52, 1, 72),
          f"{m.n_chunks} chunks, {m.n_blocks} block, {m.total_frames} frames")

    for obj in [F.Data(sid=1, seq=7, payload=b"xyz"),
                F.Data(sid=1, seq=7, payload=b"xyz", is_parity=True),
                F.Eob(sid=1, block=2, round=1),
                F.Fin(sid=1), F.FinAck(sid=1, status=2),
                F.Probe(sid=1, sf=12, seq=3), F.ProbeAck(sid=1, rssi=-99, snr_qdb=-40),
                F.Ident(sid=1, callsign="DL1ABC"), F.Abort(sid=1, reason=4)]:
        check(f"{type(obj).__name__} round trip", F.decode(obj.encode()) == obj)

    check("DATA header is 4 bytes", F.Data.HDR == 4)
    d = F.Data(sid=1, seq=0, payload=bytes(196))
    check("DATA overhead at 196 B chunks is 2%",
          abs(F.Data.HDR / len(d.encode()) - 0.02) < 0.001)

    for bad in [b"", b"\x02", b"\xe0\x01\x00\x00", bytes([F.META << 0 | 1, 1])]:
        check(f"malformed frame rejected {bad.hex() or '(empty)'}",
              _raises(lambda b=bad: F.decode(b), F.FrameError))


def test_stat_encoding():
    print("\nSTAT encoding")
    base, block = 0, 128

    # The crossover depends on how the losses are spread, which is worth
    # knowing: the bitmap costs (highest_missing/8 + 1) bytes, so it is
    # cheap for a burst near the start and expensive for scattered loss.
    def spread(n):
        """n losses spread evenly across the block, so the bitmap is full size."""
        return [round(i * (block - 1) / (n - 1)) for i in range(n)] if n > 1 else [block - 1]

    scattered = next(n for n in range(1, 40)
                     if F.Stat.choose_encoding(spread(n), base, block)
                     == F.ENC_BITMAP)
    check("scattered loss: bitmap wins from ~6% of a block",
          6 <= scattered <= 10,
          f"crossover at {scattered} of {block} missing "
          f"({scattered/block:.1%}) - SPEC.md 3.5 says ~6%")

    burst = next(n for n in range(1, 40)
                 if F.Stat.choose_encoding(list(range(n)), base, block)
                 == F.ENC_BITMAP)
    check("contiguous burst at the start: bitmap wins immediately",
          burst == 1, f"crossover at {burst} missing")
    for missing in ([3, 9, 40], list(range(60)), [127]):
        for enc in (F.ENC_BITMAP, F.ENC_LIST):
            s = F.Stat(sid=1, block=0, round=0, enc=enc, rssi=-98,
                       snr_qdb=-40, missing=missing, base=base)
            back = F.decode(s.encode(), stat_base=base)
            check(f"STAT {'bitmap' if enc == 0 else 'list  '} round trip "
                  f"({len(missing)} missing, {len(s.encode())} B)",
                  back.missing == missing)


def test_mac():
    print("\nauthentication")
    key = bytes(range(16))
    nonce = b"\x01\x02\x03\x04"
    e = F.Eob(sid=1, block=0, round=0).encode()
    sealed = F.seal(e, key, nonce)
    check("MAC adds 4 bytes to a control frame", len(sealed) - len(e) == 4)
    check("valid MAC accepted", F.decode(sealed, key, nonce) is not None)
    bad = bytearray(sealed); bad[-1] ^= 0x01
    check("forged MAC rejected",
          _raises(lambda: F.decode(bytes(bad), key, nonce), F.FrameError))
    check("wrong session nonce rejected",
          _raises(lambda: F.decode(sealed, key, b"\xff\xff\xff\xff"), F.FrameError))

    # REGRESSION: META carries the nonce that its own MAC would need, so
    # verifying it under the session nonce is circular. Every META was
    # rejected until META was moved to a zero nonce.
    m = F.Meta(sid=1, img_id=1, layer=1, img_len=100, chunk=50, codec=1,
               crc32=0, nonce=nonce)
    sealed_m = F.seal(m.encode(), key, nonce)
    ok = True
    try:
        F.decode(sealed_m, key, F.ZERO_NONCE)   # what a receiver has at first
    except F.FrameError:
        ok = False
    check("REGRESSION: META verifies before its nonce is known", ok)

    d = F.Data(sid=1, seq=0, payload=b"abc").encode()
    check("DATA is unauthenticated by default",
          F.seal(d, key, nonce) == d)
    check("MAC_DATA authenticates DATA when asked",
          len(F.seal(d, key, nonce, mac_data=True)) == len(d) + 4)


# --------------------------------------------------------------- governor

def test_governor():
    print("\nduty-cycle governor")
    class Clock:
        now_ms = 0.0
    c = Clock()
    for region, toa, want in [("EU868_G1", 1850, 183150.0),
                              ("EU868_G3", 1850, 16650.0),
                              ("EU868_G2", 1850, 1848150.0)]:
        g = Governor(region, c)
        check(f"{region} off-time matches SPEC.md 6.1",
              abs(g.region.off_time_ms(toa) - want) < 1.0,
              f"{g.region.off_time_ms(toa):.0f} ms")
    check("EU868_G1 daily budget is 864 s",
          REGIONS["EU868_G1"].budget_ms * 24 / 1000 == 864)
    check("EU868_G3 daily budget is 8640 s",
          REGIONS["EU868_G3"].budget_ms * 24 / 1000 == 8640)
    check("unlimited regions report no budget",
          REGIONS["EU868_G4_LP"].budget_ms is None)

    for kw, why in [(dict(frequency_hz=433_500_000), "no call sign"),
                    (dict(callsign="DL1ABC", frequency_hz=433_500_000,
                          encrypted=True), "encryption requested"),
                    (dict(callsign="DL1ABC"), "no frequency")]:
        check(f"AMATEUR refuses: {why}",
              _raises(lambda k=kw: Governor("AMATEUR", c, **k), RegulatoryError))
    check("frequency outside the sub-band refused",
          _raises(lambda: Governor("EU868_G3", c, frequency_hz=868_100_000),
                  RegulatoryError))
    check("power above the ERP limit refused",
          _raises(lambda: Governor("EU868_G1", c, frequency_hz=868_300_000,
                                   tx_power_dbm=27), RegulatoryError))
    check("bandwidth above a narrow-band limit refused",
          _raises(lambda: Governor("EU433_NARROW", c, frequency_hz=434_500_000,
                                   bandwidth_hz=125_000), RegulatoryError))

    # the rolling window, not a calendar hour
    g = Governor("EU868_G1", c)
    c.now_ms = 0.0
    for _ in range(20):
        g.record(1800.0)
    check("rolling window accumulates", abs(g.airtime_in_window_ms - 36000) < 1)
    c.now_ms = 3600_001.0
    check("rolling window drops entries older than Tobs",
          g.airtime_in_window_ms == 0)


# ------------------------------------------------------------- phy timing

def test_phy():
    print("\nPHY timing")
    for sf, want in [(7, 102.7), (10, 616.4), (12, 2465.8)]:
        got = phy.time_on_air_ms(51, sf)
        check(f"SF{sf} 51 B time on air", abs(got - want) < 0.5,
              f"{got:.1f} ms")
    # SF9 needs -12.5 dB, so -5 dB leaves 7.5 dB - the first SF with the
    # required 6 dB. SF8 would leave only 5 dB.
    check("SF chosen with margin", phy.choose_sf(-5.0) == 9,
          f"SNR -5 dB, 6 dB margin -> SF{phy.choose_sf(-5.0)}")
    check("worst SNR falls back to SF12", phy.choose_sf(-30.0) == 12)


# -------------------------------------------------- feasibility property

def test_feasibility():
    print("\nbroadcast feasibility (SPEC.md 5.3)")
    cfg = Config(mode="broadcast", sf=10, parity_percent=40)
    image = runner.IMAGE_10K
    meta = build_meta(cfg, image)
    k = meta.block_k(0)
    total = k + meta.n_parity

    rng = random.Random(11)
    never_early = True
    for _ in range(200):
        asm = Assembly(meta)
        p = rng.uniform(0.0, 0.8)
        for ordinal in range(total):
            if rng.random() > p:
                if ordinal < k:
                    asm.src[ordinal] = b"\x00" * meta.chunk
                else:
                    asm.par[(0, ordinal - k)] = b"\x00" * meta.chunk
            asm.max_ordinal = ordinal
            if not asm.recoverable(0):
                # It claimed impossibility. Verify that even if every
                # remaining frame arrived, we would still be short.
                best = asm.have_in_block(0) + (total - ordinal - 1)
                if best >= k:
                    never_early = False
    check("never declares a recoverable block lost, 200 random links",
          never_early)

    # and the converse: it must actually notice a hopeless link
    asm = Assembly(meta)
    asm.max_ordinal = total - 1
    check("declares a genuinely hopeless block lost", not asm.recoverable(0))
    check("parity sizing solves r/(k+r)=L",
          parity_for_loss(0.20, margin=1.0) == 25,
          "20% loss needs 25% parity, not 20%")


# ------------------------------------------------------------ end to end

def test_end_to_end():
    print("\nend to end")
    for name in ("clean_sf10", "loss20_sf10", "broadcast_loss20",
                 "authenticated", "amateur_sf12"):
        cfg, image, chan = runner.SCENARIOS[name]()
        _, tx, rx, img = runner.transfer(cfg, image, chan,
                                         listen_ms=cfg.session_timeout_ms)
        check(f"{name}: image reconstructed",
              img is not None and zlib.crc32(img) == zlib.crc32(image),
              f"{rx.result}, {tx.frames_tx} frames, "
              f"{tx.airtime_ms/1000:.0f} s airtime")

    cfg, image, chan = runner.SCENARIOS["broadcast_hopeless"]()
    sim, tx, rx, img = runner.transfer(cfg, image, chan,
                                       listen_ms=cfg.session_timeout_ms)
    check("hopeless link is abandoned, not waited out",
          rx.result == "unrecoverable"
          and rx.wall_ms < cfg.session_timeout_ms / 2,
          f"gave up after {rx.wall_ms/60000:.0f} min instead of "
          f"{cfg.session_timeout_ms/3600000:.0f} h")
    check("partial image is still delivered", img is not None and len(img) == len(image))

    # determinism: same seed, same outcome
    outs = []
    for _ in range(2):
        cfg, image, chan = runner.SCENARIOS["bursty_sf10"]()
        _, tx, rx, img = runner.transfer(cfg, image, chan)
        outs.append((tx.frames_tx, rx.result, zlib.crc32(img)))
    check("seeded runs are reproducible", outs[0] == outs[1])


def main():
    for t in (test_crypto, test_fec, test_frames, test_stat_encoding,
              test_mac, test_governor, test_phy, test_feasibility,
              test_end_to_end):
        t()
    print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        for f in FAIL:
            print(f"  FAILED: {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
