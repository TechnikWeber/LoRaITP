#!/usr/bin/env python3
"""
LoRaITP airtime & duty-cycle calculator.

Implements the Semtech SX126x LoRa time-on-air formula
(SX1261/2 datasheet, rev 2.1, sec. 6.1.4) and the ETSI EN 300 220
duty-cycle budget for the EU 863-870 MHz sub-bands.

Usage:
    python3 tools/airtime.py                 # print the reference tables
    python3 tools/airtime.py --pl 200 --sf 10
"""
import argparse
import math

# ---------------------------------------------------------------- PHY layer

def n_payload_symbols(pl, sf, cr=1, crc=True, implicit_header=False, ldro=None):
    """Number of symbols in the payload part of a LoRa frame.

    pl   : PHY payload length in bytes (LoRaITP header + data)
    cr   : 1..4  ->  coding rate 4/5 .. 4/8
    ldro : low data rate optimize; None = auto (on when T_sym > 16 ms)
    """
    de = 1 if ldro else 0
    num = 8 * pl - 4 * sf + 28 + (16 if crc else 0) - (20 if implicit_header else 0)
    den = 4 * (sf - 2 * de)
    return 8 + max(math.ceil(num / den) * (cr + 4), 0)


def time_on_air(pl, sf, bw=125000, cr=1, preamble=8, crc=True,
                implicit_header=False, ldro=None):
    """Time on air in seconds for one LoRa frame."""
    t_sym = (2 ** sf) / bw
    if ldro is None:
        # Semtech: LDRO is mandated when the symbol time exceeds 16.38 ms
        ldro = t_sym > 16.38e-3
    t_preamble = (preamble + 4.25) * t_sym
    t_payload = n_payload_symbols(pl, sf, cr, crc, implicit_header, ldro) * t_sym
    return t_preamble + t_payload


def bitrate(sf, bw=125000, cr=1):
    """Nominal LoRa chip bitrate in bit/s."""
    return sf * (bw / (2 ** sf)) * (4 / (cr + 4))


# ------------------------------------------------------- regulatory profiles

class Band:
    def __init__(self, name, f_lo, f_hi, duty, erp_dbm, note=""):
        self.name, self.f_lo, self.f_hi = name, f_lo, f_hi
        self.duty, self.erp_dbm, self.note = duty, erp_dbm, note

    @property
    def airtime_per_hour(self):
        return 3600.0 * self.duty

    @property
    def airtime_per_day(self):
        return 86400.0 * self.duty


BANDS = [
    Band("EU868 g1", 868.0, 868.6, 0.01, 14, "LoRaWAN default channels"),
    Band("EU868 g2", 868.7, 869.2, 0.001, 14, "0.1% - unusable for images"),
    Band("EU868 g3", 869.4, 869.65, 0.10, 27, "10% + 500 mW ERP  <-- best EU choice"),
    Band("EU868 g4", 869.7, 870.0, 0.01, 14, ""),
    Band("HAM 70cm", 430.0, 440.0, 1.00, 53, "amateur licence, no duty cycle"),
]


# ------------------------------------------------------------------ reports

HDR = 4          # LoRaITP data-packet header, bytes
SFS = range(7, 13)


def fmt_time(s):
    if s < 1:
        return f"{s*1000:.0f} ms"
    if s < 90:
        return f"{s:.2f} s"
    if s < 5400:
        return f"{s/60:.1f} min"
    return f"{s/3600:.2f} h"


def table_per_packet(pl_list, bw=125000, cr=1):
    print(f"\n== Time on air per packet  (BW {bw//1000} kHz, CR 4/{cr+4}, 8 sym preamble, CRC on)")
    print("PL(B) | " + " | ".join(f"  SF{sf}  " for sf in SFS))
    print("-" * (8 + 10 * len(list(SFS))))
    for pl in pl_list:
        row = [f"{fmt_time(time_on_air(pl, sf, bw, cr)):>7}" for sf in SFS]
        print(f"{pl:5d} | " + " | ".join(row))


def table_goodput(pl, bw=125000, cr=1):
    print(f"\n== Net goodput  (PL {pl} B = {pl-HDR} B image data + {HDR} B header)")
    print("  SF | time/pkt |   goodput | 100%-airtime for 20 kB")
    print("-" * 58)
    for sf in SFS:
        t = time_on_air(pl, sf, bw, cr)
        gp = (pl - HDR) / t
        n = math.ceil(20000 / (pl - HDR))
        print(f"  {sf:2d} | {fmt_time(t):>8} | {gp:6.1f} B/s | {fmt_time(n*t):>10}  ({n} pkts)")


