// web.cpp – Weboberfläche: Seiten, Formulare, Schutz (Host/Origin, Limits)
// Fall Detection & SOS Device – Firmware v7.6 (PlatformIO)
#include "web.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "globals.h"
#include "webpage.h"
#include "util.h"
#include "location.h"
#include "sos.h"
#include "sms_queue.h"

static AsyncWebServer server(80);

// Begrenzung für Web-Aktionen (läuft nur im Webserver-Task)
struct RateLimit { uint32_t windowStart; int count; int maxCount; uint32_t windowMs; };
static RateLimit rlWebSos = {0, 0, WEB_SOS_MAX, WEB_SOS_WINDOW_MS};
static RateLimit rlWebSms = {0, 0, WEB_SMS_MAX, WEB_SMS_WINDOW_MS};

static String buildPage() {
  String page = FPSTR(htmlPage);

  String modeInfo;
  if (apMode) {
    modeInfo = "⚠ Konfigurationsmodus aktiv. Verbinde dich mit dem WLAN \"" +
               String(AP_SSID) + "\" (Passwort: " + String(AP_PASS) +
               ") und öffne http://192.168.4.1";
  } else {
    modeInfo = "WLAN verbunden. IP-Adresse: " + WiFi.localIP().toString();
  }

  String sSSID, sPh[3];
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    sSSID = wifiSSID;
    for (int i = 0; i < 3; i++) sPh[i] = phoneNumbers[i];
    xSemaphoreGive(stateMutex);
  }

  page.replace("__MODE__",   modeInfo);
  page.replace("__SSID__",   htmlEscape(sSSID));
  page.replace("__PHONE1__", htmlEscape(sPh[0]));
  page.replace("__PHONE2__", htmlEscape(sPh[1]));
  page.replace("__PHONE3__", htmlEscape(sPh[2]));
  page.replace("__AP__",     String(AP_SSID));
  page.replace("__THRESHOLD__", String(FALL_THRESHOLD, 1));
  page.replace("__CANCEL__", String(CANCEL_WINDOW_MS / 1000));

  return page;
}

static String resultPage(const String& title, const String& text) {
  String p = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  p += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  p += "<meta http-equiv='refresh' content='3; url=/'>";
  p += "<style>body{font-family:Arial,sans-serif;margin:20px;background:#f4f4f9;color:#333;}";
  p += ".box{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  p += "a{color:#0056b3;}</style></head><body><div class='box'>";
  p += "<h2>" + title + "</h2><p>" + text + "</p>";
  p += "<p><a href='/'>Zurück zur Startseite</a></p></div></body></html>";
  return p;
}

// Formularwert aus dem POST-Body lesen
static String formParam(AsyncWebServerRequest* r, const char* name) {
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  return "";
}

// Nur Aufrufe über die IP des Geräts erlauben (Schutz gegen DNS-Rebinding)
static bool hostOk(AsyncWebServerRequest* r) {
  String host = r->host();
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  if (host.length() == 0) return false;
  return host == WiFi.localIP().toString() || host == WiFi.softAPIP().toString();
}

// Änderungen nur von der eigenen Seite (Schutz gegen CSRF von fremden Webseiten)
static bool sameOrigin(AsyncWebServerRequest* r) {
  String origin;
  const AsyncWebHeader* h = r->getHeader("Origin");
  if (h) origin = h->value();
  else if ((h = r->getHeader("Referer")) != nullptr) origin = h->value();
  if (origin.length() == 0) return true;          // kein Browser (z. B. curl) -> kein CSRF möglich
  if (origin == "null") return false;
  int p = origin.indexOf("://");
  if (p < 0) return false;
  String o = origin.substring(p + 3);
  int slash = o.indexOf('/');
  if (slash >= 0) o = o.substring(0, slash);
  return o.equalsIgnoreCase(r->host());
}

// Zähler pro Zeitfenster (läuft nur im Webserver-Task)
static bool rateAllow(RateLimit& rl) {
  uint32_t now = millis();
  if (rl.windowStart == 0 || now - rl.windowStart > rl.windowMs) { rl.windowStart = now; rl.count = 0; }
  if (rl.count >= rl.maxCount) return false;
  rl.count++;
  return true;
}

// Wrapper für alle Routen: Host prüfen, bei Änderungen zusätzlich Herkunft prüfen
static void addRoute(const char* uri, WebRequestMethodComposite method,
              void (*handler)(AsyncWebServerRequest*), bool changesState) {
  server.on(uri, method, [handler, changesState](AsyncWebServerRequest* r) {
    if (!hostOk(r)) {
      r->send(403, "text/plain; charset=utf-8", "Zugriff nur über die IP-Adresse des Geräts.");
      return;
    }
    if (changesState && !sameOrigin(r)) {
      Serial.println("Blockiert: Anfrage von fremder Webseite an " + r->url());
      r->send(403, "text/plain; charset=utf-8", "Anfrage von fremder Webseite blockiert.");
      return;
    }
    handler(r);
  });
}

static void handleRoot(AsyncWebServerRequest* request) {
  request->send(200, "text/html; charset=utf-8", buildPage());
}

static void handleGetLocation(AsyncWebServerRequest* request) {
  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);

  String gpsState, netState;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    gpsState = latitude + " " + longitude;
    netState = modemNetStatus;
    xSemaphoreGive(stateMutex);
  }

  String text;
  if (q == 3) {
    text = "GPS: Breitengrad: " + lat + " | Längengrad: " + lon;
  } else if (q > 0) {
    text = "≈ " + src + " (ca. " + String(acc) + " m): " + lat + ", " + lon +
           "<br><small>GPS: " + gpsState + "</small>";
  } else {
    text = gpsState;
  }
  text += "<br><small>" + netState + "</small>";
  request->send(200, "text/plain; charset=utf-8", text);
}

