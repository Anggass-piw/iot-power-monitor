from detector import AnomalyDetector


def warmed_up(n=30):
    d = AnomalyDetector()
    for i in range(n):
        d.check("dev", 220.0, 0.27, 60 + (i % 3 - 1) * 0.5)
    return d


def test_normal_readings_are_not_flagged():
    d = AnomalyDetector()
    for i in range(50):
        assert d.check("dev", 220.0, 0.27, 60 + (i % 3 - 1) * 0.5) == []


def test_power_spike_is_flagged():
    reasons = warmed_up().check("dev", 220.0, 1.8, 400.0)
    assert any("power spike" in r for r in reasons)


def test_overcurrent_is_flagged_without_history():
    reasons = AnomalyDetector(max_current=10).check("dev", 220.0, 12.0, 2600.0)
    assert any("overcurrent" in r for r in reasons)


def test_voltage_out_of_range_is_flagged():
    reasons = AnomalyDetector().check("dev", 170.0, 0.3, 50.0)
    assert any("voltage" in r for r in reasons)


def test_spike_does_not_pollute_baseline():
    d = warmed_up()
    d.check("dev", 220.0, 1.8, 400.0)
    assert d.check("dev", 220.0, 0.27, 60.0) == []


def test_sustained_load_change_becomes_new_baseline():
    d = warmed_up()
    results = [d.check("dev", 220.0, 1.0, 220.0) for _ in range(12)]
    assert results[0] != []   # first reading of the new level is flagged
    assert results[-1] == []  # later it is accepted as normal
