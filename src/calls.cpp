// calls.cpp – Anrufe von Notrufnummern automatisch annehmen (Mithören)
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#include "calls.h"
#include "globals.h"
#include "modem.h"
#include "util.h"

// Vergleicht die letzten 9 Ziffern -> "+4917612345678" == "017612345678"
bool isAuthorizedCaller(const String& caller) {
  String c = digitsOnly(caller);
  if (c.length() < 6) return false;              // unterdrückte/ungültige Nummer
  String cTail = c.substring(c.length() > 9 ? c.length() - 9 : 0);

  bool ok = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    for (int i = 0; i < 3; i++) {
      String p = digitsOnly(phoneNumbers[i]);
      if (p.length() < 6) continue;
      String pTail = p.substring(p.length() > 9 ? p.length() - 9 : 0);
      if (pTail == cTail) { ok = true; break; }
    }
    xSemaphoreGive(stateMutex);
  }
  return ok;
}

void callTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(4000));
  for (;;) {
    // 1) Unaufgeforderte Meldungen lesen, wenn der UART frei ist
    if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      drainModem();
      xSemaphoreGive(serial2Mutex);
    }

    // 2) Eingehender Anruf
    if (callerPending) {
      callerPending = false;
      String caller;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        caller = pendingCaller;
        xSemaphoreGive(stateMutex);
      }

      if (!callActive) {
        if (LISTEN_IN_ENABLED && isAuthorizedCaller(caller)) {
          Serial.println("Anruf von Notrufnummer " + caller + " -> annehmen (Mithoeren)");
          String r = sendATCommand("ATA", 3000);
          Serial.println("ATA Antwort: " + r);
          if (r.indexOf("ERROR") == -1 && r.indexOf("NO CARRIER") == -1) {
            callActive = true;
            appendSmsStatus("\n🎧 Anruf von " + caller + " angenommen (Mithören aktiv).");
          }
        } else {
          Serial.println("Anruf von " + caller + " abgelehnt (keine Notrufnummer).");
          sendATCommand("ATH", 2000);
        }
      }
    }

    // 3) Auflegen über Webseite
    if (hangupRequested) {
      hangupRequested = false;
      sendATCommand("ATH", 2000);
      if (callActive) {
        callActive = false;
        appendSmsStatus("\nAnruf über Webseite beendet.");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