// JSON für MPU-Daten
static void handleGetMPU(AsyncWebServerRequest* request) {
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    ax = mpuAccX; ay = mpuAccY; az = mpuAccZ;
    gx = mpuGyrX; gy = mpuGyrY; gz = mpuGyrZ;
    xSemaphoreGive(stateMutex);
  }

  String json = "{";
  json += "\"ax\":" + String(ax, 2) + ",";
  json += "\"ay\":" + String(ay, 2) + ",";
  json += "\"az\":" + String(az, 2) + ",";
  json += "\"gx\":" + String(gx, 2) + ",";
  json += "\"gy\":" + String(gy, 2) + ",";
  json += "\"gz\":" + String(gz, 2) + ",";
  json += "\"ok\":" + String(sensorOk ? "true" : "false");
  json += "}";

  request->send(200, "application/json", json);
}

static void handleGetSMSStatus(AsyncWebServerRequest* request) {
  request->send(200, "text/plain; charset=utf-8", getSmsStatus());
}

// NEU: Sturzalarm über Webseite abbrechen
static void handleCancel(AsyncWebServerRequest* request) {
  if (alarmPending) {
    alarmCancelled = true;
    request->send(200, "text/plain; charset=utf-8", "Sturzalarm abgebrochen – kein Notruf gesendet.");
  } else {
    request->send(200, "text/plain; charset=utf-8", "Kein Sturzalarm aktiv.");
  }
}

// Laufenden Anruf beenden (wird im callTask ausgeführt)
static void handleHangup(AsyncWebServerRequest* request) {
  hangupRequested = true;
  request->send(200, "text/plain; charset=utf-8",
                callActive ? "Anruf wird beendet..." : "Kein Anruf aktiv.");
}

static void handleSavePhones(AsyncWebServerRequest* request) {
  String in[3] = { formParam(request, "p1"), formParam(request, "p2"), formParam(request, "p3") };
  String out[3];

  // v7.4: jede Nummer prüfen (leer = nicht verwendet)
  for (int i = 0; i < 3; i++) {
    in[i].trim();
    if (in[i].length() == 0) { out[i] = ""; continue; }
    if (!normalizePhone(in[i], out[i])) {
      request->send(400, "text/html; charset=utf-8",
                    resultPage("❌ Fehler", "Nummer " + String(i + 1) + " ist ungültig: <b>" + htmlEscape(in[i]) +
                               "</b><br>Format: +49123456789 (6–15 Ziffern). Nichts wurde gespeichert."));
      return;
    }
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    for (int i = 0; i < 3; i++) phoneNumbers[i] = out[i];
    xSemaphoreGive(stateMutex);
  }

  prefs.putString("phone1", out[0]);
  prefs.putString("phone2", out[1]);
  prefs.putString("phone3", out[2]);

  request->send(200, "text/html; charset=utf-8",
                resultPage("✅ Gespeichert", "Die Notrufnummern wurden gespeichert."));
}

static void handleSaveWifi(AsyncWebServerRequest* request) {
  String newSSID = formParam(request, "ssid");
  String newPass = formParam(request, "pass");
  bool   openNet = formParam(request, "open") == "1";
  newSSID.trim();

  String err;
  if (newSSID.length() == 0 || newSSID.length() > 32) err = "Der WLAN-Name (SSID) muss 1–32 Zeichen lang sein.";
  if (openNet) {
    newPass = "";
  } else if (newPass.length() == 0) {
    // v7.4: leeres Feld = bisheriges Passwort behalten (es wird nie mehr angezeigt)
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      newPass = wifiPass;
      xSemaphoreGive(stateMutex);
    }
  } else if (newPass.length() < 8 || newPass.length() > 63) {
    err = "Das WLAN-Passwort muss 8–63 Zeichen lang sein.";
  }
  if (err.length()) {
    request->send(400, "text/html; charset=utf-8", resultPage("❌ Fehler", err));
    return;
  }

  prefs.putString("ssid", newSSID);
  prefs.putString("pass", newPass);

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    wifiSSID = newSSID;
    wifiPass = newPass;
    xSemaphoreGive(stateMutex);
  }

  String page = resultPage("✅ Gespeichert",
                  "Die WLAN-Daten wurden gespeichert.<br>Der ESP32 startet jetzt neu und verbindet sich mit <b>" +
                  htmlEscape(newSSID) + "</b>.<br><br>Falls die Verbindung fehlschlägt, öffnet sich der Access Point <b>" +
                  String(AP_SSID) + "</b>.");
  page.replace("<meta http-equiv='refresh' content='3; url=/'>", "");   // Gerät startet neu -> kein Auto-Reload
  request->send(200, "text/html; charset=utf-8", page);

  restartPending = true;
  restartTimer = millis();
}

