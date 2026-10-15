import os
import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from detector import AnomalyDetector

DB_PATH = os.getenv("DB_PATH", "readings.db")
DASHBOARD_DIR = Path(__file__).resolve().parent.parent / "dashboard"
detector = AnomalyDetector(max_current=float(os.getenv("MAX_CURRENT_A", "10")))


def connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


@asynccontextmanager
async def lifespan(app: FastAPI):
    with connect() as conn:
        conn.execute(
            """CREATE TABLE IF NOT EXISTS readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts REAL NOT NULL,
                device_id TEXT NOT NULL,
                voltage REAL, current REAL, power REAL, energy_kwh REAL,
                anomaly INTEGER NOT NULL DEFAULT 0,
                reason TEXT NOT NULL DEFAULT ''
            )"""
        )
    yield


app = FastAPI(title="IoT Power Monitor", lifespan=lifespan)


class Reading(BaseModel):
    device_id: str = "esp32-01"
    voltage: float = Field(ge=0)
    current: float = Field(ge=0)
    power: float = Field(ge=0)
    energy_kwh: float = Field(default=0, ge=0)


@app.post("/api/readings")
def add_reading(r: Reading):
    reasons = detector.check(r.device_id, r.voltage, r.current, r.power)
    with connect() as conn:
        conn.execute(
            "INSERT INTO readings (ts, device_id, voltage, current, power, energy_kwh, anomaly, reason)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (time.time(), r.device_id, r.voltage, r.current, r.power, r.energy_kwh,
             int(bool(reasons)), "; ".join(reasons)),
        )
    return {"anomaly": bool(reasons), "reasons": reasons}


@app.get("/api/readings")
def list_readings(limit: int = 120):
    limit = max(1, min(limit, 1000))
    with connect() as conn:
        rows = conn.execute(
            "SELECT * FROM readings ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
    return [dict(r) for r in reversed(rows)]


@app.get("/api/anomalies")
def list_anomalies(limit: int = 20):
    limit = max(1, min(limit, 200))
    with connect() as conn:
        rows = conn.execute(
            "SELECT * FROM readings WHERE anomaly = 1 ORDER BY id DESC LIMIT ?", (limit,)
        ).fetchall()
    return [dict(r) for r in rows]


# Must be last so it does not shadow /api routes.
app.mount("/", StaticFiles(directory=DASHBOARD_DIR, html=True), name="dashboard")
