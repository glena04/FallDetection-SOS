// location.cpp – GPS, AGPS, WLAN-/Funkzellen-Ortung, GPS-Nachmeldung
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
#include "location.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "globals.h"
#include "modem.h"
#include "sms_queue.h"
#include "util.h"
#include "root_ca.h"
#include "sos.h"
#include "wifi_manager.h"
#include "power.h"

// letzter Ersatz-Standort (stateMutex)
static String   fbLat, fbLon, fbSource;
static int      fbAccuracy = 0;
static uint32_t fbTime     = 0;
static bool     fbValid    = false;
static bool     modemOnline = false;   // A9G antwortet auf "AT"

// ---- v7.7: GPS nach Bedarf + Ortungsaufträge ----
static bool              gpsOn        = false;   // AT+GPS=1 aktiv
static volatile uint32_t gpsHoldUntil = 0;       // GPS bis zu diesem Zeitpunkt an (Energiesparen)
static volatile uint32_t prepareTime  = 0;       // Beginn der letzten Ortung

struct LocJob { bool used; bool toAll; String number; String text; uint32_t deadline; };
static const int LOC_JOBS = 4;
static LocJob    locJobs[LOC_JOBS];

bool modemIsOnline() { return modemOnline; }

bool gpsIsActive() {
  if (!POWER_SAVE_ENABLED) return false;          // ohne Energiesparen ist GPS immer an (kein Grund zum Wachbleiben)
  return gpsOn || (int32_t)(gpsHoldUntil - millis()) > 0;
}

bool locationJobsPending() {
  bool any = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < LOC_JOBS; i++) if (locJobs[i].used) any = true;
    xSemaphoreGive(stateMutex);
  }
  return any;
}

void locationPrepare() {
  locRefreshRequested = true;                     // Ersatz-Standort (WLAN/Zelle) neu holen
  if (!POWER_SAVE_ENABLED) return;
  uint32_t now = millis();
  // nur einen neuen Ortungsbeginn setzen, wenn die letzte Ortung vorbei ist
  if ((int32_t)(gpsHoldUntil - now) <= 0) prepareTime = now;
  uint32_t until = now + ALARM_GPS_HOLD_MS;
  if ((int32_t)(until - gpsHoldUntil) > 0) gpsHoldUntil = until;
  if (USE_WIFI_LOCATION) wifiRequest(ALARM_WIFI_HOLD_MS, false);
  powerActivity(5000);
}

void locationCancelPrepare() {
  if (!POWER_SAVE_ENABLED || locationJobsPending() || alarmLocQuality >= 0) return;
  gpsHoldUntil = millis();                        // GPS wird im gpsTask ausgeschaltet
}

static void addJob(bool toAll, const String& number, const String& text) {
  uint32_t now = millis();
  uint32_t deadline = now;
  if (POWER_SAVE_ENABLED) {
    deadline = prepareTime + ALARM_LOCATION_WAIT_MS;      // Countdown-Zeit zählt schon mit
    if ((int32_t)(deadline - now) < 0) deadline = now;
  }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < LOC_JOBS; i++) {
      if (!locJobs[i].used) {
        locJobs[i] = { true, toAll, number, text, deadline };
        break;
      }
    }
    xSemaphoreGive(stateMutex);
  }
}

void locationQueueAlarm(const String& baseMessage) {
  if (!POWER_SAVE_ENABLED) { enqueueSOSForAllNumbers(baseMessage); return; }   // GPS läuft ohnehin
  addJob(true, "", baseMessage);
}

void locationQueueReply(const String& number, const String& prefix) {
  addJob(false, number, prefix);
}

