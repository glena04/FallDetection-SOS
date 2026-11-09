// sms_commands.cpp – Befehle per SMS von gespeicherten Notrufnummern
// Fall Detection & SOS Device – Firmware v7.7.3 (PlatformIO)
//
// Befehle (Groß-/Kleinschreibung egal, Satzzeichen werden ignoriert):
//   STANDORT / WO / LOC / LOCATION / POSITION  -> aktueller Standort
//   STATUS                                      -> Sensor, GPS, Mobilfunk, WLAN
// Nur SMS von gespeicherten Notrufnummern werden beantwortet.
#include "sms_commands.h"
#include <WiFi.h>
#include "globals.h"
#include "modem.h"
#include "location.h"
#include "calls.h"
#include "sms_queue.h"
#include "util.h"
#include "power.h"
#include "wifi_manager.h"

// Schutz gegen SMS-Schleifen: max. SMS_CMD_MAX Antworten pro Stunde
static uint32_t cmdWindowStart = 0;
static int      cmdCount       = 0;

static bool commandRateOk() {
  uint32_t now = millis();
  if (cmdWindowStart == 0 || now - cmdWindowStart > 3600000UL) { cmdWindowStart = now; cmdCount = 0; }
  if (cmdCount >= SMS_CMD_MAX_PER_HOUR) return false;
  cmdCount++;
  return true;
}

// Nur Buchstaben behalten, groß schreiben: "Standort?" -> "STANDORT"
static String normalizeCommand(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (isAlpha(c)) out += (char)toupper(c);
  }
  return out;
}

static String statusText() {
  bool fix = false;
  String net;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    fix = gpsFix;
    net = modemNetStatus;
    xSemaphoreGive(stateMutex);
  }
  String wifi = !wifiActive() ? "aus" :
                (apMode ? "Setup-Modus" : (WiFi.status() == WL_CONNECTED ? "verbunden" : "getrennt"));
  String gps = fix ? "Fix" : ((POWER_SAVE_ENABLED && !gpsIsActive()) ? "aus" : "kein Fix");
  return String("STATUS: Sturzsensor ") + (sensorOk ? "OK" : "AUSGEFALLEN") +
         ", GPS " + gps +
         ", " + net +
         ", WLAN " + wifi + ", " + powerStatusText() + ".";
}

static void handleSmsCommand(const String& sender, const String& body) {
  Serial.println("SMS empfangen von " + sender + ": " + body);

  if (!isAuthorizedCaller(sender)) {
    Serial.println("-> ignoriert (keine gespeicherte Notrufnummer).");
    return;
  }

  String cmd = normalizeCommand(body);
  bool isLocation = (cmd == "STANDORT" || cmd == "WO" || cmd == "LOC" || cmd == "LOCATION" || cmd == "POSITION");
  bool isSetup = (cmd == "SETUP" || cmd == "WLAN" || cmd == "WIFI");
  String cmdKey = isLocation ? "LOC" : (cmd == "STATUS" ? "STATUS" : (isSetup ? "SETUP" : "OTHER"));

  // v7.6.6: gleicher Befehl vom gleichen Absender innerhalb von 2 Minuten -> nur einmal antworten
  // (z. B. wenn das Netz viele alte SMS auf einmal nachliefert)
  static String   lastSender, lastKey;
  static uint32_t lastTime = 0;
  String senderKey = digitsOnly(sender);
  if (senderKey.length() > 9) senderKey = senderKey.substring(senderKey.length() - 9);
  if (lastTime != 0 && senderKey == lastSender && cmdKey == lastKey &&
      millis() - lastTime < SMS_CMD_DUPLICATE_MS) {
    Serial.println("-> bereits beantwortet (doppelte Anfrage).");
    return;
  }

  if (!commandRateOk()) {
    Serial.println("-> ignoriert (zu viele SMS-Befehle in der letzten Stunde).");
    return;
  }
  lastSender = senderKey;
  lastKey    = cmdKey;
  lastTime   = millis();

  String reply;

  if (isLocation && POWER_SAVE_ENABLED) {
    // v7.7: GPS/WLAN einschalten, Antwort folgt nach der Ortung (max. 20 s)
    locationPrepare();
    locationQueueReply(sender, "STANDORT-ABFRAGE:");
    Serial.println("-> Ortung gestartet, Antwort folgt.");
    appendSmsStatus("\n📩 Standort-Abfrage von " + sender + " – Ortung läuft.");
    return;
  } else if (isLocation) {
    String lat, lon, src; int acc = 0;
    int q = getBestLocation(lat, lon, src, acc);
    reply = "STANDORT-ABFRAGE:" + buildLocationText(q, lat, lon, src, acc);
    if (q < 3) {
      reply += " GPS wird gesucht.";
      locRefreshRequested = true;          // Ersatz-Standort sofort erneuern
    }
  } else if (cmd == "STATUS") {
    reply = statusText();
  } else if (isSetup) {
    // v7.7: WLAN + Webseite für 10 Minuten (Heim-WLAN, sonst Setup-WLAN)
    wifiRequest(WIFI_SETUP_WINDOW_MS, true);
    powerActivity(WIFI_SETUP_WINDOW_MS);
    reply = "SETUP: WLAN fuer " + String(WIFI_SETUP_WINDOW_MS / 60000) +
            " min an. Heim-WLAN oder Setup-WLAN " + String(AP_SSID) + " (192.168.4.1).";
  } else {
    reply = "Unbekannter Befehl. Moeglich: STANDORT, STATUS, SETUP";
  }

  if (queueSms(sender, reply)) {
    Serial.println("-> Antwort in Warteschlange: " + reply);
    appendSmsStatus("\n📩 SMS-Befehl von " + sender + " beantwortet.");
  }
}

