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
| `xiao_esp32s3_sense.h` | fixed by the XIAO edge pinout | **TODO — placeholder** | n/a |
| `xiao_nrf52840.h` | fixed by the XIAO edge pinout | **TODO — placeholder** | n/a |

**Heltec V4** is stated by the vendor to keep the V3 form factor and pin
layout, so the map starts as a copy — but "pin compatible" on a product
page is not the same as every GPIO landing in the same place, and it
should be checked before first power-up.

**The XIAO control pins are placeholders.** `NSS`, `RST`, `BUSY` and
`DIO1` are marked `TODO` and currently hold arbitrary values. They must
come from the Wio-SX1262 pinout for the exact variant you have, because
Seeed sells two boards under the same name that use *different* pins:
the Kit version over the B2B connector, and the non-Kit version over
soldered headers. See [../../docs/hardware.md](../../docs/hardware.md).

Good sources, in order: the vendor schematic, Meshtastic's variant file
for the board, RadioLib's examples. Not a forum post.

## Power ceilings

`max_tx_dbm` is what the hardware can produce, not what you may use. The
duty-cycle governor refuses a configured power above the region's limit
(`LORAITP_E_POWER`), which is the check that matters — but note the
Heltec V4 high-power variant reaches 28 dBm, above the 27 dBm ERP ceiling
of `EU868_G3`, and that antenna gain counts towards ERP as well.