// Fällige Aufträge senden: sobald GPS-Fix da ist oder die Wartezeit abgelaufen ist
static void processLocationJobs() {
  bool fix = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) { fix = gpsFix; xSemaphoreGive(stateMutex); }

  for (int i = 0; i < LOC_JOBS; i++) {
    LocJob job;
    bool due = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (locJobs[i].used && (fix || (int32_t)(millis() - locJobs[i].deadline) >= 0)) {
        job = locJobs[i];
        locJobs[i].used = false;
        due = true;
      }
      xSemaphoreGive(stateMutex);
    }
    if (!due) continue;

    if (job.toAll) {
      int queued = enqueueSOSForAllNumbers(job.text);
      Serial.printf("Alarm-SMS an %d Nummer(n) in Warteschlange (%s).\n", queued, fix ? "GPS" : "ohne GPS-Fix");
      if (queued > 0) setSmsStatus("Notruf an " + String(queued) + " Nummer(n) in Warteschlange – wird gesendet...");
    } else {
      String lat, lon, src; int acc = 0;
      int q = getBestLocation(lat, lon, src, acc);
      String msg = job.text + buildLocationText(q, lat, lon, src, acc);
      if (q < 3 && gpsIsActive()) msg += " GPS wird gesucht.";
      if (queueSms(job.number, msg)) Serial.println("-> Antwort in Warteschlange: " + msg);
    }
  }
}

// GPS ein-/ausschalten (nur im gpsTask aufrufen)
static void gpsTurnOn() {
  bool agpsOn = false;
  if (USE_AGPS && ensureGprs()) {
    sendATCommand("AT+GPS=0", 1000);
    String r = sendATCommand("AT+AGPS=1", 5000);           // AGPS schaltet GPS mit ein
    agpsOn = r.indexOf("OK") != -1;
    Serial.println(agpsOn ? "AGPS aktiv." : "AGPS nicht moeglich: " + r);
  }
  if (!agpsOn) sendATCommand("AT+GPS=1", 2000);
  gpsOn = true;
  Serial.println("GPS eingeschaltet.");
}

static void gpsTurnOff() {
  sendATCommand("AT+GPS=0", 1000);
  gpsOn = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    gpsFix    = false;
    latitude  = "GPS aus (Energiesparen,";
    longitude = "nur bei Alarm an)";
    xSemaphoreGive(stateMutex);
  }
  if (alarmLocQuality >= 0) {
    alarmLocQuality = -1;
    Serial.println("GPS-Nachmeldung beendet (GPS-Zeit abgelaufen).");
  }
  Serial.println("GPS ausgeschaltet (Energiesparen).");
}

static void parseGPSResponse(const String& response) {
  String newLat, newLon;
  bool   newFix = false;

  int index = response.indexOf("+LOCATION:");
  if (index != -1) {
    String data = response.substring(index + 10);
    data.trim();
    int commaIndex = data.indexOf(',');
    if (commaIndex != -1) {
      newLat = data.substring(0, commaIndex);
      newLon = data.substring(commaIndex + 1);
      newLon.trim();
      newFix = true;
    }
  }

  if (!newFix && response.indexOf(',') != -1 && response.indexOf('.') != -1
      && response.indexOf("GPS NOT FIX") == -1
      && response.indexOf("ERROR") == -1) {
    int commaIndex = response.indexOf(',');
    int dotBefore = response.lastIndexOf('.', commaIndex);
    int startIndex = response.lastIndexOf('\n', dotBefore) + 1;
    while (startIndex < commaIndex && !isDigit(response.charAt(startIndex))) startIndex++;
    if (startIndex < commaIndex) {
      newLat = response.substring(startIndex, commaIndex);
      newLon = response.substring(commaIndex + 1);
      newLon.trim();
      int nl = newLon.indexOf('\n'); if (nl != -1) newLon = newLon.substring(0, nl);
      int cr = newLon.indexOf('\r'); if (cr != -1) newLon = newLon.substring(0, cr);
      newLon.trim();
      newFix = true;
    }
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    if (newFix) {
      latitude  = newLat;
      longitude = newLon;
      gpsFix    = true;
    } else if (response.indexOf("GPS NOT FIX") != -1) {
      latitude  = "Warte auf GPS-Fix...";
      longitude = "Bitte ins Freie gehen!";
      gpsFix    = false;
    } else if (response.indexOf("+CME ERROR") != -1) {
      latitude  = "GPS noch nicht bereit...";
      longitude = "Warte auf Satelliten...";
      gpsFix    = false;
    } else {
      latitude  = "Unbekannte Antwort";
      longitude = "Siehe Seriellen Monitor";
      gpsFix    = false;
    }
    xSemaphoreGive(stateMutex);
  }
}

