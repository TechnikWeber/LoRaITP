# `src/` — the portable core

C99. Compiles with a host compiler and no target SDK. **No `malloc`, no
vendor headers, no `#ifdef ESP32`.**

Everything platform specific reaches the core through
[`port/loraitp_port.h`](../port/loraitp_port.h). If something here needs
a facility the port does not offer, add a callback to the port — do not
add a conditional.

The rule exists for one reason: the state machine has to be debuggable.
A protocol whose timing bugs only appear over a real 30 km link at SF12
is a protocol you cannot fix, because each experiment costs an hour of
airtime. Keeping the core free of platform dependencies means the exact
same object code runs under `sim/` against injected packet loss, at
whatever speed you like.

## Layout

| File | Contents |
|---|---|
| `loraitp.h` | public API |
| `loraitp_config.h` | compile-time sizes and feature flags |
| `loraitp_frame.c` | frame encode/decode, the wire format from SPEC.md §3 |
| `loraitp_session.c` | sender and receiver state machines |
| `loraitp_governor.c` | duty-cycle accounting and the regional profile table |
| `loraitp_bitmap.c` | STAT bitmap / list encoding |
| `loraitp_fec.c` | Reed-Solomon over GF(256), erasure decoding |
| `loraitp_mac.c` | AES-128-CMAC over the port's block cipher |
| `loraitp_crc.c` | CRC-32 for the image checksum |