def table_budget(img_sizes, pl=200, bw=125000, cr=1, overhead=1.15):
    print(f"\n== Wall-clock transfer time incl. duty cycle"
          f"  (PL {pl} B, {int((overhead-1)*100)}% protocol overhead assumed)")
    for band in BANDS:
        print(f"\n-- {band.name}: {band.duty*100:g}% duty, {band.erp_dbm} dBm ERP"
              + (f"   [{band.note}]" if band.note else ""))
        print("  img |    SF |   airtime |  wall clock | % of daily budget")
        print("  " + "-" * 62)
        for size in img_sizes:
            for sf in (9, 10, 12):
                n = math.ceil(size / (pl - HDR))
                air = n * time_on_air(pl, sf, bw, cr) * overhead
                wall = air / band.duty
                frac = air / band.airtime_per_day * 100
                flag = "  OK" if frac <= 100 else "  OVER BUDGET"
                print(f" {size:5d} |  SF{sf:2d} | {fmt_time(air):>9} | {fmt_time(wall):>11} |"
                      f" {frac:6.1f}%{flag}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pl", type=int, help="payload length in bytes")
    ap.add_argument("--sf", type=int, help="spreading factor")
    ap.add_argument("--bw", type=int, default=125, help="bandwidth in kHz")
    ap.add_argument("--cr", type=int, default=1, help="1..4 for 4/5..4/8")
    a = ap.parse_args()

    if a.pl and a.sf:
        t = time_on_air(a.pl, a.sf, a.bw * 1000, a.cr)
        print(f"PL={a.pl} B SF{a.sf} BW{a.bw} CR4/{a.cr+4} -> {t*1000:.1f} ms "
              f"({bitrate(a.sf, a.bw*1000, a.cr):.0f} bit/s nominal)")
        return

    table_per_packet([16, 51, 100, 200, 251], a.bw * 1000, a.cr)
    table_goodput(200, a.bw * 1000, a.cr)
    table_goodput(51, a.bw * 1000, a.cr)
    table_budget([2000, 5000, 10000, 20000], 200, a.bw * 1000, a.cr)


if __name__ == "__main__":
    main()


# ---------------------------------------------------- profiles & energy

# Heltec WiFi LoRa 32 V3: ESP32-S3 + SX1262.
# Current figures are datasheet/typical values at 3.3 V, measured values will differ.
I_TX_22DBM = 118e-3   # SX1262 PA @ +22 dBm
I_TX_14DBM = 45e-3    # SX1262 PA @ +14 dBm
I_RX = 5.3e-3         # SX1262 RX boosted
I_MCU_ACTIVE = 35e-3  # ESP32-S3 awake, WiFi/BT off, radio driver running
I_SLEEP = 20e-6       # deep sleep, board-level (datasheet claims ~9 uA on bare modules)
VBAT = 3.7


def max_payload_for_toa(sf, t_max, bw=125000, cr=1):
    """Largest PHY payload whose time on air stays below t_max seconds."""
    best = 0
    for pl in range(1, 256):
        if time_on_air(pl, sf, bw, cr) <= t_max:
            best = pl
        else:
            break
    return best


def table_profiles(t_max=2.0, bw=125000, cr=1):
    print(f"\n== Suggested payload per SF  (cap time on air at {t_max:g} s per frame)")
    print("  SF | max PL | time/pkt | goodput | pkts for 10 kB")
    print("-" * 56)
    for sf in SFS:
        pl = min(max_payload_for_toa(sf, t_max, bw, cr), 251)
        t = time_on_air(pl, sf, bw, cr)
        gp = (pl - HDR) / t
        print(f"  {sf:2d} | {pl:6d} | {fmt_time(t):>8} | {gp:6.1f} B/s |"
              f" {math.ceil(10000/(pl-HDR)):6d}")


def table_energy(img_sizes, sf=12, pl=200, bw=125000, cr=1,
                 i_tx=I_TX_22DBM, overhead=1.15, rx_ratio=0.15):
    print(f"\n== Energy per image  (SF{sf}, PL {pl} B, TX {i_tx*1000:.0f} mA,"
          f" MCU {I_MCU_ACTIVE*1000:.0f} mA, RX duty {rx_ratio:.0%} of TX time)")
    print("  img |   airtime |  charge | energy | images from 3000 mAh")
    print("-" * 62)
    for size in img_sizes:
        n = math.ceil(size / (pl - HDR))
        air = n * time_on_air(pl, sf, bw, cr) * overhead
        rx = air * rx_ratio
        # MCU is awake for the whole session
        q = ((i_tx + I_MCU_ACTIVE) * air + (I_RX + I_MCU_ACTIVE) * rx) / 3600  # Ah
        mah = q * 1000
        print(f" {size:5d} | {fmt_time(air):>9} | {mah:6.2f} mAh |"
              f" {q*VBAT*1000:6.1f} mWh | {3000/mah:8.0f}")
    print(f"  (deep sleep between sessions: {I_SLEEP*1e6:.0f} uA"
          f" = {I_SLEEP*24*1000:.2f} mAh/day baseline)")
