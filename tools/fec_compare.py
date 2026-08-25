#!/usr/bin/env python3
"""
Broadcast mode: is it better to send the image three times, or to send
it once with twice as much parity?

Both cost exactly the same airtime. This script answers the question
numerically for a range of packet loss rates.

Usage:
    python3 tools/fec_compare.py
    python3 tools/fec_compare.py --k 52 --budget 3.0
"""
import argparse
from math import comb


def p_complete_repetition(k, p, reps):
    """All k chunks arrive at least once across `reps` independent passes."""
    p_chunk_lost = p ** reps
    return (1.0 - p_chunk_lost) ** k


def p_complete_rs(k, r, p):
    """An ideal (k+r, k) erasure code decodes if >= k of the k+r frames land."""
    n = k + r
    q = 1.0 - p
    # P(received >= k) = sum_{i=k}^{n} C(n,i) q^i p^(n-i)
    return sum(comb(n, i) * (q ** i) * (p ** (n - i)) for i in range(k, n + 1))


def expected_chunks(k, p, reps):
    """Fraction of source chunks in hand, for partial-image quality."""
    return 1.0 - p ** reps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--k", type=int, default=52,
                    help="source chunks (52 = 10 kB at 196 B chunks)")
    ap.add_argument("--budget", type=float, default=3.0,
                    help="airtime budget as a multiple of the image")
    a = ap.parse_args()

    k, b = a.k, a.budget
    reps = int(round(b))
    r = int(round(k * (b - 1)))   # same total airtime as `reps` repetitions

    print(f"\n== {k} source chunks, airtime budget = {b:g}x the image")
    print(f"   repetition: send it {reps}x            = {reps*k} frames")
    print(f"   erasure code: k={k}, r={r}  (RS/fountain) = {k+r} frames\n")
    def pct(x):
        """Readable failure probability across 12 orders of magnitude."""
        if x <= 0:
            return "     0"
        if x < 1e-6:
            return f"{x:.0e}".replace("e-0", "e-")
        return f"{x*100:6.3f}%"

    print("  loss | P(fail) repetition | P(fail) erasure code")
    print("  " + "-" * 48)
    for p in (0.02, 0.05, 0.10, 0.20, 0.30, 0.40, 0.50, 0.60):
        rep = 1.0 - p_complete_repetition(k, p, reps)
        rs = 1.0 - p_complete_rs(k, r, p)
        print(f"  {p:4.0%} | {pct(rep):>18} | {pct(rs):>20}")

    print("\n  'Failure' means the image cannot be fully reconstructed.")
    print("  Both columns cost identical airtime and identical energy.")

    print(f"\n== Partial recovery (systematic codes keep source chunks readable)")
    print("  loss | source chunks in hand after 1 pass | after 3 passes")
    print("  " + "-" * 56)
    for p in (0.10, 0.20, 0.40):
        print(f"  {p:4.0%} | {expected_chunks(k,p,1):33.1%} |"
              f" {expected_chunks(k,p,3):14.1%}")


if __name__ == "__main__":
    main()
