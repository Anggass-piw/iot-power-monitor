// ESP32 power monitor: reads a PZEM-004T v3.0 and POSTs JSON to the backend.
// Library: "PZEM-004T-v30" by Jakub Mandula (install from Library Manager).
// Wiring: PZEM TX -> GPIO16 (RX2), PZEM RX -> GPIO17 (TX2), 5V and GND from the PZEM
// board's TTL side. Use a level-safe connection; see the safety note in the README.

#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include "config.h"

#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17

PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " failed");
}

bool postReading(float v, float i, float p, float e) {
  char body[160];
  snprintf(body, sizeof(body),
           "{\"device_id\":\"%s\",\"voltage\":%.1f,\"current\":%.3f,\"power\":%.1f,\"energy_kwh\":%.4f}",
           DEVICE_ID, v, i, p, e);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code == 200;
}

void setup() {
  Serial.begin(115200);
  connectWiFi();
}

void loop() {
  connectWiFi();

  float voltage = pzem.voltage();
  float current = pzem.current();
  float power = pzem.power();
  float energy = pzem.energy();

  if (isnan(voltage) || isnan(current) || isnan(power) || isnan(energy)) {
    Serial.println("PZEM read failed, check wiring");
  } else {
    bool ok = postReading(voltage, current, power, energy);
    Serial.printf("%.1f V  %.3f A  %.1f W  %.4f kWh  -> %s\n",
                  voltage, current, power, energy, ok ? "sent" : "send failed");
  }
  delay(POST_INTERVAL_MS);
}
