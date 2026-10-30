// ESP32 power monitor: reads a PZEM-004T v3.0 and queues JSON for the backend.
// Library: "PZEM-004T-v30" by Jakub Mandula (install from Library Manager).
// Wiring: PZEM TX -> GPIO16 (RX2), PZEM RX -> GPIO17 (TX2), 5V and GND from the PZEM
// board's TTL side. Use a level-safe connection; see the safety note in the README.

#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <PZEM004Tv30.h>
#include "config.h"

#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17

PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);

static const char *QUEUE_TEMP = "/queue.tmp";
static const uint32_t MAX_RETRY_MS = 60000;
static const uint8_t MAX_SENDS_PER_LOOP = 5;
bool queueReady = false;
uint32_t nextQueueId = 0;
uint32_t retryDelayMs = POST_INTERVAL_MS;
uint32_t nextRetryAt = 0;
bool wifiAttemptStarted = false;
uint32_t nextWifiAttemptAt = 0;

String queuePath(uint32_t id) {
  char path[24];
  snprintf(path, sizeof(path), "/q%010lu.json", (unsigned long)id);
  return String(path);
}

String normalizedPath(const String &name) {
  return name.startsWith("/") ? name : String("/") + name;
}

bool isQueueFile(const String &rawName) {
  String name = normalizedPath(rawName);
  return name.startsWith("/q") && name.endsWith(".json") && name.length() == 17;
}

void scanQueueIds() {
  File root = LittleFS.open("/");
  if (!root) return;

  File entry = root.openNextFile();
  while (entry) {
    String name = normalizedPath(entry.name());
    if (isQueueFile(name)) {
      String digits = name.substring(2, 12);
      uint32_t id = (uint32_t)strtoul(digits.c_str(), NULL, 10);
      if (id >= nextQueueId) nextQueueId = id + 1;
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
}

void initQueue() {
  Preferences marker;
  bool markerReady = marker.begin("pwrmon", false);
  bool wasInitialized = markerReady && marker.getBool("queue_fs", false);
  if (markerReady) marker.end();

  // Only format when NVS confirms this queue filesystem has never been initialized.
  queueReady = LittleFS.begin(false);
  if (!queueReady && markerReady && !wasInitialized) {
    Serial.println("Initializing the blank LittleFS queue partition");
    queueReady = LittleFS.begin(true);
  }
  if (!queueReady) {
    Serial.println("LittleFS mount failed; queue was not formatted to protect pending samples");
    return;
  }
  if (markerReady && !wasInitialized) {
    Preferences writer;
    if (writer.begin("pwrmon", false)) {
      writer.putBool("queue_fs", true);
      writer.end();
    }
  }

  scanQueueIds();

  // Recover a fully written temporary record if power failed before its rename.
  if (LittleFS.exists(QUEUE_TEMP)) {
    File temp = LittleFS.open(QUEUE_TEMP, FILE_READ);
    String payload = temp ? temp.readString() : "";
    if (temp) temp.close();
    if (payload.startsWith("{") && payload.endsWith("}")) {
      String recoveredPath = queuePath(nextQueueId);
      if (LittleFS.rename(QUEUE_TEMP, recoveredPath.c_str())) {
        nextQueueId++;
        Serial.println("Recovered one queued sample after restart");
      } else {
        Serial.println("Could not recover the temporary queued sample");
      }
    } else {
      LittleFS.remove(QUEUE_TEMP);
      Serial.println("Removed an incomplete temporary queue record");
    }
  }
}

bool enqueueReading(float voltage, float current, float power, float energy) {
  if (!queueReady) return false;

  char payload[192];
  int length = snprintf(payload, sizeof(payload),
                        "{\"device_id\":\"%s\",\"voltage\":%.1f,\"current\":%.3f,\"power\":%.1f,\"energy_kwh\":%.4f}",
                        DEVICE_ID, voltage, current, power, energy);
  if (length <= 0 || length >= (int)sizeof(payload)) {
    Serial.println("Sample JSON is too large; sample was not queued");
    return false;
  }
  if (LittleFS.exists(QUEUE_TEMP)) {
    Serial.println("Queue temporary file is occupied; sample was not queued");
    return false;
  }

  File temp = LittleFS.open(QUEUE_TEMP, FILE_WRITE);
  if (!temp) {
    Serial.println("Could not open queue temporary file");
    return false;
  }
  size_t written = temp.write((const uint8_t *)payload, (size_t)length);
  temp.flush();
  temp.close();
  if (written != (size_t)length) {
    Serial.println("Could not write the complete sample to LittleFS");
    LittleFS.remove(QUEUE_TEMP);
    return false;
  }

  String destination = queuePath(nextQueueId);
  if (!LittleFS.rename(QUEUE_TEMP, destination.c_str())) {
    Serial.println("Could not commit sample to the persistent queue");
    return false;
  }
  nextQueueId++;
  return true;
}

bool oldestQueuedSample(String &path, String &payload) {
  File root = LittleFS.open("/");
  if (!root) return false;

  File entry = root.openNextFile();
  while (entry) {
    String name = normalizedPath(entry.name());
    if (isQueueFile(name) && (path.length() == 0 || name.compareTo(path) < 0)) path = name;
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  if (path.length() == 0) return false;

  File sample = LittleFS.open(path.c_str(), FILE_READ);
  if (!sample) return false;
  payload = sample.readString();
  sample.close();
  return payload.length() > 0;
}

bool postPayload(const String &payload) {
  HTTPClient http;
  if (!http.begin(SERVER_URL)) return false;
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", INGEST_API_KEY);
  int code = http.POST(payload);
  http.end();
  return code >= 200 && code < 300;
}

void scheduleRetry() {
  nextRetryAt = millis() + retryDelayMs;
  if (retryDelayMs >= MAX_RETRY_MS / 2) {
    retryDelayMs = MAX_RETRY_MS;
  } else {
    retryDelayMs *= 2;
  }
}

void flushQueue() {
  if (!queueReady || WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - nextRetryAt) < 0) return;

  for (uint8_t sent = 0; sent < MAX_SENDS_PER_LOOP; sent++) {
    String path;
    String payload;
    if (!oldestQueuedSample(path, payload)) return;
    if (!postPayload(payload)) {
      Serial.println("Backend send failed; sample kept in LittleFS queue");
      scheduleRetry();
      return;
    }
    if (!LittleFS.remove(path.c_str())) {
      Serial.println("Backend accepted sample but queue acknowledgement failed; it may be replayed");
      scheduleRetry();
      return;
    }
    retryDelayMs = POST_INTERVAL_MS;
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiAttemptStarted = false;
    return;
  }
  if (wifiAttemptStarted && (int32_t)(millis() - nextWifiAttemptAt) < 0) return;

  if (!wifiAttemptStarted) {
    Serial.printf("Connecting to %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiAttemptStarted = true;
  } else {
    Serial.println("Retrying WiFi connection");
    WiFi.reconnect();
  }
  nextWifiAttemptAt = millis() + 10000;
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  initQueue();
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
    bool queued = enqueueReading(voltage, current, power, energy);
    Serial.printf("%.1f V  %.3f A  %.1f W  %.4f kWh  -> %s\n",
                  voltage, current, power, energy, queued ? "queued" : "queue failed");
  }

  flushQueue();
  delay(POST_INTERVAL_MS);
}
