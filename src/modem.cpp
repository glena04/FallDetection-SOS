// modem.cpp – A9G: AT-Befehle, Modem-Meldungen (URC), SMS-Versand mit Wiederholung
// Fall Detection & SOS Device – Firmware v7.7.3 (PlatformIO)
#include "modem.h"
#include "sms_queue.h"
#include "power.h"

static String urcBuffer;   // Zeilenpuffer für Modem-Meldungen (serial2Mutex)
static bool   cmtBodyNext = false;   // v7.6.1: nächste Zeile ist der Text einer +CMT-SMS
static String cmtSender;

static void handleModemLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  // v7.6.1: Text einer direkt zugestellten SMS (Zeile nach +CMT:)
  if (cmtBodyNext) {
    cmtBodyNext = false;
    Serial.println("Modem: SMS-Text: " + line);
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (smsDirectCount < SMS_DIRECT_MAX) {
        smsDirectSender[smsDirectCount] = cmtSender;
        smsDirectBody[smsDirectCount]   = line;
        smsDirectCount++;
      }
      xSemaphoreGive(stateMutex);
    }
    smsReceived = true;
    return;
  }
  if (line.startsWith("+CMT:")) {                  // +CMT: "+49178...",,"26/10/27,10:00:00+04"
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    cmtSender = (q1 != -1 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
    cmtBodyNext = true;
    Serial.println("Modem: " + line);
    return;
  }
  if (line.startsWith("+CIEV:") && line.indexOf("MESSAGE") != -1) {
    Serial.println("Modem: " + line);
    smsReceived = true;
    return;
  }

  if (line.startsWith("+CLIP:")) {                 // +CLIP: "+49176...",145,...
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 != -1 && q2 > q1) {
      String num = line.substring(q1 + 1, q2);
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        pendingCaller = num;
        xSemaphoreGive(stateMutex);
      }
      callerPending = true;
    }
  } else if (line.startsWith("+CMTI:")) {         // v7.6: neue SMS gespeichert
    Serial.println("Modem: " + line);
    smsReceived = true;
  } else if (line == "NO CARRIER" || line == "BUSY" || line == "NO ANSWER") {
    if (callActive) {
      callActive = false;
      Serial.println("Anruf beendet.");
      appendSmsStatus("\nAnruf beendet.");
    }
  }
}

void feedModem(const String& data) {
  for (size_t i = 0; i < data.length(); i++) {
    char c = data.charAt(i);
    if (c == '\n') {
      handleModemLine(urcBuffer);
      urcBuffer = "";
    } else if (c != '\r') {
      urcBuffer += c;
      if (urcBuffer.length() > 200) urcBuffer = "";   // Schutz vor Müll
    }
  }
}

// Liest alles Vorhandene vom UART und wertet es aus (statt es wegzuwerfen)
void drainModem() {
  String s;
  while (Serial2.available()) s += (char)Serial2.read();
  if (s.length()) feedModem(s);
}

// v7.7: A9G im Schlafmodus verliert das erste Zeichen -> vorher mit "AT" aufwecken.
// Nur mit gehaltenem serial2Mutex aufrufen.
static uint32_t lastModemTx = 0;
static bool     modemSleepSet = false;

static void modemNudge() {
  if (POWER_SAVE_ENABLED && A9G_SLEEP_ENABLED && millis() - lastModemTx > 2000) {
    // v7.7.3: mehrmals "AT" senden, bis das A9G antwortet (max. ca. 1,5 s)
    for (int i = 0; i < 6; i++) {
      while (Serial2.available()) Serial2.read();
      Serial2.println("AT");
      String r;
      uint32_t t0 = millis();
      while (millis() - t0 < 250) {
        while (Serial2.available()) r += (char)Serial2.read();
        if (r.indexOf("OK") != -1) break;
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if (r.indexOf("OK") != -1) break;
    }
  }
  lastModemTx = millis();
}

void modemEnableSleep() {
  if (!POWER_SAVE_ENABLED || !A9G_SLEEP_ENABLED || modemSleepSet) return;
  String r = sendATCommand("AT+SLEEP=1", 1000);
  Serial.println("A9G-Schlafmodus (AT+SLEEP=1): " + r);
  modemSleepSet = true;     // nur einmal pro Einrichtung versuchen
}

String sendATCommand(String command, unsigned long timeout) {
  String response = "";
  if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(timeout + 1000)) != pdTRUE) {
    return response;
  }
  drainModem();                       // alte Meldungen auswerten (z. B. RING)
  modemNudge();                       // v7.7: A9G ggf. aufwecken
  Serial2.println(command);
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (Serial2.available()) response += (char)Serial2.read();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  feedModem(response + "\n");         // auch Antworten auf Anruf-Meldungen prüfen
  response.trim();
  xSemaphoreGive(serial2Mutex);
  return response;
}

// Mobile Daten (GPRS) aktivieren - nötig für AGPS und Funkzellen-Ortung
bool ensureGprs() {
  if (gprsReady) return true;
  sendATCommand("AT+CGATT=1", 5000);
  sendATCommand(String("AT+CGDCONT=1,\"IP\",\"") + GPRS_APN + "\"", 1000);
  String r = sendATCommand("AT+CGACT=1,1", 5000);
  gprsReady = r.indexOf("OK") != -1;
  Serial.println(gprsReady ? "GPRS aktiv." : "GPRS fehlgeschlagen: " + r);
  return gprsReady;
}

