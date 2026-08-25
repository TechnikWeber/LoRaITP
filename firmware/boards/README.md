# Board pin maps

One header per board, selected with `-DLORAITP_BOARD_<NAME>`. This is
the only genuinely board-specific part of the firmware; everything above
it is shared.

## Verification status — read this before powering anything

Pin numbers are the one thing in this repository that cannot be derived
or computed. They have to be looked up, and a wrong one gives you a radio
that enumerates over SPI and then hangs waiting on BUSY, which is a
miserable thing to debug.

| Board | LoRa SPI | LoRa control pins | Peripherals |
|---|---|---|---|
| `heltec_v3.h` | documented, stable | documented, stable | documented |
| `heltec_v4.h` | **copied from V3** | **copied from V3** | **copied from V3** |
| `xiao_esp32s3_sense.h` | fixed by the XIAO edge pinout | **read off the module silkscreen** | n/a |
| `xiao_nrf52840.h` | fixed by the XIAO edge pinout | **TODO — placeholder** | n/a |

**Heltec V4** is stated by the vendor to keep the V3 form factor and pin
layout, so the map starts as a copy — but "pin compatible" on a product
page is not the same as every GPIO landing in the same place, and it
should be checked before first power-up.

**The XIAO ESP32S3 Sense map is confirmed** against the silkscreen of a
Wio-SX1262 for XIAO — the plug-on variant with two 7-pin sockets, not the
Kit board that goes on the B2B connector:

| Signal | XIAO | GPIO | | Signal | XIAO | GPIO |
|---|---|---|---|---|---|---|
| MOSI | D10 | 9 | | DIO1 | D1 | 2 |
| MISO | D9 | 8 | | RST | D2 | 3 |
| SCK | D8 | 7 | | BUSY | D3 | 4 |
| | | | | NSS | D4 | 5 |
| | | | | RF_SW | D5 | 6 |

Two consequences worth carrying forward:

* **`RF_SW` is a host-driven pin.** This module does not steer its
  antenna switch from DIO2. `dio2_as_rf_switch` must be false and GPIO6
  driven, or the radio transmits into a matched load and hears nothing.
  The polarity used — high to transmit — is the convention rather than
  something measured; if the link works one way only, invert it first.
* **`RST` lands on GPIO3, which is the Sense SD card's chip select** on
  the same SPI bus. Leave the card slot empty, or move `RST` to D0
  (GPIO1) with a jumper. Images live in internal flash regardless.

**The XIAO nRF52840 pins are still placeholders.** Its Wio-SX1262 uses
through-hole headers and a different mapping again. See
[../../docs/hardware.md](../../docs/hardware.md).

Good sources, in order: the vendor schematic, Meshtastic's variant file
for the board, RadioLib's examples. Not a forum post.

## Power ceilings

`max_tx_dbm` is what the hardware can produce, not what you may use. The
duty-cycle governor refuses a configured power above the region's limit
(`LORAITP_E_POWER`), which is the check that matters — but note the
Heltec V4 high-power variant reaches 28 dBm, above the 27 dBm ERP ceiling
of `EU868_G3`, and that antenna gain counts towards ERP as well.
