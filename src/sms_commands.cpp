// sms_commands.cpp – Befehle per SMS von gespeicherten Notrufnummern
// Fall Detection & SOS Device – Firmware v7.6 (PlatformIO)
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
  String wifi = apMode ? "Setup-Modus" : (WiFi.status() == WL_CONNECTED ? "verbunden" : "getrennt");
  return String("STATUS: Sturzsensor ") + (sensorOk ? "OK" : "AUSGEFALLEN") +
         ", GPS " + (fix ? "Fix" : "kein Fix") +
         ", " + net +
         ", WLAN " + wifi + ".";
}

static void handleSmsCommand(const String& sender, const String& body) {
  Serial.println("SMS empfangen von " + sender + ": " + body);

  if (!isAuthorizedCaller(sender)) {
    Serial.println("-> ignoriert (keine gespeicherte Notrufnummer).");
    return;
  }
  if (!commandRateOk()) {
    Serial.println("-> ignoriert (zu viele SMS-Befehle in der letzten Stunde).");
    return;
  }

  String cmd = normalizeCommand(body);
  String reply;

  if (cmd == "STANDORT" || cmd == "WO" || cmd == "LOC" || cmd == "LOCATION" || cmd == "POSITION") {
    String lat, lon, src; int acc = 0;
    int q = getBestLocation(lat, lon, src, acc);
    reply = "STANDORT-ABFRAGE:" + buildLocationText(q, lat, lon, src, acc);
    if (q < 3) {
      reply += " GPS wird gesucht.";
      locRefreshRequested = true;          // Ersatz-Standort sofort erneuern
    }
  } else if (cmd == "STATUS") {
    reply = statusText();
  } else {
    reply = "Unbekannter Befehl. Moeglich: STANDORT, STATUS";
  }

  if (queueSms(sender, reply)) {
    Serial.println("-> Antwort in Warteschlange: " + reply);
    appendSmsStatus("\n📩 SMS-Befehl von " + sender + " beantwortet.");
  }
}

void processIncomingSms() {
  // Alle ungelesenen SMS lesen (Textmodus):
  // +CMGL: 1,"REC UNREAD","+4917612345678",,"26/10/26,15:30:00+04"
  // Standort
  String r = sendATCommand("AT+CMGL=\"REC UNREAD\"", 3000);

  int pos = 0;
  while ((pos = r.indexOf("+CMGL:", pos)) != -1) {
    int eol = r.indexOf('\n', pos);
    if (eol == -1) break;
    String header = r.substring(pos, eol);

    // Absender = 2. Text in Anführungszeichen
    String sender;
    int q1 = header.indexOf('"');
    int q2 = (q1 >= 0) ? header.indexOf('"', q1 + 1) : -1;
    int q3 = (q2 >= 0) ? header.indexOf('"', q2 + 1) : -1;
    int q4 = (q3 >= 0) ? header.indexOf('"', q3 + 1) : -1;
    if (q3 != -1 && q4 > q3) sender = header.substring(q3 + 1, q4);

    int next = r.indexOf("+CMGL:", eol);
    int okPos = r.indexOf("\nOK", eol);
    int end = (next != -1) ? next : (okPos != -1 ? okPos : (int)r.length());

    String body = r.substring(eol + 1, end);
    body.trim();
    if (sender.length()) handleSmsCommand(sender, body);
    pos = end;
  }

  // Gelesene SMS löschen, damit der SIM-Speicher nicht voll läuft
  sendATCommand("AT+CMGD=1,1", 2000);
}
