// power.cpp – Energiesparen
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
//
// Ruhezustand (nach IDLE_SLEEP_MS ohne Bedienung, wenn nichts zu tun ist):
//   - ESP32: Light Sleep (RAM bleibt erhalten, Aufwachen in ca. 1 ms)
//   - A9G:   Schlafmodus (bleibt im Netz -> Anrufe und SMS kommen an), GPS aus
//   - WLAN:  aus
//   - MPU6050 misst weiter und weckt den ESP32 über INT bei Bewegung
// Wecken: SOS-Taste (GPIO 4), MPU-INT (MPU_INT_PIN), Modem-Meldung (RX-Pin), Timer.
// Nach dem Wecken bleibt der ESP32 nur so lange wach, wie es etwas zu tun gibt.
#include "power.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "globals.h"
#include "modem.h"
#include "location.h"
#include "wifi_manager.h"
#include "fall_detection.h"

static volatile uint32_t awakeUntil = 0;
static uint32_t sleepCount   = 0;
static uint64_t sleepTotalMs = 0;
static uint32_t wakeMotion = 0, wakeButton = 0, wakeModem = 0, wakeTimer = 0;

void powerActivity(uint32_t holdMs) {
  uint32_t until = millis() + holdMs;
  if ((int32_t)(until - awakeUntil) > 0) awakeUntil = until;
}

// Grund, warum nicht geschlafen wird ("" = nichts zu tun)
static const char* busyReason() {
  if ((int32_t)(awakeUntil - millis()) > 0) return "Wachhalte-Zeit (Start/Bedienung/Bewegung)";
  if (alarmPending || fallCheckActive)      return "Sturzpruefung/Countdown";
  if (callActive)                           return "Gespraech";
  if (smsBusy || (smsQueue && uxQueueMessagesWaiting(smsQueue) > 0)) return "SMS wird gesendet";
  if (locationJobsPending() || gpsIsActive()) return "Ortung/GPS";
  if (wifiActive())                         return "WLAN an";
  if (restartPending)                       return "Neustart";
  if (!sensorOk)                            return "Sturzsensor nicht bereit";
  return "";
}

static bool systemBusy() { return busyReason()[0] != 0; }

