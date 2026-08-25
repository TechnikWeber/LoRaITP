#!/usr/bin/env python3
"""
Run LoRaITP transfers against a simulated channel.

    python3 sim/run.py                    # the full scenario set
    python3 sim/run.py --list
    python3 sim/run.py clean_sf10 --trace
"""
import argparse
import sys
import pathlib
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import channel as ch  # noqa: E402
from engine import Simulator  # noqa: E402
from loraitp import session  # noqa: E402
from loraitp.session import Config, parity_for_loss  # noqa: E402


def fmt_ms(ms):
    s = ms / 1000.0
    if s < 90:
        return f"{s:.1f} s"
    if s < 5400:
        return f"{s/60:.1f} min"
    return f"{s/3600:.2f} h"


def transfer(cfg, image, chan, trace=False, listen_ms=None):
    sim = Simulator(chan, trace=trace)
    tx_stats = session.Stats()
    rx_stats = session.Stats()
    gen_tx = session.sender(cfg, image, sim.clock, stats=tx_stats)
    gen_rx = session.receiver(cfg, sim.clock,
                              listen_ms=listen_ms or cfg.session_timeout_ms,
                              stats=rx_stats)
    res_a, res_b = sim.run(gen_tx, gen_rx)
    _, _, img = res_b if res_b else (None, None, None)
    return sim, tx_stats, rx_stats, img


def report(name, cfg, chan, sim, tx, rx, image, img_out):
    ok = img_out is not None and zlib.crc32(img_out) == zlib.crc32(image)
    verdict = "IMAGE OK" if ok else f"partial ({rx.completeness:.0%})"
    print(f"\n--- {name}")
    print(f"  mode {cfg.mode:11s} SF{cfg.sf}  chunk {cfg.chunk_len()} B  "
          f"region {cfg.region}  parity {cfg.parity_percent}%")
    print(f"  channel        {type(chan).__name__}, measured loss "
          f"{chan.measured_loss:.1%}")
    print(f"  frames         {tx.frames_tx} tx / {rx.frames_rx} rx"
          f"   retransmits {tx.retransmits}   rounds {tx.rounds}")
    print(f"  airtime        {fmt_ms(tx.airtime_ms)} sender"
          + (f" + {fmt_ms(rx.airtime_ms)} receiver" if rx.airtime_ms else ""))
    print(f"  wall clock     {fmt_ms(max(tx.wall_ms, rx.wall_ms))}")
    print(f"  result         {rx.result:14s} {verdict}"
          + (f"   blocks lost {rx.blocks_lost}" if rx.blocks_lost else ""))
    if rx.mac_rejects or tx.mac_rejects:
        print(f"  MAC rejects    {rx.mac_rejects + tx.mac_rejects}")
    if tx.idents:
        print(f"  IDENT frames   {tx.idents} (injected by the governor)")
    return ok


IMAGE_10K = bytes((i * 37 + (i >> 5) * 11) & 0xFF for i in range(10_000))
IMAGE_2K = IMAGE_10K[:2_000]

SCENARIOS = {}


def scenario(fn):
    SCENARIOS[fn.__name__] = fn
    return fn


@scenario
def clean_sf10():
    """A working link. One round trip, no repairs - the design claim."""
    return Config(sf=10), IMAGE_10K, ch.Channel(seed=1)


@scenario
def loss5_sf10():
    """5% independent loss. Should converge in two rounds."""
    return Config(sf=10), IMAGE_10K, ch.Bernoulli(0.05, seed=2)


@scenario
def loss20_sf10():
    """20% loss. Tests that the repair loop actually terminates."""
    return Config(sf=10), IMAGE_10K, ch.Bernoulli(0.20, seed=3)


@scenario
def bursty_sf10():
    """Gilbert-Elliott bursts - the realistic case, and the awkward one."""
    return Config(sf=10), IMAGE_10K, ch.GilbertElliott(seed=4)


