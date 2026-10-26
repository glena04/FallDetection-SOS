// fall_detection.cpp – Sturzerkennung, Abbruch-Countdown, Sensor-Überwachung
// Fall Detection & SOS Device – Firmware v7.5 (PlatformIO)
#include "fall_detection.h"
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <esp_task_wdt.h>
#include "globals.h"
#include "sos.h"
#include "sms_queue.h"

static Adafruit_MPU6050 mpu;
static bool fallDetected = false;

// Werte für das Webinterface speichern
static void storeMpuValues(const sensors_event_t& a, const sensors_event_t& g) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    mpuAccX = a.acceleration.x; mpuAccY = a.acceleration.y; mpuAccZ = a.acceleration.z;
    mpuGyrX = g.gyro.x;         mpuGyrY = g.gyro.y;         mpuGyrZ = g.gyro.z;
    xSemaphoreGive(stateMutex);
  }
}

// Winkel zwischen zwei Schwerkraft-Vektoren (unabhängig von der Einbaulage)
static float angleBetweenDeg(float ax, float ay, float az, float bx, float by, float bz) {
  float n = sqrtf(ax * ax + ay * ay + az * az) * sqrtf(bx * bx + by * by + bz * bz);
  if (n < 0.001f) return 0.0f;
  float c = (ax * bx + ay * by + az * bz) / n;
  c = constrain(c, -1.0f, 1.0f);
  return acosf(c) * 180.0f / PI;
}

// Misst ~1,5 s: mittlere Lage + ob die Person ruhig liegt
static bool measureRest(float& gx, float& gy, float& gz) {
  const int N = 75;
  int stillCount = 0;
  gx = gy = gz = 0.0f;
  for (int i = 0; i < N; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    storeMpuValues(a, g);
    gx += a.acceleration.x; gy += a.acceleration.y; gz += a.acceleration.z;
    float tot = sqrtf(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);
    float gyr = sqrtf(g.gyro.x * g.gyro.x + g.gyro.y * g.gyro.y + g.gyro.z * g.gyro.z);
    if (fabsf(tot - G_EARTH) < STILL_ACC_DEV && gyr < STILL_GYRO) stillCount++;
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
  }
  gx /= N; gy /= N; gz /= N;
  return stillCount > (N * 8) / 10;   // 80 % der Zeit ruhig
}

// NEU: Countdown vor dem Alarm. Rückgabe true = vom Benutzer abgebrochen
static bool runCancelCountdown() {
  alarmCancelled = false;
  alarmPending   = true;

  uint32_t start = millis();
  int lastShown = -1;
  while (millis() - start < CANCEL_WINDOW_MS && !alarmCancelled) {
    uint32_t elapsed = millis() - start;
    int remain = (CANCEL_WINDOW_MS - elapsed + 999) / 1000;
    if (remain != lastShown) {
      lastShown = remain;
      setSmsStatus("⚠ STURZ ERKANNT! Notruf in " + String(remain) +
                   " s.\nZum Abbrechen SOS-Taster drücken oder „Sturzalarm abbrechen“ klicken.");
      Serial.printf("Sturzalarm in %d s - Taster druecken zum Abbrechen\n", remain);
    }
    if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, ((elapsed / 250) % 2) ? HIGH : LOW); // piepen/blinken
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, LOW);
  alarmPending = false;
  return alarmCancelled;
}

// Alarm auslösen. Rückgabe true = SMS wurden gesendet
static bool triggerFallAlarm() {
  fallDetected = true;
  Serial.println("Sturz erkannt - starte Abbruch-Countdown.");

  bool anyNumber = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++)
      if (phoneNumbers[i].length() > 0) anyNumber = true;
    xSemaphoreGive(stateMutex);
  }
  if (!anyNumber) {
    setSmsStatus("STURZ ERKANNT, aber keine Nummer gespeichert!");
    return false;
  }

  // NEU: 15 s Zeit zum Abbrechen
  if (runCancelCountdown()) {
    setSmsStatus("Sturzalarm abgebrochen – kein Notruf gesendet.");
    Serial.println("Sturzalarm abgebrochen.");
    return false;
  }

  Serial.println("SOS: Sturz bestaetigt - sende Notruf!");
  int queued = enqueueSOSForAllNumbers("STURZ ERKANNT! Ich brauche Hilfe.");
  if (queued > 0) {
    setSmsStatus("STURZ ERKANNT: Notruf an " + String(queued) + " Nummer(n) gesendet.");
    return true;
  }
  setSmsStatus("STURZ ERKANNT: SMS-Warteschlange voll!");
  return false;
}

