import pytest
from fastapi.testclient import TestClient

import main


@pytest.fixture
def client(tmp_path, monkeypatch):
    monkeypatch.setattr(main, "DB_PATH", str(tmp_path / "readings.sqlite"))
    monkeypatch.setenv("INGEST_API_KEY", "test-only-key")
    with TestClient(main.app) as test_client:
        yield test_client


READING = {
    "device_id": "test-device",
    "voltage": 220.0,
    "current": 0.5,
    "power": 110.0,
    "energy_kwh": 0.01,
}


def test_ingest_rejects_missing_or_incorrect_key(client):
    assert client.post("/api/readings", json=READING).status_code == 401
    response = client.post(
        "/api/readings",
        json=READING,
        headers={"X-API-Key": "wrong-key"},
    )
    assert response.status_code == 401


def test_ingest_fails_closed_when_server_key_is_not_configured(client, monkeypatch):
    monkeypatch.delenv("INGEST_API_KEY")
    response = client.post(
        "/api/readings",
        json=READING,
        headers={"X-API-Key": "test-only-key"},
    )
    assert response.status_code == 503


def test_ingest_accepts_valid_key_and_stores_reading(client):
    response = client.post(
        "/api/readings",
        json=READING,
        headers={"X-API-Key": "test-only-key"},
    )
    assert response.status_code == 200
    assert response.json()["anomaly"] is False

    stored = client.get("/api/readings").json()
    assert len(stored) == 1
    assert stored[0]["device_id"] == "test-device"
