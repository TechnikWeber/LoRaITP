#!/usr/bin/env python3
"""
Check the web flasher's manifests against the partition tables.

These two files describe the same flash layout in different places and
different units, and nothing connects them. The manifest says where
esptool writes each image; the CSV says where the bootloader will look
for it. Get them out of step and every build passes, every test passes,
the flasher reports success, and the board does not boot - because the
app was written to an address the partition table does not call an app
partition.

That is exactly what had happened: the manifests carried the Arduino
default offsets (otadata 0xe000, app 0x10000) while this project uses a
custom table (otadata 0xf000, ota_0 0x20000). Nothing could have caught
it except somebody flashing a board.

Run with no arguments from the repository root. Exits non-zero on the
first disagreement, with the numbers.
"""
import configparser
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
FLASH = ROOT / "docs" / "flash"

# Where the second-stage bootloader goes, per chip family. The ESP32-S3
# maps flash at 0; the original ESP32 wants 0x1000.
BOOTLOADER_OFFSET = {"ESP32-S3": 0x0, "ESP32-S2": 0x1000, "ESP32": 0x1000}

PARTITION_TABLE_OFFSET = 0x8000


def parse_csv(path):
    """{name: (type, subtype, offset, size)} from an ESP-IDF partition CSV."""
    out = {}
    for line in path.read_text().splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [c.strip() for c in line.split(",")]
        if len(f) < 5:
            continue
        out[f[0]] = (f[1], f[2], int(f[3], 0), int(f[4], 0))
    return out


def envs_with_manifests():
    """Every PlatformIO env that the flasher page offers, and its CSV."""
    ini = configparser.ConfigParser()
    ini.read(FIRMWARE / "platformio.ini")
    for section in ini.sections():
        m = re.fullmatch(r"env:(.+)", section)
        if not m:
            continue
        env = m.group(1)
        manifest = FLASH / f"manifest-{env}.json"
        if not manifest.exists():
            continue          # not offered by the flasher; nothing to check
        csv = ini[section].get("board_build.partitions")
        if csv is None:
            yield env, manifest, None
        else:
            yield env, manifest, FIRMWARE / csv


def check(env, manifest_path, csv_path):
    problems = []
    manifest = json.loads(manifest_path.read_text())

    if csv_path is None:
        return [f"{env}: no board_build.partitions, but a manifest exists"]
    if not csv_path.exists():
        return [f"{env}: {csv_path} is missing"]

    parts = parse_csv(csv_path)
    if "otadata" not in parts or "ota_0" not in parts:
        return [f"{env}: {csv_path.name} has no otadata/ota_0 to check against"]

    for build in manifest["builds"]:
        family = build["chipFamily"]
        if family not in BOOTLOADER_OFFSET:
            problems.append(f"{env}: unknown chipFamily {family!r}")
            continue

        # {basename without .bin: offset}
        got = {pathlib.PurePath(p["path"]).name[:-4]: p["offset"]
               for p in build["parts"]}

        want = {
            "bootloader": BOOTLOADER_OFFSET[family],
            "partitions": PARTITION_TABLE_OFFSET,
            # boot_app0 is the initial content of the otadata partition:
            # the marker saying which slot to boot. It belongs at otadata,
            # wherever the table puts it.
            "boot_app0": parts["otadata"][2],
            # The application belongs in the first app slot, obviously,
            # and this is the one that silently bricked the board.
            "firmware": parts["ota_0"][2],
        }

        for name, expect in want.items():
            if name not in got:
                problems.append(f"{env}: manifest has no {name}.bin")
            elif got[name] != expect:
                problems.append(
                    f"{env}: {name}.bin at 0x{got[name]:x}, but "
                    f"{csv_path.name} puts it at 0x{expect:x}"
                    + (" - a board flashed from this manifest will not boot"
                       if name == "firmware" else ""))

        for name in got:
            if name not in want:
                problems.append(f"{env}: manifest writes an unexpected "
                                f"{name}.bin at 0x{got[name]:x}")
    return problems


def main():
    checked, problems = 0, []
    for env, manifest, csv in envs_with_manifests():
        checked += 1
        problems += check(env, manifest, csv)

    if checked == 0:
        print("no manifests found - is this the repository root?")
        return 1

    for p in problems:
        print(f"  FAIL {p}")
    if problems:
        print(f"\n{checked} manifest(s) checked, {len(problems)} problem(s)")
        return 1

    print(f"  ok   {checked} flasher manifest(s) agree with their "
          f"partition tables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
