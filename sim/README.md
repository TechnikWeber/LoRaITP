# `sim/` — reference implementation and channel simulator

The executable version of [SPEC.md](../SPEC.md). Where this code and the
specification disagree, one of them is wrong — and twice already it was
the specification.

```console
$ python3 sim/selftest.py        # 68 checks, no dependencies
$ python3 sim/run.py             # 12 transfer scenarios
$ python3 sim/run.py --list
$ python3 sim/run.py clean_sf10 --trace
```

Standard library only. No hardware, no waiting.

## Why this came before the firmware

A LoRa transfer at SF12 takes an hour of wall clock and, on a 1 % band,
most of a day's legal airtime. Debugging a state machine at that cadence
is not feasible — a handful of experiments a day, each confounded by
weather and interference. Here the same transfer runs in milliseconds,
with the loss rate you chose rather than the one you got, and
deterministically from a seed, which is the property that makes
debugging possible at all.

## Layout

| Path | Contents |
|---|---|
| `loraitp/frames.py` | wire format, SPEC.md §3 |
| `loraitp/session.py` | sender and receiver state machines |
| `loraitp/governor.py` | duty-cycle accounting, regional profiles |
| `loraitp/gf256.py` | Reed–Solomon erasure coding over GF(256) |
| `loraitp/aescmac.py` | AES-128 and CMAC, dependency-free |
| `loraitp/phy.py` | time on air; imports `tools/airtime.py` rather than copying it |
| `engine.py` | discrete-event scheduler for two half-duplex endpoints |
| `channel.py` | Bernoulli, Gilbert–Elliott bursts, one-way |
| `run.py` | scenarios |
| `selftest.py` | unit and property tests |

Endpoints are generators yielding `Send` / `Recv` / `Sleep`; the
scheduler advances virtual time. The part worth being careful about is
half duplex: a frame is delivered only if the peer was already listening
when the preamble started and still listening when it ended. Getting
that wrong hides exactly the class of bug the simulator exists to find —
and the first version did get it wrong, resolving transmissions before
the receiver had opened its window, so every transfer failed.

## What it found

**Two specification bugs.**

*`META` could never be authenticated.* Its MAC was specified over the
session nonce, but `META` is the frame that delivers the nonce — the
receiver does not have it yet. Every `META` was rejected. `META` is now
authenticated under a zero nonce, with replay caught by tracking seen
`(IMG_ID, NONCE)` pairs. See SPEC.md §11.

*Parity was sized wrong, and the mistake is inviting.* `parity_percent`
is a fraction of *k*, but what the code tolerates is `r/(k+r)` of the
frames transmitted. 30 % parity survives 23 % loss, not 30 %. The
`broadcast_loss20` scenario failed on its first run for exactly this
reason; `broadcast_underparity` keeps the failure visible.

**One implementation bug the spec would not have caught.** A `STAT`
bitmap is relative to the block it reports on, and the sender decoded it
against a base of zero. It only bites for block > 0 *and* only when the
bitmap encoding wins over the list, which is why the multi-block
scenario passed anyway.

## Answers to the questions this directory was set up to ask

**Does a session converge, and what does it cost?** Yes, at every loss
rate tested. 10 kB at SF10 on `EU868_G3`:

| Channel | Rounds | Frames | Airtime | Wall clock |
|---|---|---|---|---|
| clean | 1 | 52 | 93 s | 15.5 min |
| 5 % loss | 2 | 56 | 102 s | 16.6 min |
| 20 % loss | 3 | 62 | 110 s | 18.3 min |
| Gilbert–Elliott bursts | 2 | 57 | 100 s | 16.9 min |

A clean transfer costs exactly one round trip, as designed. Twenty per
cent loss costs 18 % more airtime — the repair mechanism is close to its
theoretical floor.

**Where is the bitmap/list crossover really?** At **6.2 %** of a block
for scattered losses, which confirms the ~6 % in SPEC.md §3.5. But it
depends on the *distribution*: the bitmap costs
`highest_missing/8 + 1` bytes, so for a burst at the start of a block it
wins immediately. Both encodings stay.

**Does the feasibility test abort at the right moment?** Yes. Over 200
randomised one-way links it never declared a still-recoverable block
lost, and on a hopeless link it gave up after **7 minutes instead of the
6-hour session timeout**.

**What does g3 buy?** The same SF12 transfer of a 2 kB image: **3.7
hours** on `EU868_G1`, **22 minutes** on `EU868_G3`. Identical airtime,
identical energy.

## Still open

- The 2 s time-on-air cap is still a considered guess. Settling it needs
  a packet-error-rate measurement against frame length on a real link,
  which is a hardware question the simulator cannot answer — it can only
  say what the cap costs once the curve is known.
- `T_STAT` has not been stressed against a lost `STAT` under burst loss.
- No collision model for several senders sharing a channel.
