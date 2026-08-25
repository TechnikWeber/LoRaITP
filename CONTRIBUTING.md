# Contributing to LoRaITP

The specification is a draft. Comments on it are worth more right now
than code.

## The layer boundary

This repository holds a protocol, a portable implementation and two
firmwares. They stay separable, and the boundary is enforced by
directory, not by convention:

```
SPEC.md          the wire format — normative
tools/           calculators; every number in the docs comes from here
sim/             Python reference implementation + channel simulator
src/             portable C core   <- no platform headers, no malloc
port/            platform shims    <- no protocol logic
firmware/        applications
tests/           core against the simulator, on the host
```

Two rules keep it honest:

**`src/` must compile with a plain host C compiler.** No ESP-IDF, no
Arduino, no `#ifdef` for a target. If the core needs something from the
platform, it gets a new callback in `port/loraitp_port.h`. This is what
lets the same code run in tests, in the simulator and on hardware — and
it is what makes a later split into a standalone library a directory
move rather than a rewrite.

**`port/` contains no protocol logic.** In particular, ports never
enforce a duty cycle; the governor in the core does that, and doing it
in both places corrupts the accounting.

If a change makes `src/` stop building on the host, that is the bug —
not the test that caught it.

## Numbers

Every timing, energy or budget figure in the documentation is produced
by something in `tools/` and can be re-derived by running it. Please
keep it that way: if you add a claim, add the calculation, and say which
tool produced it.

Where a figure came from a datasheet or a regulation rather than a
calculation, cite the document and its version. Regulatory tables change
between revisions, and a number without a source cannot be rechecked.

## Regulatory changes

The regional profiles in `src/loraitp_governor.c` and
[`docs/duty-cycle.md`](docs/duty-cycle.md) are derived from the German
BNetzA allocation and the underlying ETSI standard. If you add a region:

- cite the specific allocation document and its version
- state the observation window the duty cycle is measured over — it is
  not always one hour
- do not add a default frequency for an amateur band

Nobody here can verify your licence or your local rules, and the code
should not pretend otherwise. The profiles are engineering aids that
make the compliant path the easy one.

## Style

C99, four spaces, no tabs. Functions return `LORAITP_OK` or a negative
error. The core is single-threaded and reentrant per context.

Python: standard library only in `tools/`. `sim/` may use numpy.
