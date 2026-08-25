# `tests/`

The core compiled with a host compiler and exercised through
`port_sim.c`. No hardware, no radio, no waiting.

- `test_frame.c` — encode/decode round trips, truncated and malformed frames
- `test_governor.c` — duty-cycle accounting against hand-computed budgets,
  including the sliding-window boundary and the 49-day millisecond wrap
- `test_bitmap.c` — bitmap/list encoding, and the size crossover
- `test_fec.c` — Reed-Solomon recovers from exactly `r` erasures and
  fails predictably at `r+1`
- `test_session.c` — full transfers against injected loss patterns
- `vectors/` — frame test vectors shared with the Python implementation,
  so the two cannot drift apart silently
