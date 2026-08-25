"""
A small discrete-event scheduler for two half-duplex LoRa endpoints.

Both endpoints are generators that yield actions - Send, Recv, Sleep -
and the scheduler advances virtual time. There are no threads and no
real sleeping, so a transfer that takes six hours of wall clock on a 1%
band runs here in well under a second, and it runs the *same* state
machine.

The one property worth being careful about is half duplex. A frame is
delivered only if the peer was already listening when the preamble
started and was still listening when the frame ended. An endpoint that
is transmitting, sleeping, or that opened its window late simply does
not hear it. Getting that wrong would hide exactly the class of bug the
simulator exists to find.
"""
from dataclasses import dataclass


@dataclass
class Send:
    payload: bytes
    toa_ms: float


@dataclass
class Recv:
    timeout_ms: float


@dataclass
class Sleep:
    ms: float


@dataclass
class Rx:
    """What a Recv resolves to. `frame` is None on timeout."""
    frame: bytes | None
    rssi_dbm: float = 0.0
    snr_db: float = 0.0


class Clock:
    def __init__(self):
        self.now_ms = 0.0


class _Proc:
    def __init__(self, name, gen, clock):
        self.name = name
        self.gen = gen
        self.clock = clock
        self.t = 0.0
        self.window = None       # (start_ms, end_ms) while blocked in Recv
        self.pending = None      # an action we could not resolve yet
        self.done = False
        self.result = None
        self._send_value = None

    @property
    def listening(self):
        return self.window is not None


class Simulator:
    def __init__(self, channel, trace=False, max_events=5_000_000):
        self.channel = channel
        self.trace = trace
        self.log = []
        self.max_events = max_events
        self.clock = Clock()
        self.frames_sent = 0
        self.frames_lost = 0

    def _note(self, t, who, what):
        if self.trace:
            self.log.append((t, who, what))

    def run(self, gen_a, gen_b):
        a = _Proc("A", gen_a, self.clock)
        b = _Proc("B", gen_b, self.clock)
        procs = {"A": a, "B": b}
        peer = {"A": b, "B": a}

        for _ in range(self.max_events):
            live = [p for p in procs.values() if not p.done]
            if not live:
                break

            runnable = [p for p in live if not p.listening]
            if runnable:
                p = min(runnable, key=lambda x: x.t)
                if not self._step(p, peer[p.name]):
                    # p wants to transmit but the peer has not yet reached
                    # that instant, so we do not know whether it is
                    # listening. Resolve the peer first, then retry.
                    q = peer[p.name]
                    if q.done or q.listening:
                        self._step(p, q, force=True)
                    else:
                        self._step(q, p)
            else:
                # Both endpoints are listening. Nobody will speak again, so
                # the earlier window expires - in the field, a lost EOB or
                # a lost STAT.
                p = min(live, key=lambda x: x.window[1])
                self._timeout(p)

            for name, p in procs.items():
                if p.done or not p.listening:
                    continue
                q = peer[name]
                if q.done or q.t > p.window[1]:
                    self._timeout(p)
        else:
            raise RuntimeError("event limit reached - the state machine is "
                               "probably looping")
        return a.result, b.result

    # ------------------------------------------------------------ steps

    def _pull(self, proc):
        """Advance the generator to its next action."""
        value, proc._send_value = proc._send_value, None
        self.clock.now_ms = proc.t
        try:
            return proc.gen.send(value)
        except StopIteration as stop:
            proc.done = True
            proc.result = stop.value
            return None

    def _step(self, proc, other, force=False):
        """
        Resolve one action. Returns False if the action could not be
        resolved yet because the peer's state at that instant is unknown.
        """
        if proc.pending is None:
            proc.pending = self._pull(proc)
            if proc.pending is None:
                return True

        act = proc.pending

        if isinstance(act, Sleep):
            proc.pending = None
            proc.t += act.ms
            self.clock.now_ms = proc.t
            return True

        if isinstance(act, Recv):
            proc.pending = None
            proc.window = (proc.t, proc.t + act.timeout_ms)
            return True

        if isinstance(act, Send):
            # The half-duplex question - was the peer listening when the
            # preamble started? - is only answerable once the peer has
            # advanced to at least this instant, or is blocked listening
            # across it. Otherwise we must let the peer run first.
            if not force and not other.done and not other.listening \
                    and other.t <= proc.t:
                return False
            proc.pending = None
            self._do_send(proc, other, act)
            return True

        raise TypeError(f"unknown action {act!r}")

    def _do_send(self, proc, other, send):
        t0, t1 = proc.t, proc.t + send.toa_ms
        self.frames_sent += 1
        proc.t = t1
        self.clock.now_ms = t1

        heard = (other.listening
                 and other.window[0] <= t0
                 and t1 <= other.window[1]
                 and not other.done)
        if not heard:
            self._note(t0, proc.name,
                       f"TX {len(send.payload)}B (peer not listening)")
            self.frames_lost += 1
            return

        ok, rssi, snr = self.channel.deliver(proc.name, send.payload, t0)
        if not ok:
            self._note(t0, proc.name, f"TX {len(send.payload)}B (lost)")
            self.frames_lost += 1
            return

        self._note(t0, proc.name, f"TX {len(send.payload)}B -> {other.name}")
        other.window = None
        other.t = t1
        other._send_value = Rx(send.payload, rssi, snr)

    def _timeout(self, proc):
        end = proc.window[1]
        proc.window = None
        proc.t = end
        proc._send_value = Rx(None)
        self._note(end, proc.name, "RX timeout")
