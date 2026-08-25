# `port/` — platform shims

Thin adapters that fill in `loraitp_port_t`. Each one is expected to be
a few hundred lines and to contain no protocol logic whatsoever.

| Port | Target |
|---|---|
| `port_sim.c` | the Python/C channel simulator — no hardware |
| `port_espidf_sx126x.c` | ESP32-S3 + SX1262 under ESP-IDF |
| `port_linux_spi.c` | Linux spidev + gpiod, for a Raspberry Pi with an SX1262 HAT |

## Two rules

**Ports never enforce a duty cycle.** That is the governor's job in the
core. A port that also throttles makes the accounting wrong in a way
that is very hard to see: the core believes it has budget left, the port
silently delays, and the measured airtime no longer matches the model.

**`radio_send` returns the real time on air.** Not the modelled value —
what the radio actually did, measured between the transmit-start and
transmit-done interrupts. The governor's whole correctness rests on
this number. If a port cannot measure it, it should compute it with
`loraitp_time_on_air_us()` and say so in a comment.