void processIncomingSms() {
  // v7.6.1: zuerst direkt zugestellte SMS (+CMT) beantworten
  String snd[SMS_DIRECT_MAX], txt[SMS_DIRECT_MAX];
  int n = 0;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    n = smsDirectCount;
    for (int i = 0; i < n; i++) { snd[i] = smsDirectSender[i]; txt[i] = smsDirectBody[i]; }
    smsDirectCount = 0;
    xSemaphoreGive(stateMutex);
  }
  for (int i = 0; i < n; i++) handleSmsCommand(snd[i], txt[i]);

  // Alle ungelesenen SMS lesen (Textmodus):
  // +CMGL: 1,"REC UNREAD","+4917612345678",,"26/10/26,15:30:00+04"
  // Standort
  String r = sendATCommand("AT+CMGL=\"REC UNREAD\"", 3000);
  if (r.indexOf("OK") == -1) {
    // v7.6.2: Modem war belegt (z. B. GPRS-Aufbau) -> später erneut versuchen, nichts löschen
    Serial.println("SMS-Abfrage nicht moeglich (Modem belegt) - neuer Versuch.");
    smsReceived = true;
    return;
  }
  if (r.indexOf("+CMGL:") != -1) Serial.println("Gespeicherte SMS gefunden (roh):\n" + r);

  int pos = 0;
  while ((pos = r.indexOf("+CMGL:", pos)) != -1) {
    int eol = r.indexOf('\n', pos);
    if (eol == -1) break;
    String header = r.substring(pos + 6, eol);   // ohne "+CMGL:"
    header.trim();

    // v7.6.5: Felder an Kommas trennen (Kommas in "..." zählen nicht), Anführungszeichen entfernen
    // Feld 0 = Index, 1 = Status, 2 = Absender – funktioniert mit und ohne Anführungszeichen
    String field[5];
    int f = 0;
    bool inQuotes = false;
    for (size_t k = 0; k < header.length() && f < 5; k++) {
      char c = header.charAt(k);
      if (c == '"') inQuotes = !inQuotes;
      else if (c == ',' && !inQuotes) f++;
      else if (f < 5) field[f] += c;
    }
    int    index  = field[0].toInt();
    String sender = field[2];
    sender.trim();

    int next  = r.indexOf("+CMGL:", eol);
    int okPos = r.indexOf("\nOK", eol);
    int end   = (next != -1) ? next : (okPos != -1 ? okPos : (int)r.length());

    String body = r.substring(eol + 1, end);
    body.trim();

    if (sender.length() && index > 0) {
      handleSmsCommand(sender, body);
      sendATCommand("AT+CMGD=" + String(index), 2000);   // v7.6.5: genau diese SMS löschen
    } else {
      Serial.println("SMS konnte nicht gelesen werden (Kopfzeile: " + header + ") - bleibt gespeichert.");
    }
    pos = end;
  }

  // v7.7.2: bereits gelesene, aber nicht verarbeitete SMS löschen (ungelesene bleiben),
  // damit der SIM-Speicher nicht wieder voll läuft (SMSFULL -> keine neuen SMS)
  sendATCommand("AT+CMGD=1,1", 2000);
}
