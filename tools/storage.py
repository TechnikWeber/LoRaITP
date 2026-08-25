#!/usr/bin/env python3
"""
Flash budget per target board: how many images fit, and for how long.

Answers the question that decides whether this project needs an SD card
at all. Spoiler: on every supported board, it does not.

Usage:
    python3 tools/storage.py               # all boards
    python3 tools/storage.py --board heltec-v4
    python3 tools/storage.py --board heltec-v3 --no-ota
"""
import argparse

# ESP-IDF fixed overheads, in kB.
ESP_FIXED = 32 + 4 + 24 + 4          # bootloader, table, nvs, phy_init
ESP_OTADATA = 8

# An application with WiFi, an HTTP server, the LoRa driver and a
# software JPEG encoder. Generous; measured builds land under this.
ESP_APP_KB = 2048

# nRF52840: MBR + SoftDevice + Adafruit UF2 bootloader, then the app.
NRF_BOOT_KB = 156
NRF_APP_KB = 400

FS_OVERHEAD = 0.06                   # LittleFS blocks and metadata
ERASE_CYCLES = 10_000                # conservative for NOR flash


class Board:
    def __init__(self, name, mcu, flash_kb, ram, image_flash_kb=None,
                 arch="esp", camera=False, wifi=True, note=""):
        self.name, self.mcu, self.flash_kb = name, mcu, flash_kb
        self.ram, self.arch = ram, arch
        self.camera, self.wifi, self.note = camera, wifi, note
        # Where images live; None means "the same flash as the app".
        self.image_flash_kb = image_flash_kb

    def layout(self, ota=True):
        if self.arch == "nrf":
            # Images go to the separate QSPI part, so the app's flash
            # budget and the image budget are independent.
            app = NRF_BOOT_KB + NRF_APP_KB
            data = self.image_flash_kb
            return self.flash_kb, NRF_BOOT_KB, app, data
        fixed = ESP_FIXED + (ESP_OTADATA if ota else 0)
        app = ESP_APP_KB * (2 if ota else 1)
        return self.flash_kb, fixed, app, self.flash_kb - fixed - app


BOARDS = [
    Board("heltec-v3", "ESP32-S3FN8", 8 * 1024, "512 kB SRAM, PSRAM: verify",
          note="pin-compatible with V4; no camera interface broken out"),
    Board("heltec-v4", "ESP32-S3R2", 16 * 1024, "512 kB SRAM + 2 MB PSRAM",
          note="16 MB flash, 2 MB PSRAM, up to 28 dBm - cap to 27 on g3"),
    Board("xiao-esp32s3-sense", "ESP32-S3R8", 8 * 1024,
          "512 kB SRAM + 8 MB PSRAM", camera=True,
          note="OV2640 included; SD slot on the same B2B connector"),
    Board("xiao-nrf52840", "nRF52840", 1024, "256 kB SRAM",
          image_flash_kb=2048, arch="nrf", wifi=False,
          note="2 MB QSPI for images; no WiFi, no camera interface"),
]


def report(b, ota, per_day):
    total, fixed, app, data = b.layout(ota)
    usable = int(data * (1 - FS_OVERHEAD))

    print(f"\n=== {b.name}  ({b.mcu})")
    print(f"  {b.ram}")
    if b.note:
        print(f"  {b.note}")
    print(f"  camera interface: {'yes' if b.camera else 'no':3s}"
          f"   WiFi: {'yes' if b.wifi else 'no'}")
    if b.arch == "nrf":
        print(f"  app flash {total} kB internal"
              f" ({app} kB used by bootloader + app)")
        print(f"  images on {data} kB QSPI -> {usable} kB usable")
    else:
        print(f"  {total} kB flash"
              + (", two OTA slots" if ota else ", one app slot"))
        print(f"  -> data partition {data} kB -> {usable} kB usable")

    print(f"  {'image':>8} | {'images':>8} | at {per_day}/day")
    print("  " + "-" * 40)
    for kb in (5, 8, 20):
        n = usable // kb
        print(f"  {kb:6d} kB | {n:8d} | {n/(365*per_day):7.1f} years")

    five = 5 * 8
    print(f"  five 8 kB images = {five} kB = "
          f"{100.0*five/usable:.2f}% of the image store")

    per_year_mb = 8 * 365 * per_day / 1024
    budget_mb = usable * ERASE_CYCLES / 1024
    print(f"  wear at 8 kB/day: {per_year_mb:.1f} MB/year against a "
          f"{budget_mb/1024:,.0f} GB budget ({budget_mb/per_year_mb:,.0f} years)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", help="one of: "
                    + ", ".join(b.name for b in BOARDS))
    ap.add_argument("--no-ota", action="store_true",
                    help="single app slot instead of two")
    ap.add_argument("--per-day", type=int, default=1)
    a = ap.parse_args()

    sel = [b for b in BOARDS if a.board in (None, b.name)]
    if not sel:
        print(f"unknown board {a.board!r}")
        return 2
    for b in sel:
        report(b, not a.no_ota, a.per_day)

    print("\n  LittleFS wear-levels across the whole partition, so flash")
    print("  endurance is not the binding constraint anywhere. Airtime is.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
