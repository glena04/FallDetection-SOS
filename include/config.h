// config.h – alle Einstellungen (Pins, Schwellen, Zeiten)
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
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
const bool  USE_AGPS          = false;       // AGPS (v7.8.2: aus – A9G-AGPS-Dienst antwortet nicht, CME ERROR 50)
const bool  USE_WIFI_LOCATION = true;        // 1. Ersatz: WLAN-Ortung (braucht WLAN mit Internet)
const bool  USE_CELL_LOCATION = false;       // Funkzellen-Ortung (v7.8.2: aus – A9G liefert keine Daten)
// beaconDB: kostenlos, ohne Key, aber experimentell. Alternative: Google Geolocation API
// "https://www.googleapis.com/geolocation/v1/geolocate?key=DEIN_KEY"
const char* const GEO_API_URL       = "https://api.beacondb.net/v1/geolocate";
const bool  GEO_TLS_VERIFY    = true;        // v7.4: Server-Zertifikat prüfen (nur zum Testen auf false)

const uint32_t FALLBACK_INTERVAL_MS = 120000;   // Ersatz-Standort alle 2 min erneuern
const uint32_t FALLBACK_MAX_AGE_MS  = 900000;   // Ersatz-Standort höchstens 15 min alt
const uint32_t FOLLOWUP_MAX_MS      = 7200000;  // GPS-Nachmeldung bis 2 h nach dem Alarm

// v7.2: SMS bleiben in der Warteschlange, bis das A9G wieder bereit ist
const uint32_t SMS_RETRY_INTERVAL_MS = 10000;    // alle 15 s neu versuchen
const uint32_t SMS_MAX_RETRY_MS      = 3600000;  // max. 1 Stunde lang versuchen
const uint32_t SMS_SEND_TIMEOUT_MS   = 15000;    // max. Wartezeit auf +CMGS
// v7.4: Ausfallsicherheit
const uint32_t SENSOR_RETRY_MS          = 10000;    // Sensor alle 10 s neu suchen
const uint32_t SENSOR_ALERT_AFTER_MS    = 60000;    // SMS-Warnung nach 1 min Ausfall
const uint32_t SENSOR_ALERT_INTERVAL_MS = 21600000; // Warnung höchstens alle 6 h
const uint32_t AP_RETRY_MS              = 60000;    // im AP-Modus jede Minute WLAN erneut versuchen (v7.6.4)
const uint32_t WDT_TIMEOUT_S            = 30;       // Watchdog: Neustart, wenn ein Task 30 s hängt
const size_t   STATUS_MAX_LEN           = 1200;     // max. Länge des Statustexts
const int      SMS_SLOTS                = 8;        // gespeicherte SMS (= Größe der Warteschlange)
#define MPU_ADDR 0x68
 
// v7.4 Stufe 2: Schutz der Webseite (ohne Login)
const int      WEB_SOS_MAX        = 3;        // Web-SOS: max. 3 ...
const uint32_t WEB_SOS_WINDOW_MS  = 600000;   // ... pro 10 Minuten
const int      WEB_SMS_MAX        = 5;        // manuelle SMS: max. 5 ...
const uint32_t WEB_SMS_WINDOW_MS  = 600000;   // ... pro 10 Minuten
const uint32_t SMS_POLL_INTERVAL_MS = 60000;   // v7.6.2: SIM jede Minute auf neue SMS prüfen
const uint32_t SMS_CMD_DUPLICATE_MS = 120000;  // v7.6.6: gleiche Anfrage innerhalb 2 min nur einmal beantworten
const int      SMS_CMD_MAX_PER_HOUR = 10;     // v7.6: max. Antworten auf SMS-Befehle pro Stunde
const size_t   MANUAL_SMS_MAX_LEN = 80;       // Länge der eigenen Nachricht (Standort kommt dazu)