// v7.3: Beste verfügbare Position. Qualität: 3 = GPS, 2 = WLAN, 1 = Funkzelle, 0 = keine
int getBestLocation(String& lat, String& lon, String& src, int& acc) {
  int q = 0;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    if (gpsFix) {
      lat = latitude; lon = longitude; src = "GPS"; acc = 10; q = 3;
    } else if (fbValid && millis() - fbTime < FALLBACK_MAX_AGE_MS) {
      lat = fbLat; lon = fbLon; src = fbSource; acc = fbAccuracy;
      q = (fbSource == "WLAN") ? 2 : 1;
    }
    xSemaphoreGive(stateMutex);
  }
  return q;
}

// Standorttext für SMS (nur ASCII, damit die SMS sicher ankommt)
String buildLocationText(int q, const String& lat, const String& lon, const String& src, int acc) {
  if (q == 0) return " (Kein Standort verfuegbar)";
  String link = "https://maps.google.com/?q=" + lat + "," + lon;
  if (q == 3) return " Standort (GPS): " + link;
  return " Ca.-Standort (" + src + ", ~" + String(acc) + " m): " + link;
}

// Sendet den genaueren Standort nach, sobald er da ist (GPS sofort,
// einen Ersatz-Standort nur, wenn der Alarm gar keinen Standort hatte)
static void checkFollowUp() {
  if (alarmLocQuality < 0) return;
  if (millis() - alarmTime > FOLLOWUP_MAX_MS) {
    alarmLocQuality = -1;
    Serial.println("Nachmeldung beendet: kein GPS innerhalb der Frist.");
    return;
  }
  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);
  bool better = (q == 3) || (alarmLocQuality == 0 && q > 0);
  if (!better) return;

  int queued = enqueueToAll("UPDATE NOTRUF:" + buildLocationText(q, lat, lon, src, acc));
  if (queued > 0) {
    Serial.println("Nachmeldung gesendet (" + src + ").");
    appendSmsStatus("\n📍 Neuer Standort (" + src + ") nachgesendet.");
    alarmLocQuality = (q == 3) ? -1 : q;    // nach GPS fertig, sonst weiter auf GPS warten
  }
}

// 1) WLAN-Ortung: umliegende WLANs scannen und an Geolocation-API senden
static bool wifiScanFailed = false;   // v7.5.2: Scan fehlgeschlagen -> bald erneut versuchen

// v7.5.2: WLAN-Scan mit bis zu 3 Versuchen. Kurz nach dem Verbinden lehnt der
// WLAN-Treiber Scans manchmal ab - dann kurz warten und erneut versuchen.
static int scanNearbyWifi() {
  for (int attempt = 1; attempt <= 3; attempt++) {
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, true);   // synchron (blockiert nur diesen Task)
    if (n > 0) return n;
    Serial.printf("WLAN-Scan Versuch %d: Ergebnis %d (WLAN-Status %d)\n", attempt, n, (int)WiFi.status());
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  return 0;
}

