# `sim/` — reference implementation and channel simulator

Build this before any firmware.

## Why first

A LoRa transfer at SF12 takes an hour of wall-clock time and, on a 1 %
band, most of a day's legal airtime. Debugging a state machine at that
cadence is not feasible: you get a handful of experiments per day, each
confounded by weather, interference and whatever else moved.

The simulator runs the same transfer in milliseconds, with packet loss
you chose rather than packet loss you got, and it does it
deterministically from a seed — so a failure is reproducible, which is
the property that makes debugging possible at all.

It also has to exist before the spec can be trusted. Every timing
constant in [SPEC.md](../SPEC.md) is currently a considered guess.

## Contents

| Path | Purpose |
|---|---|
| `loraitp/` | Python reference implementation of the wire format and both state machines |
| `channel.py` | loss models: Bernoulli, Gilbert-Elliott bursts, collisions, fades |
| `scenarios/` | reproducible test cases, seeded |
| `replay.py` | decode a capture from real hardware and step through it |

## What it must answer before firmware starts

1. Does a session converge at 0 %, 5 %, 20 % and 40 % loss, and how much
   airtime does convergence cost at each?
2. Is `T_STAT` long enough when the `STAT` frame itself is lost?
3. Where is the real crossover between the bitmap and list encodings?
4. In broadcast mode, does the feasibility test abort at the right
   moment — never while recovery was still possible, and promptly when
   it was not?
5. What does the 2 s time-on-air cap actually cost, and is it the right
   number? This is the one constant most likely to be wrong.

The Python implementation is normative in the sense that it is the
executable version of the spec. Where the C core and the Python
disagree, the disagreement is a bug in one of them, and the simulator
should be able to say which.