// v7.2: Modem (neu) einrichten - auch nach Stromausfall/Neustart des A9G
void modemInit() {
  sendATCommand("AT+CMGF=1", 1000);                    // SMS-Textmodus
  sendATCommand("AT+CLIP=1", 1000);                    // Anrufernummer anzeigen (+CLIP)
  if (MUTE_SPEAKER) sendATCommand("AT+CLVL=0", 1000);  // Lautsprecher leise (falls unterstützt)
  // v7.6.5: SMS-Speicher fest auf die SIM-Karte setzen (lesen, schreiben, empfangen)
  String cpms = sendATCommand("AT+CPMS=\"SM\",\"SM\",\"SM\"", 1000);
  Serial.println("SMS-Speicher (CPMS): " + cpms);
  String cnmi = sendATCommand("AT+CNMI=2,1,0,0,0", 1000);   // v7.6: neue SMS melden (+CMTI)
  if (cnmi.indexOf("OK") == -1) cnmi = sendATCommand("AT+CNMI=1,1,0,0,0", 1000);   // v7.6.5: Alternative
  Serial.println("SMS-Benachrichtigung (CNMI): " + cnmi);
  smsReceived = true;                                  // v7.6: ungelesene SMS beim Start prüfen
  modemSleepSet = false;                               // v7.7: Schlafmodus nach Einrichtung neu setzen
  // v7.7.3: Schlafmodus abschalten, falls er (z. B. von v7.7) noch aktiv ist
  if (!A9G_SLEEP_ENABLED) sendATCommand("AT+SLEEP=0", 1000);
  // v7.7: GPS wird nicht mehr hier, sondern in location.cpp nach Bedarf geschaltet
}

// v7.2: Ist das A9G bereit zum Senden? (antwortet + im Netz angemeldet)
static bool modemReadyForSms() {
  if (sendATCommand("AT", 500).indexOf("OK") == -1) return false;
  String r = sendATCommand("AT+CREG?", 800);           // +CREG: x,1 oder x,5 = angemeldet
  bool ok = r.indexOf(",1") != -1 || r.indexOf(",5") != -1;
  if (!ok) Serial.println("A9G nicht im Netz: " + r);
  return ok;
}

// v7.2: Ein Sendeversuch. true = erfolgreich, sonst Fehlertext in err
bool trySendSms(const SmsRequest& req, String& err) {
  if (!modemReadyForSms()) { err = "A9G nicht bereit oder kein Netz"; return false; }

  if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
    err = "UART belegt";
    return false;
  }

  drainModem();
  modemNudge();                       // v7.7
  Serial2.println("AT+CMGF=1");
  vTaskDelay(pdMS_TO_TICKS(500));
  drainModem();

  Serial2.print("AT+CMGS=\"");
  Serial2.print(req.number);
  Serial2.println("\"");

  unsigned long start = millis();
  bool promptSeen = false;
  while (millis() - start < 5000) {
    if (Serial2.available()) {
      char c = (char)Serial2.read();
      if (c == '>') { promptSeen = true; break; }
      feedModem(String(c));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!promptSeen) {
    Serial2.write(0x1B);                 // ESC: evtl. offene Eingabe abbrechen
    xSemaphoreGive(serial2Mutex);
    err = "keine Bereitschaft ('>')";
    return false;
  }

  Serial2.print(req.message);
  Serial2.write(0x1A);

  // Auf Bestätigung (+CMGS) oder Fehler warten
  String confirm = "";
  start = millis();
  while (millis() - start < SMS_SEND_TIMEOUT_MS) {
    while (Serial2.available()) confirm += (char)Serial2.read();
    if (confirm.indexOf("+CMGS") != -1 || confirm.indexOf("ERROR") != -1) {
      vTaskDelay(pdMS_TO_TICKS(100));
      while (Serial2.available()) confirm += (char)Serial2.read();
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  feedModem(confirm + "\n");
  confirm.trim();
  Serial.println("SMS-Antwort: " + confirm);
  xSemaphoreGive(serial2Mutex);

  if (confirm.indexOf("+CMGS") != -1) return true;
  err = confirm.indexOf("ERROR") != -1 ? "Netz meldet Fehler" : "keine Bestätigung vom A9G";
  return false;
}

// v7.2: SMS werden wiederholt, bis sie gesendet sind (max. SMS_MAX_RETRY_MS)
void smsTask(void* pv) {
  SmsRequest req;
  for (;;) {
    if (xQueueReceive(smsQueue, &req, portMAX_DELAY) != pdTRUE) continue;
    smsBusy = true;                 // v7.7: nicht schlafen, solange gesendet wird

    uint32_t firstTry = millis();
    int attempt = 0;
    for (;;) {
      attempt++;
      String err;
      if (trySendSms(req, err)) {
        appendSmsStatus("\nSMS gesendet an: " + String(req.number) +
                        (attempt > 1 ? " (Versuch " + String(attempt) + ")" : ""));
        powerActivity(POST_SMS_AWAKE_MS);   // v7.7: kurz wach bleiben (Rückruf, weitere SMS)
        break;
      }

      Serial.printf("SMS an %s fehlgeschlagen (%s), Versuch %d\n",
                    req.number, err.c_str(), attempt);

      if (millis() - firstTry > SMS_MAX_RETRY_MS) {
        appendSmsStatus("\nAufgegeben nach " + String(attempt) + " Versuchen: " + String(req.number));
        break;
      }
      if (attempt == 1) {
        appendSmsStatus("\nWarte auf A9G: " + String(req.number) + " (" + err +
                        ") – neuer Versuch alle " + String(SMS_RETRY_INTERVAL_MS / 1000) + " s.");
      }
      vTaskDelay(pdMS_TO_TICKS(SMS_RETRY_INTERVAL_MS));
    }
    releaseSlot(req.slot);          // v7.4: gesendet oder aufgegeben -> aus dem Flash löschen
    smsBusy = false;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
