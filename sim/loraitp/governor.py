"""
The duty-cycle governor.  SPEC.md 6.

This is a normative part of the protocol, not a helper. Every
transmission is granted here before it happens, and there is no path
that transmits without asking.

Regional data is taken from BNetzA Vfg. 91/2025 table 2 (November 2025,
valid to 31.12.2035), which also fixes the definition the accounting
implements: the duty cycle is the ratio of on-time to a *rolling* one
hour window, not a calendar hour.
"""
from collections import deque
from dataclasses import dataclass

OBSERVATION_MS = 3600_000     # Tobs, per the Vfg definition
IDENT_INTERVAL_S = 540        # comfortably inside a 10 minute requirement


class RegulatoryError(Exception):
    """
    A configuration that must not be allowed to transmit. Raised, not
    warned about: amateur mode without a call sign is a bug in the
    application, and the right response is to refuse.
    """


@dataclass(frozen=True)
class Region:
    name: str
    f_lo_hz: int
    f_hi_hz: int
    duty: float | None          # None = no duty-cycle limit
    max_erp_dbm: float
    vfg_row: str = ""
    max_bw_hz: int | None = None
    amateur: bool = False
    note: str = ""

    @property
    def budget_ms(self):
        return None if self.duty is None else int(OBSERVATION_MS * self.duty)

    def off_time_ms(self, toa_ms):
        """After Ton, the band is blocked for Ton * (1/dc - 1)."""
        if self.duty is None:
            return 0.0
        return toa_ms * (1.0 / self.duty - 1.0)


REGIONS = {r.name: r for r in [
    Region("EU868_G3", 869_400_000, 869_650_000, 0.10, 27.0, "54",
           note="default: 10x the airtime and 13 dB over the LoRaWAN channels"),
    Region("EU868_G1", 868_000_000, 868_600_000, 0.01, 14.0, "48",
           note="the LoRaWAN default channels; busy"),
    Region("EU868_G2", 868_700_000, 869_200_000, 0.001, 14.0, "50",
           note="0.1% - too small a budget for images"),
    Region("EU868_G4", 869_700_000, 870_000_000, 0.01, 14.0, "56b"),
    Region("EU868_G4_LP", 869_700_000, 870_000_000, None, 7.0, "56a",
           note="5 mW but no duty limit - the right profile for bench work"),
    Region("EU433", 433_050_000, 434_790_000, 0.10, 10.0, "44b"),
    Region("EU433_NARROW", 434_040_000, 434_790_000, None, 10.0, "45c",
           max_bw_hz=25_000, note="unlimited duty, but <= 25 kHz bandwidth"),
    Region("AMATEUR", 0, 0, None, 53.0, "-", amateur=True,
           note="licence required; call sign enforced, encryption refused"),
    Region("TEST_UNRESTRICTED", 0, 0, None, 0.0, "-",
           note="dummy load or simulator only"),
]}

DEFAULT_REGION = "EU868_G3"


class Governor:
    def __init__(self, region_name, clock, frequency_hz=None,
                 bandwidth_hz=125_000, tx_power_dbm=None,
                 callsign=None, encrypted=False,
                 ident_interval_s=IDENT_INTERVAL_S):
        try:
            self.region = REGIONS[region_name]
        except KeyError:
            raise RegulatoryError(f"unknown region {region_name!r}")
        self.clock = clock
        self.callsign = callsign.upper() if callsign else None
        self.ident_interval_ms = ident_interval_s * 1000
        self._window = deque()          # (t_end_ms, toa_ms)
        self._blocked_until = 0.0
        self.airtime_total_ms = 0.0
        self.last_ident_ms = None

        self._check(frequency_hz, bandwidth_hz, tx_power_dbm, encrypted)

    # ------------------------------------------------------- validation

    def _check(self, freq, bw, power, encrypted):
        r = self.region
        if r.amateur:
            # SPEC.md 6.4. These are hard failures on purpose.
            if not self.callsign:
                raise RegulatoryError(
                    "amateur mode requires a call sign; refusing to transmit")
            if encrypted:
                raise RegulatoryError(
                    "encryption is not permitted in the amateur service")
            if not freq:
                raise RegulatoryError(
                    "amateur mode has no default frequency; the correct one "
                    "depends on licence class, country and band plan")
        else:
            if freq is not None and r.f_lo_hz and not (
                    r.f_lo_hz <= freq <= r.f_hi_hz):
                raise RegulatoryError(
                    f"{freq/1e6:.3f} MHz is outside {r.name} "
                    f"({r.f_lo_hz/1e6:.3f}-{r.f_hi_hz/1e6:.3f} MHz)")
            if power is not None and power > r.max_erp_dbm:
                raise RegulatoryError(
                    f"{power} dBm exceeds the {r.max_erp_dbm} dBm ERP limit "
                    f"of {r.name}")
        if r.max_bw_hz and bw > r.max_bw_hz:
            raise RegulatoryError(
                f"{bw/1000:g} kHz exceeds the {r.max_bw_hz/1000:g} kHz "
                f"bandwidth limit of {r.name}")

    # -------------------------------------------------------- accounting

    def _prune(self):
        cutoff = self.clock.now_ms - OBSERVATION_MS
        while self._window and self._window[0][0] <= cutoff:
            self._window.popleft()

    @property
    def airtime_in_window_ms(self):
        self._prune()
        return sum(toa for _, toa in self._window)

    def delay_before_ms(self, toa_ms):
        """
        How long the caller must wait before this frame may be sent.
        Zero means now.
        """
        if self.region.duty is None:
            return 0.0
        wait = max(0.0, self._blocked_until - self.clock.now_ms)

        # The off-time rule alone is enough to satisfy the ratio, but the
        # rolling window is what the regulation actually says, so check
        # both and take the stricter.
        budget = self.region.budget_ms
        self._prune()
        used = sum(t for _, t in self._window)
        if used + toa_ms > budget and self._window:
            need = used + toa_ms - budget
            freed = 0.0
            for t_end, t_on in self._window:
                freed += t_on
                if freed >= need:
                    wait = max(wait, t_end + OBSERVATION_MS - self.clock.now_ms)
                    break
        return wait

    def record(self, toa_ms):
        """Called after a frame has actually left the antenna."""
        now = self.clock.now_ms
        self._window.append((now, toa_ms))
        self._blocked_until = now + self.region.off_time_ms(toa_ms)
        self.airtime_total_ms += toa_ms

    # ------------------------------------------------------ amateur ID

    def ident_due(self):
        """SPEC.md 6.4: the governor injects these; callers cannot suppress."""
        if not self.region.amateur:
            return False
        if self.last_ident_ms is None:
            return True
        return self.clock.now_ms - self.last_ident_ms >= self.ident_interval_ms

    def ident_sent(self):
        self.last_ident_ms = self.clock.now_ms

    # ----------------------------------------------------------- report

    def budget(self):
        b = self.region.budget_ms
        return {
            "region": self.region.name,
            "airtime_used_ms": self.airtime_in_window_ms,
            "airtime_budget_ms": b,
            "blocked_for_ms": max(0.0, self._blocked_until - self.clock.now_ms),
            "airtime_total_ms": self.airtime_total_ms,
            "utilisation": (None if b is None
                            else self.airtime_in_window_ms / b),
        }
