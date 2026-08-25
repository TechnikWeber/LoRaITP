"""
PHY timing.

Deliberately imports tools/airtime.py rather than reimplementing the
time-on-air formula. Two copies of that formula would drift, and every
timing claim in the documentation is derived from that one file.
"""
import pathlib
import sys

_TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from airtime import time_on_air as _toa  # noqa: E402

# Minimum SNR the LoRa demodulator needs, per spreading factor (dB).
REQUIRED_SNR = {7: -7.5, 8: -10.0, 9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}

MAX_FRAME = 255


def time_on_air_ms(payload_len, sf, bw_hz=125000, cr=1, preamble=8):
    """Time on air of one frame, in milliseconds (float)."""
    return _toa(payload_len, sf, bw_hz, cr, preamble) * 1000.0


def max_payload_for_toa(sf, toa_ms, bw_hz=125000, cr=1):
    """Largest payload whose time on air stays within toa_ms."""
    best = 1
    for pl in range(1, MAX_FRAME + 1):
        if time_on_air_ms(pl, sf, bw_hz, cr) <= toa_ms:
            best = pl
        else:
            break
    return best


def choose_sf(snr_db, margin_db=6.0):
    """Fastest spreading factor with `margin_db` to spare. SPEC.md 9."""
    for sf in sorted(REQUIRED_SNR):
        if snr_db - REQUIRED_SNR[sf] >= margin_db:
            return sf
    return 12
