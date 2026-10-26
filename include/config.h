// config.h – alle Einstellungen (Pins, Schwellen, Zeiten)
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#pragma once
#include <Arduino.h>

// ---- WLAN-Zugangsdaten: aus include/secrets.h (nicht auf GitHub!) ----
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID_DEFAULT
#define WIFI_SSID_DEFAULT "DEIN_WLAN_NAME"
#endif
#ifndef WIFI_PASS_DEFAULT
#define WIFI_PASS_DEFAULT "DEIN_WLAN_PASSWORT"
#endif
const char* const DEFAULT_SSID = WIFI_SSID_DEFAULT;
const char* const DEFAULT_PASS = WIFI_PASS_DEFAULT;

const char* const AP_SSID      = "ESP32-SOS-Setup";
const char* const AP_PASS      = "sos12345";   // min. 8 Zeichen

#define RXD2 16   // ESP32 RX2 -> A9G TX
#define TXD2 17   // ESP32 TX2 -> A9G RX

#define SOS_BUTTON_PIN  4              // Taster gegen GND
// v7.1: Taster wird abgefragt statt per Interrupt -> GSM-Störungen lösen keinen SOS mehr aus
const uint32_t SOS_HOLD_MS    = 2000;   // SOS: Taster 2 s gedrückt halten
const uint32_t CANCEL_HOLD_MS = 100;    // Sturzalarm abbrechen: kurzer Druck
const uint32_t SOS_LOCKOUT_MS = 60000;  // nach einem Taster-SOS 60 s Sperre

// Aktiver Summer für den Abbruch-Countdown (GPIO 14)
#define BUZZER_PIN 14   // aktiver Summer (über Transistor), -1 = nicht vorhanden

// I2C Pins für MPU6050 (ESP32-S3)
#define I2C_SDA 8
#define I2C_SCL 9

// Sturzerkennung
const float FALL_THRESHOLD = 25.0; // ca. 2.5g - Aufprall-Schwelle ohne Freifall

// NEU: Abbruchzeit nach erkanntem Sturz (bevor die SMS gesendet wird)
const uint32_t CANCEL_WINDOW_MS = 15000;   // 15 Sekunden

// NEU: Rückruf / Mithören
const bool LISTEN_IN_ENABLED = true;  // Anrufe von gespeicherten Notrufnummern automatisch annehmen
const bool MUTE_SPEAKER      = true;  // Lautsprecher stumm -> Anrufer hört nur zu

const unsigned long GPS_INTERVAL = 30000;  // 30 Sekunden

// v7.3: AGPS + Ersatz-Standort, solange kein GPS-Fix da ist
const char* const GPRS_APN          = "web.vodafone.de";  // APN der SIM-Karte (z. B. "internet", "web.vodafone.de", "online.telia.se")
const bool  USE_AGPS          = true;        // AGPS: schnellerer GPS-Fix (braucht mobile Daten)
const bool  USE_WIFI_LOCATION = true;        // 1. Ersatz: WLAN-Ortung (braucht WLAN mit Internet)
const bool  USE_CELL_LOCATION = true;        // 2. Ersatz: Funkzellen-Ortung über A9G (braucht mobile Daten)
// beaconDB: kostenlos, ohne Key, aber experimentell. Alternative: Google Geolocation API
// "https://www.googleapis.com/geolocation/v1/geolocate?key=DEIN_KEY"
const char* const GEO_API_URL       = "https://api.beacondb.net/v1/geolocate";
const bool  GEO_TLS_VERIFY    = true;        // v7.4: Server-Zertifikat prüfen (nur zum Testen auf false)

const uint32_t FALLBACK_INTERVAL_MS = 120000;   // Ersatz-Standort alle 2 min erneuern
const uint32_t FALLBACK_MAX_AGE_MS  = 900000;   // Ersatz-Standort höchstens 15 min alt
const uint32_t FOLLOWUP_MAX_MS      = 7200000;  // GPS-Nachmeldung bis 2 h nach dem Alarm

// v7.2: SMS bleiben in der Warteschlange, bis das A9G wieder bereit ist
const uint32_t SMS_RETRY_INTERVAL_MS = 15000;    // alle 15 s neu versuchen
const uint32_t SMS_MAX_RETRY_MS      = 3600000;  // max. 1 Stunde lang versuchen
const uint32_t SMS_SEND_TIMEOUT_MS   = 15000;    // max. Wartezeit auf +CMGS
// v7.4: Ausfallsicherheit
const uint32_t SENSOR_RETRY_MS          = 10000;    // Sensor alle 10 s neu suchen
const uint32_t SENSOR_ALERT_AFTER_MS    = 60000;    // SMS-Warnung nach 1 min Ausfall
const uint32_t SENSOR_ALERT_INTERVAL_MS = 21600000; // Warnung höchstens alle 6 h
const uint32_t AP_RETRY_MS              = 300000;   // im AP-Modus alle 5 min WLAN erneut versuchen
const uint32_t WDT_TIMEOUT_S            = 30;       // Watchdog: Neustart, wenn ein Task 30 s hängt
const size_t   STATUS_MAX_LEN           = 1200;     // max. Länge des Statustexts
const int      SMS_SLOTS                = 8;        // gespeicherte SMS (= Größe der Warteschlange)
#define MPU_ADDR 0x68

// v7.4 Stufe 2: Schutz der Webseite (ohne Login)
const int      WEB_SOS_MAX        = 3;        // Web-SOS: max. 3 ...
const uint32_t WEB_SOS_WINDOW_MS  = 600000;   // ... pro 10 Minuten
const int      WEB_SMS_MAX        = 5;        // manuelle SMS: max. 5 ...
const uint32_t WEB_SMS_WINDOW_MS  = 600000;   // ... pro 10 Minuten
const size_t   MANUAL_SMS_MAX_LEN = 80;       // Länge der eigenen Nachricht (Standort kommt dazu)

// ======================= STURZERKENNUNG =======================
// ---- Parameter Sturzerkennung ----
const float    G_EARTH                 = 9.81f;
const uint32_t SAMPLE_MS               = 20;      // 50 Hz (vorher 10 Hz)
const float    FREEFALL_THRESHOLD      = 0.5f * G_EARTH; // < 0,5 g = Freifall
const uint32_t FREEFALL_MIN_MS         = 60;      // Mindestdauer Freifall
const uint32_t FF_TO_IMPACT_WINDOW_MS  = 800;     // Aufprall muss danach kommen
const float    IMPACT_THRESHOLD        = FALL_THRESHOLD; // 25 m/s² ohne Freifall
const float    IMPACT_THRESHOLD_AFTER_FF = 20.0f;  // niedriger, wenn Freifall gesehen
const uint32_t SETTLE_MS               = 1500;    // Ausrollen nach Aufprall
const float    ORIENTATION_CHANGE_DEG  = 45.0f;   // Lageänderung vorher/nachher
const float    STILL_ACC_DEV           = 1.5f;    // m/s² Abweichung von 1 g
const float    STILL_GYRO              = 0.5f;    // rad/s
