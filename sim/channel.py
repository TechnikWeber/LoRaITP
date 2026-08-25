"""
Channel models.

A simulator is only as useful as its loss model. Independent Bernoulli
loss is the easy case and the least realistic one: real LoRa links lose
frames in bursts, because fading, interference and passing vehicles all
last much longer than one frame. The Gilbert-Elliott model is here
because a protocol tuned against Bernoulli loss can be badly wrong about
burst behaviour - and the duty-cycle governor, which spreads frames
minutes apart, interacts with burst length in a way worth measuring.

Every model is seeded, so a failure is reproducible. That is the
property that makes debugging possible at all.
"""
import random


class Channel:
    """Base: perfect link. Also carries the RSSI/SNR the PROBE reports."""

    def __init__(self, rssi_dbm=-98.0, snr_db=-5.0, jitter_db=0.0, seed=0):
        self.rssi_dbm = rssi_dbm
        self.snr_db = snr_db
        self.jitter_db = jitter_db
        self.rng = random.Random(seed)
        self.delivered = 0
        self.dropped = 0

    def _measure(self):
        j = self.rng.gauss(0, self.jitter_db) if self.jitter_db else 0.0
        return self.rssi_dbm + j, self.snr_db + j

    def _lost(self, src, payload, t_ms):
        return False

    def deliver(self, src, payload, t_ms):
        rssi, snr = self._measure()
        if self._lost(src, payload, t_ms):
            self.dropped += 1
            return False, rssi, snr
        self.delivered += 1
        return True, rssi, snr

    @property
    def measured_loss(self):
        n = self.delivered + self.dropped
        return self.dropped / n if n else 0.0


class Bernoulli(Channel):
    """Independent loss. `loss` may be a float or {"A": p, "B": p}."""

    def __init__(self, loss=0.1, **kw):
        super().__init__(**kw)
        self.loss = loss

    def _p(self, src):
        return self.loss[src] if isinstance(self.loss, dict) else self.loss

    def _lost(self, src, payload, t_ms):
        return self.rng.random() < self._p(src)


class GilbertElliott(Channel):
    """
    Two-state burst model. The link sits in GOOD or BAD and switches with
    the given per-frame probabilities.

    mean_good_frames = 1 / p_gb,  mean_bad_frames = 1 / p_bg
    """

    def __init__(self, p_gb=0.02, p_bg=0.25, loss_good=0.01, loss_bad=0.7, **kw):
        super().__init__(**kw)
        self.p_gb, self.p_bg = p_gb, p_bg
        self.loss_good, self.loss_bad = loss_good, loss_bad
        self.bad = False
        self.bad_frames = 0

    def _lost(self, src, payload, t_ms):
        if self.bad:
            if self.rng.random() < self.p_bg:
                self.bad = False
        elif self.rng.random() < self.p_gb:
            self.bad = True
        if self.bad:
            self.bad_frames += 1
        p = self.loss_bad if self.bad else self.loss_good
        return self.rng.random() < p


class OneWay(Channel):
    """
    A receiver that cannot transmit at all - the broadcast case. Frames
    from B are dropped unconditionally, which is stronger than a lossy
    return path and catches any accidental reliance on feedback.
    """

    def __init__(self, forward, **kw):
        super().__init__(**kw)
        self.forward = forward

    def deliver(self, src, payload, t_ms):
        if src == "B":
            self.dropped += 1
            return False, self.rssi_dbm, self.snr_db
        return self.forward.deliver(src, payload, t_ms)

    @property
    def measured_loss(self):
        return self.forward.measured_loss