static bool wifiLocate(String& lat, String& lon, int& acc) {
  if (!USE_WIFI_LOCATION) return false;
  if (apMode || WiFi.status() != WL_CONNECTED) {
    Serial.println("WLAN-Ortung: kein WLAN mit Internet verbunden.");
    return false;
  }

  int n = scanNearbyWifi();
  Serial.printf("WLAN-Ortung: %d Netze gefunden.\n", n);
  wifiScanFailed = (n <= 0);
  if (n < 2) { WiFi.scanDelete(); Serial.println("WLAN-Ortung: zu wenige Netze (mind. 2)."); return false; }

  String body = "{\"considerIp\":false,\"wifiAccessPoints\":[";
  int used = 0;
  for (int i = 0; i < n && used < 12; i++) {
    if (WiFi.SSID(i).endsWith("_nomap")) continue;     // Opt-out respektieren
    if (used) body += ",";
    body += "{\"macAddress\":\"" + WiFi.BSSIDstr(i) + "\",\"signalStrength\":" + String(WiFi.RSSI(i)) + "}";
    used++;
  }
  body += "]}";
  WiFi.scanDelete();
  if (used < 2) { Serial.println("WLAN-Ortung: zu wenige verwendbare Netze."); return false; }
  Serial.printf("WLAN-Ortung: sende %d Netze an %s\n", used, GEO_API_URL);

  WiFiClientSecure client;
  // v7.4 Stufe 3: Zertifikat des Servers prüfen -> keine gefälschten Standorte (Man-in-the-Middle)
  if (GEO_TLS_VERIFY) client.setCACert(GEO_ROOT_CA);
  else                client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, GEO_API_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setUserAgent("ESP32-SOS-Device/7.3");
  http.setTimeout(8000);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code == 404) {
    Serial.println("WLAN-Ortung: Standort unbekannt (HTTP 404) - die Netze sind nicht in der Datenbank.");
    return false;
  }
  if (code != 200) {
    Serial.printf("WLAN-Ortung fehlgeschlagen: HTTP %d %s\n", code, resp.c_str());
    if (code < 0) {                       // Verbindungs-/TLS-Fehler genauer anzeigen
      char tlsErr[120];
      client.lastError(tlsErr, sizeof(tlsErr));
      Serial.printf("TLS/Verbindung: %s\n", tlsErr);
    }
    return false;
  }
  String a;
  if (!jsonNumber(resp, "lat", lat) || !jsonNumber(resp, "lng", lon)) return false;
  acc = jsonNumber(resp, "accuracy", a) ? (int)a.toFloat() : 100;
  return true;
}

// 2) Funkzellen-Ortung über das A9G (AT+LOCATION=1)
static bool cellLocate(String& lat, String& lon) {
  if (!ensureGprs()) return false;
  String r = sendATCommand("AT+LOCATION=1", 6000);
  Serial.println("A9G Funkzellen-Antwort: " + r);
  if (extractLatLon(r, lat, lon)) return true;
  gprsReady = false;                    // beim nächsten Mal neu verbinden
  return false;
}

// Ersatz-Standort holen: zuerst WLAN, dann Funkzelle
static void updateFallbackLocation(bool modemAlive) {
  String lat, lon, src; int acc = 0;
  bool ok = false;
  if (wifiLocate(lat, lon, acc)) { src = "WLAN"; ok = true; }
  else if (USE_CELL_LOCATION && modemAlive && cellLocate(lat, lon)) { src = "Funkzelle"; acc = 1000; ok = true; }

  if (ok) {
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
      fbLat = lat; fbLon = lon; fbSource = src; fbAccuracy = acc;
      fbTime = millis(); fbValid = true;
      xSemaphoreGive(stateMutex);
    }
    Serial.println("Ersatz-Standort (" + src + ", ~" + String(acc) + " m): " + lat + "," + lon);
  } else {
    Serial.println("Kein Ersatz-Standort verfuegbar.");
  }
}