// v7.4: Warten mit Watchdog-Reset (nur im mpuTask verwenden)
static void wdtDelay(uint32_t ms) {
  while (ms > 0) {
    uint32_t d = ms > 1000 ? 1000 : ms;
    vTaskDelay(pdMS_TO_TICKS(d));
    esp_task_wdt_reset();
    ms -= d;
  }
}

// v7.4: Sensor initialisieren (auch für Neuversuche). true = bereit
bool initMPU() {
  bool ok = false;
  if (mpu.begin(MPU_ADDR, &Wire)) {
    Serial.println("MPU6050 erfolgreich initialisiert.");
    ok = true;
  } else {
    // Klon-Chips (z.B. MPU6500) melden eine andere ID -> WHO_AM_I selbst prüfen
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x75);                        // WHO_AM_I Register
    Wire.endTransmission(false);
    Wire.requestFrom(MPU_ADDR, 1);
    uint8_t id = Wire.available() ? Wire.read() : 0xFF;
    Serial.printf("MPU WHO_AM_I = 0x%02X\n", id);

    if (id != 0x00 && id != 0xFF) {          // Chip antwortet -> verwenden
      Wire.beginTransmission(MPU_ADDR);
      Wire.write(0x6B); Wire.write(0x00);    // PWR_MGMT_1: aus dem Sleep-Modus wecken
      Wire.endTransmission();
      delay(100);
      Serial.println("Kompatibler MPU-Klon erkannt - wird verwendet.");
      ok = true;
    } else {
      Serial.println("MPU6050 nicht gefunden! Verkabelung prüfen.");
    }
  }
  if (ok) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G); // Bis zu 8g (gut für Stürze)
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);   // 44 Hz: Aufprall-Spitzen bleiben sichtbar
  }
  return ok;
}

