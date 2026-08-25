#!/usr/bin/env python3
"""
LoRaITP link budget and path geometry calculator.

At the distances LoRaITP targets the limiting factor is almost never
transmit power - it is whether the path is geometrically clear. This
tool prints both so the two can be compared directly.

Usage:
    python3 tools/linkbudget.py
    python3 tools/linkbudget.py --dist 30 --freq 868 --erp 27
"""
import argparse
import math

# SX126x LoRa demodulator, BW 125 kHz (datasheet typical)
SENSITIVITY = {7: -123, 8: -126, 9: -129, 10: -132, 11: -134.5, 12: -137}
REQUIRED_SNR = {7: -7.5, 8: -10.0, 9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}


def fspl_db(dist_km, freq_mhz):
    """Free space path loss."""
    return 32.45 + 20 * math.log10(freq_mhz) + 20 * math.log10(dist_km)


def fresnel_radius_m(dist_km, freq_mhz, frac=0.5):
    """Radius of the first Fresnel zone at `frac` along the path."""
    d1 = dist_km * frac
    d2 = dist_km - d1
    return 17.32 * math.sqrt((d1 * d2) / ((freq_mhz / 1000.0) * dist_km))


def earth_bulge_m(dist_km, frac=0.5, k=4 / 3):
    """Apparent earth bulge at `frac` along the path, k-factor corrected."""
    d1 = dist_km * frac
    d2 = dist_km - d1
    return (d1 * d2) / (12.75 * k)


def report(dist_km, freq_mhz, erp_dbm, rx_gain_dbi, rx_loss_db):
    eirp = erp_dbm + 2.15
    loss = fspl_db(dist_km, freq_mhz)
    rx_level = eirp - loss + rx_gain_dbi - rx_loss_db

    print(f"\n== Link budget: {dist_km:g} km @ {freq_mhz:g} MHz")
    print(f"  TX ERP                 {erp_dbm:7.1f} dBm  ({eirp:.1f} dBm EIRP)")
    print(f"  free space path loss   {loss:7.1f} dB")
    print(f"  RX antenna gain        {rx_gain_dbi:7.1f} dBi")
    print(f"  RX feedline loss       {-rx_loss_db:7.1f} dB")
    print(f"  -> received level      {rx_level:7.1f} dBm")
    print()
    print("  SF | sensitivity | margin")
    print("  " + "-" * 32)
    for sf in sorted(SENSITIVITY):
        m = rx_level - SENSITIVITY[sf]
        flag = "OK" if m > 10 else ("tight" if m > 0 else "NO LINK")
        print(f"  {sf:2d} | {SENSITIVITY[sf]:8.1f} dBm | {m:6.1f} dB  {flag}")

    print(f"\n== Path geometry (the part that actually limits {dist_km:g} km)")
    r = fresnel_radius_m(dist_km, freq_mhz)
    b = earth_bulge_m(dist_km)
    print(f"  1st Fresnel zone radius at midpoint   {r:6.1f} m")
    print(f"  earth bulge at midpoint (k=4/3)       {b:6.1f} m")
    print(f"  clearance needed at midpoint (60% F1) {0.6*r + b:6.1f} m")
    print(f"  radio horizon for a {r+b:.0f} m mast          "
          f"{4.12*math.sqrt(r+b):6.1f} km")
    print("\n  Free space loss is the optimistic case. A treeline in the")
    print("  path can cost 30 dB; a hill costs everything. Height first.")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dist", type=float, default=30.0, help="km")
    p.add_argument("--freq", type=float, default=868.0, help="MHz")
    p.add_argument("--erp", type=float, default=27.0, help="dBm ERP")
    p.add_argument("--rxgain", type=float, default=6.0, help="dBi")
    p.add_argument("--rxloss", type=float, default=2.0, help="dB")
    a = p.parse_args()
    report(a.dist, a.freq, a.erp, a.rxgain, a.rxloss)


if __name__ == "__main__":
    main()