@scenario
def sf12_g1_duty():
    """
    SF12 on the 1% band. The point is the wall clock: this is where the
    duty-cycle governor dominates everything else.
    """
    return (Config(sf=12, region="EU868_G1", frequency_hz=868_300_000,
                   tx_power_dbm=14, session_timeout_ms=30 * 3600_000),
            IMAGE_2K, ch.Bernoulli(0.05, seed=5))


@scenario
def sf12_g3_duty():
    """The same transfer on g3. Same airtime, a tenth of the wall clock."""
    return (Config(sf=12, region="EU868_G3", session_timeout_ms=30 * 3600_000),
            IMAGE_2K, ch.Bernoulli(0.05, seed=5))


@scenario
def broadcast_clean():
    """One way, no return channel, good link. Should stop early."""
    return (Config(mode="broadcast", sf=10, parity_percent=30),
            IMAGE_10K, ch.OneWay(ch.Channel(seed=6)))


@scenario
def broadcast_loss20():
    """
    One way at 20% loss, parity sized for it.

    The first version of this scenario used 30% parity and failed, which
    was the simulator being right: parity is a fraction of k, but what
    the code tolerates is r/(k+r). 30% parity survives 23% loss, not 30%.
    """
    return (Config(mode="broadcast", sf=10,
                   parity_percent=parity_for_loss(0.20)),
            IMAGE_10K, ch.OneWay(ch.Bernoulli(0.20, seed=7)))


@scenario
def broadcast_underparity():
    """
    The same 20% link with parity sized by the naive reading (20%).
    Expected to fail - it is here so the failure stays visible.
    """
    return (Config(mode="broadcast", sf=10, parity_percent=20),
            IMAGE_10K, ch.OneWay(ch.Bernoulli(0.20, seed=7)))


@scenario
def broadcast_hopeless():
    """
    One way at 60% loss with 15% parity. Recovery is impossible, and the
    interesting question is whether the receiver works that out and
    powers down instead of listening to the end.
    """
    return (Config(mode="broadcast", sf=10, parity_percent=15,
                   session_timeout_ms=6 * 3600_000),
            IMAGE_10K, ch.OneWay(ch.Bernoulli(0.60, seed=8)))


@scenario
def authenticated():
    """CMAC on control frames, plus a probe to pick the spreading factor."""
    return (Config(sf=10, key=bytes(range(16)), probe=True),
            IMAGE_10K, ch.Bernoulli(0.05, seed=9, snr_db=-8.0))


@scenario
def amateur_sf12():
    """Amateur mode: no duty cycle, but IDENT is injected automatically."""
    return (Config(sf=12, region="AMATEUR", frequency_hz=433_500_000,
                   callsign="DL1ABC", tx_power_dbm=30,
                   session_timeout_ms=6 * 3600_000),
            IMAGE_10K, ch.Bernoulli(0.05, seed=10))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", nargs="*", help="names; default is all")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--trace", action="store_true")
    a = ap.parse_args()

    if a.list:
        for n, fn in SCENARIOS.items():
            print(f"{n:20s} {(fn.__doc__ or '').strip().splitlines()[0]}")
        return 0

    names = a.scenario or list(SCENARIOS)
    failures = []
    for n in names:
        if n not in SCENARIOS:
            print(f"unknown scenario {n!r}")
            return 2
        cfg, image, chan = SCENARIOS[n]()
        sim, tx, rx, img = transfer(cfg, image, chan, trace=a.trace,
                                    listen_ms=cfg.session_timeout_ms)
        ok = report(n, cfg, chan, sim, tx, rx, image, img)
        expect_ok = not any(k in n for k in ("hopeless", "underparity"))
        if ok != expect_ok:
            failures.append(n)
        if a.trace:
            for t, who, what in sim.log[:60]:
                print(f"    {t/1000:9.2f}s {who} {what}")

    print()
    if failures:
        print(f"UNEXPECTED OUTCOME in: {', '.join(failures)}")
        return 1
    print(f"{len(names)} scenario(s) behaved as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