// v7.4: Sturzerkennung. Kehrt nur zurück, wenn der Sensor ausfällt (Grund in reason)
static void runFallDetection(String& reason) {
  float refX = 0, refY = 0, refZ = G_EARTH; // Referenzlage (vor dem Sturz)
  bool  refInit = false;
  bool  inFreefall = false;
  uint32_t ffStart = 0, ffEnd = 0;

  // Sensor-Überwachung
  uint32_t lastHealth = millis();
  uint32_t lowGSince  = 0;
  int      sameCount  = 0;
  float    lastAx = 0, lastAy = 0, lastAz = 0;

  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    esp_task_wdt_reset();

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    storeMpuValues(a, g);

    float ax = a.acceleration.x, ay = a.acceleration.y, az = a.acceleration.z;
    float totalAcc = sqrtf(ax * ax + ay * ay + az * az);
    uint32_t now = millis();

    // ---- v7.4: Sensor-Gesundheit prüfen ----
    if (now - lastHealth >= 1000) {                       // 1) antwortet der Chip?
      lastHealth = now;
      Wire.beginTransmission(MPU_ADDR);
      if (Wire.endTransmission() != 0) { reason = "keine Antwort am I2C-Bus"; return; }
    }
    if (ax == lastAx && ay == lastAy && az == lastAz) {    // 2) Werte eingefroren?
      if (++sameCount > 150) { reason = "Messwerte eingefroren"; return; }   // 3 s
    } else {
      sameCount = 0;
      lastAx = ax; lastAy = ay; lastAz = az;
    }
    if (totalAcc < 2.0f) {                                 // 3) keine Schwerkraft > 5 s?
      if (lowGSince == 0) lowGSince = now;
      else if (now - lowGSince > 5000) { reason = "keine Schwerkraft gemessen"; return; }
    } else {
      lowGSince = 0;
    }

    // 1) Referenzlage nur in ruhigen Phasen (~1 g) langsam nachführen
    if (fabsf(totalAcc - G_EARTH) < 1.0f) {
      if (!refInit) { refX = ax; refY = ay; refZ = az; refInit = true; }
      else {
        refX = 0.95f * refX + 0.05f * ax;
        refY = 0.95f * refY + 0.05f * ay;
        refZ = 0.95f * refZ + 0.05f * az;
      }
    }

    // 2) Freifall-Phase erkennen
    if (totalAcc < FREEFALL_THRESHOLD) {
      if (!inFreefall) { inFreefall = true; ffStart = now; }
    } else if (inFreefall) {
      inFreefall = false;
      if (now - ffStart >= FREEFALL_MIN_MS) ffEnd = now;
    }
    bool recentFreefall = (ffEnd != 0) && (now - ffEnd < FF_TO_IMPACT_WINDOW_MS);

    // 3) Aufprall
    float impactThr = recentFreefall ? IMPACT_THRESHOLD_AFTER_FF : IMPACT_THRESHOLD;
    if (totalAcc > impactThr && refInit && !fallDetected) {
      float r0x = refX, r0y = refY, r0z = refZ;   // Lage VOR dem Sturz einfrieren
      Serial.printf("Aufprall %.1f m/s^2 (Freifall: %s) - pruefe Lage...\n",
                    totalAcc, recentFreefall ? "ja" : "nein");

      wdtDelay(SETTLE_MS);                         // Ausrollen lassen

      // 4) Lage danach + Ruhe prüfen
      float px, py, pz;
      bool still = measureRest(px, py, pz);
      float change = angleBetweenDeg(r0x, r0y, r0z, px, py, pz);
      Serial.printf("Lageaenderung %.0f Grad, ruhig: %s\n", change, still ? "ja" : "nein");

      if (change > ORIENTATION_CHANGE_DEG && (still || recentFreefall)) {
        bool sent = triggerFallAlarm();            // inkl. 15 s Abbruch-Countdown
        wdtDelay(sent ? 15000 : 3000);             // Cooldown
        fallDetected = false;
      } else {
        Serial.println("Entwarnung: kein Sturz.");
      }

      ffEnd = 0;
      lowGSince = 0; sameCount = 0; lastHealth = millis();
      lastWake = xTaskGetTickCount();              // Takt nach langer Pause neu setzen
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

// ---- MPU6050 Task (Sturzerkennung + Sensor-Überwachung) ----
void mpuTask(void* pv) {
  esp_task_wdt_add(NULL);           // v7.4: Watchdog überwacht diesen Task
  wdtDelay(2000);                   // Warten bis I2C stabil ist

  uint32_t faultSince = 0;
  uint32_t lastAlert  = 0;
  bool     alertSent  = false;
  String   reason     = "beim Start nicht gefunden";

  for (;;) {
    // ---- Sensor fehlt/ausgefallen: regelmäßig neu versuchen ----
    while (!mpuReady) {
      sensorOk = false;
      if (faultSince == 0) {
        faultSince = millis();
        Serial.println("SENSORFEHLER: " + reason);
        setSmsStatus("⚠ Sturzsensor ausgefallen (" + reason +
                     "). Sturzerkennung inaktiv – SOS-Taster funktioniert.");
      }
      // nach 1 min Ausfall die Kontakte warnen (höchstens alle 6 h)
      if (millis() - faultSince > SENSOR_ALERT_AFTER_MS &&
          (!alertSent || millis() - lastAlert > SENSOR_ALERT_INTERVAL_MS)) {
        if (enqueueToAll("GERAETEWARNUNG: Sturzsensor ausgefallen. Sturzerkennung inaktiv, SOS-Taste funktioniert.") > 0) {
          alertSent = true;
          lastAlert = millis();
        }
      }
      wdtDelay(SENSOR_RETRY_MS);
      Wire.end();                                  // I2C-Bus neu starten
      Wire.begin(I2C_SDA, I2C_SCL);
      Wire.setTimeOut(50);
      mpuReady = initMPU();
    }

    // ---- Sensor (wieder) bereit ----
    if (faultSince != 0) {
      Serial.println("Sturzsensor wieder bereit.");
      appendSmsStatus("\n✅ Sturzsensor wieder bereit – Sturzerkennung aktiv.");
      if (alertSent) enqueueToAll("INFO: Sturzsensor wieder OK, Sturzerkennung aktiv.");
      faultSince = 0;
      alertSent  = false;
    }
    sensorOk = true;

    runFallDetection(reason);      // läuft, bis der Sensor ausfällt
    mpuReady = false;              // -> oben neu versuchen
  }
}
