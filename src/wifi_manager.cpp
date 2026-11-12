// wifi_manager.cpp – WLAN-Verbindung, Setup-AP, automatische Neuverbindung
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
//
// v7.7: WLAN läuft nicht mehr blockierend. Mit Energiesparen ist es nur an:
//   - 10 min nach dem Start und nach der SMS "SETUP" (mit Setup-AP als Ersatz)
//   - kurz während eines Alarms für die WLAN-Ortung (ohne Setup-AP)
//   - solange die Webseite benutzt wird
// Alle WLAN-Befehle laufen hier im loop()-Task (andere Tasks stellen nur Anfragen).
#include "wifi_manager.h"
#include <WiFi.h>
#include "globals.h"

static bool          wifiIsOn        = false;
static bool          allowApFallback = false;
static bool          connectedLogged = false;
static uint32_t      connectStart    = 0;
static uint32_t      wifiOnUntil     = 0;
static unsigned long lastWifiCheck   = 0;
static unsigned long lastApRetry     = 0;
static unsigned long apRetryStart    = 0;
static bool          apRetryActive   = false;

// Anfragen anderer Tasks
static volatile uint32_t reqMs = 0;
static volatile bool     reqAp = false;

void wifiRequest(uint32_t ms, bool allowAp) {
  if (ms > reqMs) reqMs = ms;
  if (allowAp) reqAp = true;
}

bool wifiActive() { return wifiIsOn; }

bool wifiConnecting() {
  return wifiIsOn && !apMode && WiFi.status() != WL_CONNECTED && millis() - connectStart < 12000;
}

static void startAp() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.println("WLAN-Verbindung fehlgeschlagen!");
  Serial.println("Konfigurations-Access-Point gestartet.");
  Serial.println("WLAN: " + String(AP_SSID));
  Serial.println("Passwort: " + String(AP_PASS));
  Serial.println("IP-Adresse: " + WiFi.softAPIP().toString());
}

static void wifiStart(bool allowAp) {
  Serial.println("WLAN ein: verbinde mit \"" + wifiSSID + "\" ...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  wifiIsOn        = true;
  apMode          = false;
  apRetryActive   = false;
  allowApFallback = allowAp;
  connectedLogged = false;
  connectStart    = millis();
  lastWifiCheck   = millis();
}

static void wifiStop() {
  Serial.println("WLAN aus (Energiesparen).");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiIsOn      = false;
  apMode        = false;
  apRetryActive = false;
}

void wifiBegin() {
  // v7.7.1: WLAN sofort starten – das initialisiert auch den TCP/IP-Stack,
  // der vor webBegin() (AsyncTCP) bereit sein muss. Sonst: "tcpip_api_call ... Invalid mbox".
  if (POWER_SAVE_ENABLED) wifiOnUntil = millis() + WIFI_SETUP_WINDOW_MS;
  wifiStart(true);
}

void wifiLoop() {
  // 1) Anfragen übernehmen
  if (reqMs) {
    uint32_t until = millis() + reqMs;
    if (!wifiIsOn || (int32_t)(until - wifiOnUntil) > 0) wifiOnUntil = until;
    if (!wifiIsOn) wifiStart(reqAp);
    else if (reqAp) allowApFallback = true;
    reqMs = 0;
    reqAp = false;
  }
  if (!wifiIsOn) return;

  // 2) Energiesparen: Zeitfenster abgelaufen -> aus
  if (POWER_SAVE_ENABLED && (int32_t)(millis() - wifiOnUntil) > 0) {
    wifiStop();
    return;
  }

  // 3) Verbindung melden / Setup-AP als Ersatz
  if (!apMode) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!connectedLogged) {
        connectedLogged = true;
        Serial.print("WLAN verbunden! IP-Adresse: ");
        Serial.println(WiFi.localIP());
      }
    } else if (allowApFallback && millis() - connectStart > 15000) {
      startAp();
      lastApRetry = millis();
    }
  }

  // 4) im AP-Modus regelmäßig wieder mit dem WLAN verbinden
  //    (nur wenn gerade niemand am Setup-WLAN angemeldet ist)
  if (apMode && !apRetryActive && millis() - lastApRetry > AP_RETRY_MS &&
      WiFi.softAPgetStationNum() == 0) {
    lastApRetry   = millis();
    apRetryActive = true;
    apRetryStart  = millis();
    Serial.println("AP-Modus: versuche erneut WLAN \"" + wifiSSID + "\" ...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  }
  if (apRetryActive) {
    if (WiFi.status() == WL_CONNECTED) {
      apRetryActive = false;
      apMode        = false;
      connectedLogged = true;
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      Serial.print("WLAN wieder verbunden! IP-Adresse: ");
      Serial.println(WiFi.localIP());
    } else if (millis() - apRetryStart > 15000) {
      apRetryActive = false;
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);                   // Setup-WLAN bleibt aktiv
      Serial.println("WLAN weiterhin nicht erreichbar - bleibe im AP-Modus.");
    }
  }

  // 5) verlorene Verbindung wiederherstellen
  if (!apMode && connectedLogged && millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN verloren - versuche Neuverbindung...");
      WiFi.reconnect();
    }
  }
}