// ======================= ENERGIESPAREN (v7.7) =======================
// Ruhezustand: ESP32 im Light Sleep, A9G im Schlafmodus, GPS und WLAN aus.
// Wecken: SOS-Taste, Bewegung (MPU6050-INT), Modem-Meldung (Anruf/SMS), Timer.
const bool     POWER_SAVE_ENABLED       = true;     // false = alles dauerhaft an (z. B. am USB)
#define MPU_INT_PIN 5                                // MPU6050 INT -> ESP32 GPIO 5 (-1 = nicht verbunden: kein ESP-Schlaf)
// v7.8: A9G-Schlafmodus
//   0 = aus (empfohlen, zuverlässig)
//   1 = AT+SLEEP=1, Wecken per UART (bei deinem Modul: wacht nicht wieder auf!)
//   2 = per Pin: AT+CSCLK=1, ESP32 schaltet A9G-IO25 (A9G_WAKE_PIN) – vorher mit
//       der Konsole testen ("WAKE HIGH" / "WAKE LOW", dann "AT")
#define A9G_SLEEP_MODE 0
const bool     A9G_SLEEP_ENABLED        = (A9G_SLEEP_MODE != 0);
#define A9G_WAKE_PIN           6   // ESP32-GPIO -> A9G IO25 (über Spannungsteiler 2,8 V), -1 = nicht verbunden
#define A9G_WAKE_ACTIVE_HIGH   1   // 1 = HIGH weckt, 0 = LOW weckt (mit Konsolen-Test ermitteln)
// v7.8: Power-Taste des A9G per ESP32 "drücken" (Optokoppler oder Transistor über der Taste)
//   Startet das A9G automatisch, wenn es nicht mehr antwortet (z. B. nach Schlaf oder Akku-Einbruch)
#define A9G_PWRKEY_PIN         7   // ESP32-GPIO -> 1 kΩ -> NPN-Basis, Kollektor an Taste (Seite ≠ GND), -1 = nicht verbunden
#define A9G_PWRKEY_ACTIVE_HIGH 1   // 1 = HIGH "drückt" die Taste (NPN-Transistor)
const uint32_t A9G_PWRKEY_PRESS_MS      = 2500;     // so lange "drücken"
const uint32_t A9G_PWRKEY_RETRY_MS      = 120000;   // höchstens alle 2 min
const uint32_t IDLE_SLEEP_MS            = 600000;   // 10 min nach Start/Bedienung wach, dann Schlaf
const uint32_t WIFI_SETUP_WINDOW_MS     = 600000;   // WLAN nach Start / SMS "SETUP" 10 min an
const uint32_t ALARM_LOCATION_WAIT_MS   = 8000;    // Alarm: max. 20 s auf GPS warten (inkl. Countdown)
const uint32_t ALARM_GPS_HOLD_MS        = 120000;   // GPS nach Alarm 2 min an (GPS-Nachmeldung)
const uint32_t ALARM_WIFI_HOLD_MS       = 30000;    // WLAN für die Ortung 30 s an
const uint32_t POST_SMS_AWAKE_MS        = 30000;    // nach gesendeter SMS noch 30 s wach (Rückruf)
const uint32_t MOTION_WINDOW_MS         = 5000;     // nach Bewegung 5 s wach (Sturzprüfung)
const float    MOTION_AWAKE_DEV         = 3.0f;     // m/s² Abweichung von 1 g = Bewegung
const uint8_t  MOTION_WAKE_THRESHOLD    = 150;      // MPU-Weckschwelle (1 ≈ 2 mg) -> ca. 0,3 g
const uint32_t HOUSEKEEPING_INTERVAL_MS = 900000;   // alle 15 min kurz wach (SMS prüfen)
const uint32_t HOUSEKEEPING_AWAKE_MS    = 15000;
const uint32_t MODEM_WAKE_HOLD_MS       = 10000;    // nach Modem-Meldung 10 s wach
const uint32_t MODEM_CHECK_IDLE_MS      = 600000;   // Modem-Prüfung im Ruhezustand alle 10 min

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
