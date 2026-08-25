#!/usr/bin/env python3
"""
Flash budget for an ESP32-S3 node: how many images fit, and for how long.

Answers the question that decides whether this project needs an SD card
at all. Spoiler: it does not, and not by a small margin.

Usage:
    python3 tools/storage.py
    python3 tools/storage.py --flash 4 --ota
"""
import argparse

# ESP-IDF fixed overheads, in kB.
BOOTLOADER = 32
PART_TABLE = 4
NVS = 24
PHY_INIT = 4
OTADATA = 8

# An ESP-IDF app with WiFi, an HTTP server, the LoRa driver and a
# software JPEG encoder. Measured builds land well under this.
APP_SLOT_KB = 2048

# LittleFS block overhead plus metadata, as a fraction.
FS_OVERHEAD = 0.06

# Flash endurance per sector, conservative for NOR flash.
ERASE_CYCLES = 10_000


def layout(flash_mb, ota):
    total = flash_mb * 1024
    fixed = BOOTLOADER + PART_TABLE + NVS + PHY_INIT + (OTADATA if ota else 0)
    app = APP_SLOT_KB * (2 if ota else 1)
    data = total - fixed - app
    return total, fixed, app, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flash", type=int, default=8, help="flash size in MB")
    ap.add_argument("--ota", action="store_true",
                    help="reserve two app slots for over-the-air update")
    ap.add_argument("--per-day", type=int, default=1, help="images per day")
    a = ap.parse_args()

    total, fixed, app, data = layout(a.flash, a.ota)
    usable = int(data * (1 - FS_OVERHEAD))

    print(f"\n== Partition budget, {a.flash} MB flash"
          + (", with OTA" if a.ota else ", no OTA"))
    print(f"  bootloader, table, nvs, phy   {fixed:6d} kB")
    print(f"  application {'(2 slots)' if a.ota else '(1 slot) '}         "
          f"{app:6d} kB")
    print(f"  -> data partition             {data:6d} kB")
    print(f"  -> usable after LittleFS      {usable:6d} kB")

    print(f"\n== How many images fit")
    print("  image size |   images | at 1/day lasts")
    print("  " + "-" * 44)
    for kb in (2, 5, 8, 10, 20, 50):
        n = usable // kb
        print(f"  {kb:6d} kB  | {n:8d} | {n/365:8.1f} years")

    print(f"\n== Is five images a problem?")
    for kb in (8, 20):
        used = 5 * kb
        print(f"  5 x {kb:2d} kB = {used:3d} kB "
              f"= {100.0*used/usable:.3f}% of the data partition")

    print(f"\n== Flash wear at {a.per_day} image(s)/day")
    for kb in (8, 20):
        per_year_mb = kb * 365 * a.per_day / 1024
        budget_mb = usable * ERASE_CYCLES / 1024
        print(f"  {kb:2d} kB/day -> {per_year_mb:6.1f} MB written per year; "
              f"wear-levelled budget {budget_mb/1024:,.0f} GB "
              f"({budget_mb/per_year_mb:,.0f} years)")

    print("\n  LittleFS wear-levels across the whole partition, so the")
    print("  binding constraint is not flash endurance. It is not close.")


if __name__ == "__main__":
    main()
