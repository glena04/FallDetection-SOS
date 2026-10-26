// wifi_manager.cpp – WLAN-Verbindung, Setup-AP, automatische Neuverbindung
// Fall Detection & SOS Device – Firmware v7.5.1 (PlatformIO)
#include "wifi_manager.h"
#include <WiFi.h>
#include "globals.h"

static unsigned long lastWifiCheck = 0;
static unsigned long lastApRetry   = 0;
static unsigned long apRetryStart  = 0;
static bool          apRetryActive = false;

void wifiBegin() {
  // WLAN verbinden
  Serial.print("Verbinde mit WLAN \"");
  Serial.print(wifiSSID);
  Serial.print("\" ");

  WiFi.mode(WIFI_STA);
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(true);
  delay(100);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.print("Verbunden! IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
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
}

void wifiLoop() {
  // v7.4: im AP-Modus regelmäßig wieder mit dem WLAN verbinden
  // (nur wenn gerade niemand am Setup-WLAN angemeldet ist)
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

  if (!apMode && millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN verloren - versuche Neuverbindung...");
      WiFi.reconnect();
    }
  }
}