// v7.5.1: Netzstatus des A9G lesen (Registrierung + Signal) und anzeigen
static void updateNetworkStatus() {
  String reg = sendATCommand("AT+CREG?", 800);
  String csq = sendATCommand("AT+CSQ", 800);

  int stat = -1, rssi = -1;
  int i = reg.indexOf("+CREG:");
  if (i != -1) {
    int c = reg.indexOf(',', i);
    if (c != -1) stat = reg.substring(c + 1).toInt();
  }
  i = csq.indexOf("+CSQ:");
  if (i != -1) rssi = csq.substring(i + 5).toInt();

  String regText;
  switch (stat) {
    case 1:  regText = "im Netz (Heimnetz)"; break;
    case 5:  regText = "im Netz (Roaming)"; break;
    case 2:  regText = "NICHT im Netz - sucht Netz..."; break;
    case 3:  regText = "NICHT im Netz - Anmeldung abgelehnt (SIM/2G?)"; break;
    case 0:  regText = "NICHT im Netz - sucht nicht"; break;
    default: regText = "Status unbekannt"; break;
  }
  String sig = (rssi < 0 || rssi == 99) ? "kein Signal" : String(rssi) + "/31";
  String text = "Mobilfunk: " + regText + ", Signal " + sig;

  static String lastText;
  if (text != lastText) { Serial.println("A9G " + text); lastText = text; }
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    modemNetStatus = text;
    xSemaphoreGive(stateMutex);
  }
}

void gpsTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(3000));   // let WiFi settle first
  uint32_t lastFallback = 0;
  uint32_t lastModemCheck = 0;
  bool     fallbackTried = false;
  bool     wifiWasConnected = false;

  if (POWER_SAVE_ENABLED && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    latitude  = "GPS aus (Energiesparen,";
    longitude = "nur bei Alarm an)";
    xSemaphoreGive(stateMutex);
  }

  for (;;) {
    // v7.7: Ist gerade etwas zu tun? (Energiesparen: sonst Modem in Ruhe lassen)
    bool jobs   = locationJobsPending();
    bool wantGps = !POWER_SAVE_ENABLED || (int32_t)(gpsHoldUntil - millis()) > 0;
    bool active = !POWER_SAVE_ENABLED || jobs || wantGps || gpsOn || locRefreshRequested ||
                  alarmLocQuality >= 0;

    // A9G erreichbar?
    // v7.6.3: "AT" bis zu 3x versuchen. Erst nach 3 erfolglosen Runden gilt das A9G
    // als offline - ein kurz belegtes Modem (SMS, GPRS) löst keine Neueinrichtung mehr aus.
    static int failRounds = 0;
    bool alive = modemOnline;
    if (active || !modemOnline || millis() - lastModemCheck > MODEM_CHECK_IDLE_MS) {
      lastModemCheck = millis();
      bool responded = false;
      bool uartBusy  = false;
      for (int i = 0; i < 3 && !responded; i++) {
        int p = modemPing(1000);
        if (p < 0) { uartBusy = true; break; }     // v7.8.2: Modem arbeitet (SMS/GPRS) -> kein Fehler
        responded = (p == 1);
        if (!responded) vTaskDelay(pdMS_TO_TICKS(500));
      }
      if (uartBusy) responded = modemOnline;        // Zustand beibehalten, Runde nicht zählen
      if (!responded && !uartBusy) {
        // v7.7.3: A9G evtl. noch im Schlafmodus (v7.7) -> mehrfach wecken und Schlafmodus aus
        for (int i = 0; i < 5 && !responded; i++) {
          String r = sendATCommand("AT+SLEEP=0", 400);
          responded = r.indexOf("OK") != -1;
        }
        if (responded) Serial.println("A9G aus dem Schlafmodus geholt (AT+SLEEP=0).");
      }
      if (!uartBusy) failRounds = responded ? 0 : failRounds + 1;

      // v7.8: A9G antwortet dauerhaft nicht -> Power-Taste per ESP32 "drücken"
      static uint32_t lastPwrKey = 0;
      if (!responded && !uartBusy && failRounds >= 3 && A9G_PWRKEY_PIN >= 0 &&
          (lastPwrKey == 0 || millis() - lastPwrKey > A9G_PWRKEY_RETRY_MS)) {
        lastPwrKey = millis();
        Serial.println("A9G reagiert nicht - druecke Power-Taste ...");
        powerActivity(90000);                      // v7.8.1: wach bleiben, bis das A9G wieder da ist
        a9gPressPowerKey(A9G_PWRKEY_PRESS_MS);
        vTaskDelay(pdMS_TO_TICKS(15000));          // v7.8.1: Startzeit des A9G (mehr Reserve)
      }
      if (!responded && modemOnline && failRounds < 3) {
        Serial.printf("A9G antwortet nicht (Runde %d/3) - warte ab.\n", failRounds);
      }
      alive = responded || (modemOnline && failRounds < 3);
      if (responded && !modemOnline) {
        Serial.println("A9G erreichbar - wird eingerichtet.");
        gprsReady = false;
        modemInit();
        modemOnline = true;
        gpsOn = false;                       // nach (Neu-)Start ist das GPS aus
      } else if (!alive && modemOnline) {
        Serial.println("A9G nicht erreichbar!");
        modemOnline = false;
        gprsReady   = false;
        gpsOn       = false;
      }

      // v7.5.1: Mobilfunk-Status
      if (alive) {
        updateNetworkStatus();
      } else if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        modemNetStatus = "Mobilfunk: A9G nicht erreichbar";
        xSemaphoreGive(stateMutex);
      }
    }

    // v7.5.1: sobald das WLAN (neu) verbunden ist, Ersatz-Standort sofort versuchen
    // (mit Energiesparen nur, wenn gerade geortet wird)
    bool wifiNow = !apMode && WiFi.status() == WL_CONNECTED;
    if (wifiNow && !wifiWasConnected && (!POWER_SAVE_ENABLED || active)) locRefreshRequested = true;
    wifiWasConnected = wifiNow;

    // v7.7: GPS nach Bedarf ein-/ausschalten
    if (alive && wantGps && !gpsOn)  gpsTurnOn();
    if (alive && !wantGps && gpsOn)  gpsTurnOff();

    // 1) GPS
    if (alive && gpsOn) {
      String resp = sendATCommand("AT+LOCATION=2", 2000);
      Serial.print("A9G GPS Antwort: ");
      Serial.println(resp);
      parseGPSResponse(resp);
    } else if (!alive && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      latitude  = "A9G nicht erreichbar...";
      longitude = "Stromversorgung/Kabel prüfen";
      gpsFix    = false;
      xSemaphoreGive(stateMutex);
    }

    // 2) Ohne GPS-Fix: Ersatz-Standort (WLAN, sonst Funkzelle)
    bool fix = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) { fix = gpsFix; xSemaphoreGive(stateMutex); }
    if (fix) {
      locRefreshRequested = false;
    } else if (POWER_SAVE_ENABLED && locRefreshRequested && wifiConnecting()) {
      // v7.7: WLAN verbindet sich gerade -> kurz warten, dann WLAN-Ortung (vor Funkzelle)
    } else if (locRefreshRequested ||
               (!POWER_SAVE_ENABLED && (!fallbackTried ||
                millis() - lastFallback > (wifiScanFailed ? 30000UL : FALLBACK_INTERVAL_MS)))) {
      locRefreshRequested = false;
      fallbackTried = true;
      lastFallback  = millis();
      updateFallbackLocation(alive);
    }

    // 3) v7.7: fällige Alarm-SMS / Standort-Antworten senden
    processLocationJobs();

    // 4) Genaueren Standort nach einem Alarm nachsenden
    checkFollowUp();

    // Warten: während einer Ortung jede Sekunde, sonst seltener
    bool busyNow = locationJobsPending() || locRefreshRequested || (POWER_SAVE_ENABLED && gpsOn);
    uint32_t waitMs = busyNow ? 1000 : (!alive ? 5000 : (alarmLocQuality >= 0 ? 10000 : GPS_INTERVAL));
    for (uint32_t w = 0; w < waitMs && !locRefreshRequested && !(waitMs > 1000 && locationJobsPending()); w += 500) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}