static void sleepOnce() {
  // Bewegung oder Tastendruck gerade aktiv -> nicht einschlafen
  if (digitalRead(SOS_BUTTON_PIN) == LOW) { powerActivity(SOS_HOLD_MS + 2000); return; }
  if (digitalRead(MPU_INT_PIN) == HIGH) {
    vTaskDelay(pdMS_TO_TICKS(60));          // mpuTask liest -> Interrupt wird gelöscht
    if (digitalRead(MPU_INT_PIN) == HIGH) { powerActivity(MOTION_WINDOW_MS); return; }
  }

  modemEnableSleep();                       // A9G-Schlafmodus (einmal pro Einrichtung)
  if (systemBusy()) return;

  // ausgeschaltetes A9G -> RX-Pin nicht als Weckquelle
  // v7.8.1: im Schlafmodus 1/2 ist die UART des A9G evtl. inaktiv -> RX nur im Modus 0 nutzen
  bool useModemWake = modemIsOnline() && A9G_SLEEP_MODE == 0;

  // v7.7: nie mitten in einer I2C- oder Modem-Übertragung einschlafen
  if (!sensorBusLock(50)) return;
  if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(50)) != pdTRUE) { sensorBusUnlock(); return; }

  Serial.flush();
  Serial2.flush();
  a9gSleepPin();                            // v7.8: Schlafmodus 2 – A9G per Pin schlafen

  gpio_wakeup_enable((gpio_num_t)SOS_BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)MPU_INT_PIN,    GPIO_INTR_HIGH_LEVEL);
  if (useModemWake) gpio_wakeup_enable((gpio_num_t)RXD2, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)HOUSEKEEPING_INTERVAL_MS * 1000ULL);

  uint32_t t0 = millis();
  esp_err_t err = esp_light_sleep_start();

  // Weckgrund sofort lesen (bevor andere Tasks laufen)
  bool sos    = digitalRead(SOS_BUTTON_PIN) == LOW;
  bool motion = digitalRead(MPU_INT_PIN) == HIGH;
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  a9gWakePin();                             // v7.8: A9G zuerst wecken, bevor andere Tasks senden
  xSemaphoreGive(serial2Mutex);
  sensorBusUnlock();

  gpio_wakeup_disable((gpio_num_t)SOS_BUTTON_PIN);
  gpio_wakeup_disable((gpio_num_t)MPU_INT_PIN);
  if (useModemWake) gpio_wakeup_disable((gpio_num_t)RXD2);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  if (err != ESP_OK) {
    Serial.printf("Light Sleep nicht moeglich (Fehler %d) - bleibe 10 s wach.\n", (int)err);
    powerActivity(10000);
    return;
  }
  sleepCount++;
  sleepTotalMs += millis() - t0;

  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    wakeTimer++;
    smsReceived = true;                     // SIM auf neue SMS prüfen
    powerActivity(HOUSEKEEPING_AWAKE_MS);
  } else if (sos) {
    wakeButton++;
    Serial.println("Aufgewacht: SOS-Taste");
    powerActivity(SOS_HOLD_MS + 2000);      // Zeit für 2 s Halten
  } else if (motion) {
    wakeMotion++;
    powerActivity(MOTION_WINDOW_MS);        // Sturzprüfung im mpuTask
  } else {
    wakeModem++;
    Serial.println("Aufgewacht: Modem-Meldung (Anruf/SMS?)");
    smsReceived = true;
    powerActivity(MODEM_WAKE_HOLD_MS);      // callTask liest RING / +CMTI
  }
}

String powerStatusText() {
  if (!POWER_SAVE_ENABLED) return "Energiesparen aus";
  if (MPU_INT_PIN < 0)     return "Energiesparen: nur GPS/WLAN/Modem (MPU-INT fehlt)";
  uint32_t pct = millis() > 0 ? (uint32_t)(sleepTotalMs * 100ULL / millis()) : 0;
  return "Energiesparen an, Schlafanteil " + String(pct) + "%";
}

void powerTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(5000));
  uint32_t lastReport = 0;
  bool wasBusy = true;

  for (;;) {
    bool canSleep = POWER_SAVE_ENABLED && MPU_INT_PIN >= 0;
    bool busy = systemBusy();

    // v7.7.2: Grund fürs Wachbleiben bei Änderung anzeigen
    static String lastReason;
    String reason = busyReason();
    if (POWER_SAVE_ENABLED && reason != lastReason && reason.length()) {
      uint32_t left = (int32_t)(awakeUntil - millis()) > 0 ? (awakeUntil - millis()) / 1000 : 0;
      Serial.printf("Energiesparen: wach wegen %s%s\n", reason.c_str(),
                    left ? (" (noch " + String(left) + " s)").c_str() : "");
    }
    lastReason = reason;

    if (canSleep && !busy) {
      if (wasBusy) {
        Serial.println("Energiesparen: Ruhezustand (Wecken: SOS-Taste, Bewegung, Anruf/SMS, alle 15 min).");
        wasBusy = false;
      }
      sleepOnce();
    } else {
      wasBusy = true;
    }

    // Statistik alle 10 min (nur wenn wach)
    if (POWER_SAVE_ENABLED && millis() - lastReport > 600000) {
      lastReport = millis();
      Serial.printf("Energiesparen: %lu x geschlafen (%s), Wecken durch Bewegung %lu, Taste %lu, Modem %lu, Timer %lu\n",
                    (unsigned long)sleepCount, powerStatusText().c_str(),
                    (unsigned long)wakeMotion, (unsigned long)wakeButton,
                    (unsigned long)wakeModem, (unsigned long)wakeTimer);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
