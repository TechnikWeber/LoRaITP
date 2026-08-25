# `port/` — platform shims

Thin adapters that fill in `loraitp_port_t`. No protocol logic.

| Port | Target | State |
|---|---|---|
| `port_sim.c` | in-memory radio for host tests | done, drives the 81 core checks |
| `port_radiolib.cpp` | SX1262 on ESP32-S3 and nRF52840 | written, passes a contract test against `tests/mock/`; **never built against the real RadioLib** |
| `port_linux_spi.c` | Linux spidev + gpiod, for a Pi with a LoRa HAT | not written |

`port_radiolib.cpp` covers every board LoRaITP targets, because RadioLib
drives the SX1262 on both chip families. Four boards therefore need four
pin maps in [`../firmware/boards/`](../firmware/boards/README.md) rather
than four drivers.

## Two rules

**Ports never enforce a duty cycle.** That is the governor's job in the
core. A port that also throttles makes the accounting wrong in a way
that is very hard to see: the core believes it has budget left, the port
silently delays, and measured airtime no longer matches the model.

**`radio_send` returns the real time on air.** Not the modelled value —
what the radio actually did. `port_radiolib.cpp` measures it between the
transmit command and the TxDone interrupt, which over-counts by the SetTx
command latency; over-counting is the safe direction for a budget. A port
that genuinely cannot measure should compute it with
`loraitp_time_on_air_us()` and say so in a comment.

## What the mocks do and do not prove

`tests/mock/RadioLib.h` is the exact API surface `port_radiolib.cpp`
depends on, and nothing else. It lets the adapter be compiled and
contract-tested on a host with no Arduino core — `cd tests && make port`
— which catches the failure mode where an adapter compiles but wires the
callbacks up wrongly, mismeasures airtime, or hangs on a timeout.

It does **not** prove the adapter matches the real RadioLib. The mock and
the adapter were written from the same understanding, so they agree by
construction. The first build against the genuine library is still the
real test, and the mock's second purpose is to be the checklist when a
RadioLib version bump breaks something.

## Things that will bite on first power-up

* **A wrong BUSY pin** gives a radio that enumerates over SPI and then
  hangs on the first command. `loraitp_radiolib_attach()` refuses a
  config with BUSY, NSS or DIO1 unset, but it cannot tell a wrong pin
  from a right one.
* **The RF switch.** Most SX1262 modules control it from DIO2, which is
  the default here; a few bring it out to a GPIO. Getting it wrong gives
  a radio that transmits into a matched load and hears nothing — which
  looks exactly like being out of range.
* **TCXO voltage.** Wrong value, no clock, no radio.
* **Sync word.** `0x12` is the private-network value. `0x34` is LoRaWAN,
  and using it would put LoRaITP traffic where it does not belong.
