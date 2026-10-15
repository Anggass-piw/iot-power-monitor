"""Send fake readings to the backend so you can run the project without hardware.

Usage: python simulate.py [--url http://localhost:8000/api/readings] [--interval 1]
"""
import argparse
import json
import math
import random
import time
import urllib.request


def post(url, payload):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"}
    )
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.load(resp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:8000/api/readings")
    ap.add_argument("--interval", type=float, default=1.0)
    args = ap.parse_args()

    energy_kwh, t = 0.0, 0
    while True:
        base = 60 + 8 * math.sin(t / 15) + random.gauss(0, 1.2)  # a fan-like load
        voltage = 220 + random.gauss(0, 1.0)
        power = base

        if t % 45 == 44:      # short circuit-like spike
            power = base * 4
        elif t % 70 == 69:    # voltage sag
            voltage = 175

        current = power / voltage
        energy_kwh += power * args.interval / 3.6e6
        result = post(args.url, {
            "device_id": "sim-01", "voltage": round(voltage, 1), "current": round(current, 3),
            "power": round(power, 1), "energy_kwh": round(energy_kwh, 5),
        })
        flag = "ANOMALY " + "; ".join(result["reasons"]) if result["anomaly"] else "ok"
        print(f"t={t:4d}  {voltage:6.1f} V  {current:5.2f} A  {power:6.1f} W  {flag}")
        t += 1
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