static void handleSendSMS(AsyncWebServerRequest* request) {
  String numberIn = formParam(request, "number");
  String message  = formParam(request, "message");
  message.trim();

  String number;
  if (!normalizePhone(numberIn, number)) {
    request->send(400, "text/html; charset=utf-8",
                  resultPage("❌ Fehler", "Ungültige Telefonnummer. Format: +49123456789"));
    return;
  }
  message = sanitizeSmsText(message);
  if (message.length() == 0 || message.length() > MANUAL_SMS_MAX_LEN) {
    request->send(400, "text/html; charset=utf-8",
                  resultPage("❌ Fehler", "Die Nachricht muss 1–" + String(MANUAL_SMS_MAX_LEN) + " Zeichen lang sein."));
    return;
  }
  if (!rateAllow(rlWebSms)) {
    request->send(429, "text/html; charset=utf-8",
                  resultPage("⏳ Limit erreicht", "Maximal " + String(WEB_SMS_MAX) +
                             " manuelle SMS pro 10 Minuten. Bitte später erneut versuchen."));
    return;
  }

  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);
  message += buildLocationText(q, lat, lon, src, acc);

  if (!queueSms(number, message)) {
    request->send(503, "text/html; charset=utf-8", resultPage("❌ Fehler", "SMS-Warteschlange voll."));
    return;
  }

  request->send(200, "text/html; charset=utf-8",
                resultPage("✅ In Warteschlange", "SMS an " + htmlEscape(number) + " wird gesendet."));
}

static void handleSOS(AsyncWebServerRequest* request) {
  bool any = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++) if (phoneNumbers[i].length() > 0) any = true;
    xSemaphoreGive(stateMutex);
  }
  if (!any) {
    request->send(400, "text/plain; charset=utf-8", "Fehler: Keine Notrufnummer gespeichert!");
    return;
  }
  if (!rateAllow(rlWebSos)) {
    request->send(429, "text/plain; charset=utf-8",
                  "Limit erreicht: max. " + String(WEB_SOS_MAX) + " Web-Notrufe pro 10 Minuten. "
                  "Der SOS-Taster funktioniert weiterhin.");
    return;
  }

  int queued = enqueueSOSForAllNumbers("NOTRUF! Ich brauche Hilfe.");
  if (queued == 0) {
    request->send(503, "text/plain; charset=utf-8", "SMS-Warteschlange voll.");
    return;
  }

  setSmsStatus("Notruf wird an " + String(queued) + " Nummer(n) gesendet...");
  request->send(200, "text/plain; charset=utf-8",
                "Notruf-SMS an " + String(queued) + " Nummer(n) in Warteschlange.");
}

void webBegin() {
  // Lesen = GET, Ändern = POST + Herkunftsprüfung
  addRoute("/",             HTTP_GET,  handleRoot,         false);
  addRoute("/getLocation",  HTTP_GET,  handleGetLocation,  false);
  addRoute("/getMPU",       HTTP_GET,  handleGetMPU,       false);
  addRoute("/getSMSStatus", HTTP_GET,  handleGetSMSStatus, false);
  addRoute("/savePhones",   HTTP_POST, handleSavePhones,   true);
  addRoute("/saveWifi",     HTTP_POST, handleSaveWifi,     true);
  addRoute("/sendSMS",      HTTP_POST, handleSendSMS,      true);
  addRoute("/sos",          HTTP_POST, handleSOS,          true);
  addRoute("/cancel",       HTTP_POST, handleCancel,       true);
  addRoute("/hangup",       HTTP_POST, handleHangup,       true);

  // Seite darf nicht in fremde Webseiten eingebettet werden (Clickjacking)
  DefaultHeaders::Instance().addHeader("X-Frame-Options", "DENY");
  DefaultHeaders::Instance().addHeader("Content-Security-Policy", "frame-ancestors 'none'");

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Nicht gefunden");
  });

  server.begin();
  Serial.println("Async HTTP-Server gestartet. Oeffne die IP im Browser!");
}
