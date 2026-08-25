# Base station

Either an ESP32 board with an OLED and an access point (Heltec V4 is the
comfortable choice: 16 MB flash, status on the display), or a Linux host
with an SX1262 on spidev.

Mains powered, listens, reassembles, writes JPEGs.

On Linux, `port/port_linux_spi.c` against an SX1262 on spidev — a
Raspberry Pi with a LoRa HAT. On an ESP32 board, the same
`port/port_radiolib.cpp` the node uses.

## Responsibilities

- continuous receive, or a scheduled window in low-power deployments
- reassembly, CRC verification, partial-image salvage
- writing `captures/<timestamp>_<img_id>.jpg` plus a sidecar JSON with
  RSSI, SNR, loss rate, repair rounds and airtime
- serving the last image over HTTP, so the thing is actually usable

That sidecar is not decoration. The measured loss rate and repair-round
count per session are what turn the spec's guessed constants into
chosen ones.
