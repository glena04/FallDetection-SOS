// sos.cpp – SOS-Taster (2 s halten, störsicher) und Notruf-SMS
// Fall Detection & SOS Device – Firmware v7.5 (PlatformIO)
#include "sos.h"
#include <esp_task_wdt.h>
#include "globals.h"
#include "location.h"
#include "sms_queue.h"

int enqueueSOSForAllNumbers(const String& baseMessage) {
  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);
  int queued = enqueueToAll(baseMessage + buildLocationText(q, lat, lon, src, acc));
  if (queued > 0 && q < 3) {
    // Kein GPS beim Alarm -> genaueren Standort automatisch nachsenden
    alarmLocQuality     = q;
    alarmTime           = millis();
    locRefreshRequested = true;   // sofort neuen Ersatz-Standort holen
  }
  return queued;
}

// SOS per Taster senden
static void sendButtonSOS() {
  Serial.println(">>> SOS-TASTER (2 s gehalten) <<<");

  bool anyNumber = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++)
      if (phoneNumbers[i].length() > 0) anyNumber = true;
    xSemaphoreGive(stateMutex);
  }

  if (!anyNumber) {
    Serial.println("SOS abgebrochen: Keine Notrufnummer gespeichert!");
    setSmsStatus("SOS-Taster gedrückt, aber keine Nummer gespeichert!");
    return;
  }
  int queued = enqueueSOSForAllNumbers("NOTRUF! Ich brauche Hilfe.");
  if (queued > 0) {
    setSmsStatus("SOS-Taster: Notruf an " + String(queued) + " Nummer(n) gesendet.");
    Serial.println("SOS-SMS an " + String(queued) + " Nummer(n) in Warteschlange.");
  } else {
    setSmsStatus("SOS-Taster: SMS-Warteschlange voll!");
  }
}

// v7.1: Taster wird alle 20 ms abgefragt.
// Ein SOS zählt nur, wenn der Taster 2 s DURCHGEHEND gedrückt ist.
// Kurze Störimpulse (z. B. beim Senden des A9G) werden ignoriert.
void buttonTask(void* pv) {
  esp_task_wdt_add(NULL);       // v7.4: Watchdog überwacht den SOS-Taster
  uint32_t pressStart = 0;
  bool     handled    = false;   // pro Tastendruck nur eine Aktion
  uint32_t lastSos    = 0;
  bool     sosSent    = false;

  for (;;) {
    bool pressed = (digitalRead(SOS_BUTTON_PIN) == LOW);
    uint32_t now = millis();

    if (pressed) {
      if (pressStart == 0) pressStart = now;
      uint32_t held = now - pressStart;

      if (!handled) {
        if (alarmPending && held >= CANCEL_HOLD_MS) {
          // Während des Countdowns: kurzer Druck bricht den Sturzalarm ab
          alarmCancelled = true;
          handled = true;
          Serial.println(">>> Sturzalarm per Taster ABGEBROCHEN <<<");
        } else if (!alarmPending && held >= SOS_HOLD_MS) {
          handled = true;
          if (sosSent && now - lastSos < SOS_LOCKOUT_MS) {
            Serial.println("SOS-Taster ignoriert: Sperrzeit nach letztem SOS.");
          } else {
            lastSos = now;
            sosSent = true;
            sendButtonSOS();
          }
        }
      }
    } else {
      pressStart = 0;            // losgelassen -> nächster Druck möglich
      handled    = false;
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
