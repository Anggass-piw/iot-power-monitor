"""Anomaly detection for power readings.

Two layers:
1. Hard limits  - overcurrent and voltage out of range (safety rules).
2. Statistical  - rolling z-score on power, per device.

z = (x - mean) / std, computed over the last `window` normal readings.
Flagged readings are kept out of the window so a spike does not distort
the baseline. If the new level persists for `persist` readings it is treated
as a legitimate load change and becomes the new baseline.
"""
from collections import defaultdict, deque
from statistics import mean, pstdev


class AnomalyDetector:
    def __init__(self, window=60, z_threshold=3.0, min_samples=10, min_std=1.0,
                 persist=5, max_current=10.0, v_min=198.0, v_max=242.0):
        self.z_threshold = z_threshold
        self.min_samples = min_samples
        self.min_std = min_std  # floor so a flat load does not cause false alarms
        self.persist = persist
        self.max_current = max_current
        self.v_min = v_min
        self.v_max = v_max
        self._history = defaultdict(lambda: deque(maxlen=window))
        self._streak = defaultdict(int)

    def check(self, device_id, voltage, current, power):
        """Return a list of reasons. Empty list means the reading is normal."""
        reasons = []

        if current > self.max_current:
            reasons.append(f"overcurrent: {current:.2f} A > {self.max_current:.1f} A")
        if not (self.v_min <= voltage <= self.v_max):
            reasons.append(f"voltage out of range: {voltage:.1f} V")

        history = self._history[device_id]
        z_flagged = False
        if len(history) >= self.min_samples:
            mu = mean(history)
            sd = max(pstdev(history), self.min_std)
            z = (power - mu) / sd
            if abs(z) > self.z_threshold:
                kind = "spike" if z > 0 else "drop"
                reasons.append(f"power {kind}: z={z:.1f} (baseline {mu:.1f} W)")
                z_flagged = True

        if z_flagged:
            self._streak[device_id] += 1
            if self._streak[device_id] >= self.persist:  # load really changed
                history.clear()
                history.append(power)
                self._streak[device_id] = 0
        else:
            self._streak[device_id] = 0
            history.append(power)

        return reasons
