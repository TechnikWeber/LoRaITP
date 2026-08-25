# Base station — Linux receiver

Mains powered, listens, reassembles, writes JPEGs.

Uses `port/port_linux_spi.c` against an SX1262 on spidev — a Raspberry
Pi with a LoRa HAT, or the same Heltec board acting as a USB modem.

## Responsibilities

- continuous receive, or a scheduled window in low-power deployments
- reassembly, CRC verification, partial-image salvage
- writing `captures/<timestamp>_<img_id>.jpg` plus a sidecar JSON with
  RSSI, SNR, loss rate, repair rounds and airtime
- serving the last image over HTTP, so the thing is actually usable

That sidecar is not decoration. The measured loss rate and repair-round
count per session are what turn the spec's guessed constants into
chosen ones.
