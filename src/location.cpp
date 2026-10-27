// location.cpp – GPS, AGPS, WLAN-/Funkzellen-Ortung, GPS-Nachmeldung
// Fall Detection & SOS Device – Firmware v7.6 (PlatformIO)
#include "location.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "globals.h"
#include "modem.h"
#include "sms_queue.h"
#include "util.h"
#include "root_ca.h"

// letzter Ersatz-Standort (stateMutex)
static String   fbLat, fbLon, fbSource;
static int      fbAccuracy = 0;
static uint32_t fbTime     = 0;
static bool     fbValid    = false;
static bool     modemOnline = false;   // A9G antwortet auf "AT"

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
  bool     fallbackTried = false;
  bool     wifiWasConnected = false;

  for (;;) {
    // A9G erreichbar?
    bool alive = sendATCommand("AT", 500).indexOf("OK") != -1;
    if (alive && !modemOnline) {
      Serial.println("A9G erreichbar - wird eingerichtet.");
      gprsReady = false;
      modemInit();
      modemOnline = true;
    } else if (!alive && modemOnline) {
      Serial.println("A9G nicht erreichbar!");
      modemOnline = false;
      gprsReady   = false;
    }

    // v7.5.1: Mobilfunk-Status
    if (alive) {
      updateNetworkStatus();
    } else if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      modemNetStatus = "Mobilfunk: A9G nicht erreichbar";
      xSemaphoreGive(stateMutex);
    }

    // v7.5.1: sobald das WLAN (neu) verbunden ist, Ersatz-Standort sofort versuchen
    bool wifiNow = !apMode && WiFi.status() == WL_CONNECTED;
    if (wifiNow && !wifiWasConnected) locRefreshRequested = true;
    wifiWasConnected = wifiNow;

    // 1) GPS
    if (alive) {
      String resp = sendATCommand("AT+LOCATION=2", 2000);
      Serial.print("A9G GPS Antwort: ");
      Serial.println(resp);
      parseGPSResponse(resp);
    } else if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
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
    } else if (!fallbackTried || locRefreshRequested ||
               millis() - lastFallback > (wifiScanFailed ? 30000UL : FALLBACK_INTERVAL_MS)) {
      locRefreshRequested = false;
      fallbackTried = true;
      lastFallback  = millis();
      updateFallbackLocation(alive);
    }

    // 3) Genaueren Standort nach einem Alarm nachsenden
    checkFollowUp();

    // Warten: nach einem Alarm öfter prüfen; bei Alarm sofort weiter
    uint32_t waitMs = !alive ? 5000 : (alarmLocQuality >= 0 ? 10000 : GPS_INTERVAL);
    for (uint32_t w = 0; w < waitMs && !locRefreshRequested; w += 500) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}
