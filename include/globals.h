// globals.h – gemeinsamer Zustand aller Tasks
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "config.h"

// ---- SMS-Auftrag ----
struct SmsRequest {
  char number[32];
  char message[300];
  int8_t slot;          // Speicherplatz im Flash (-1 = nicht gespeichert)
};

// ---- Synchronisation ----
extern SemaphoreHandle_t stateMutex;    // schützt: Standort, Nummern, Statustext, pendingCaller, MPU-Werte
extern SemaphoreHandle_t serial2Mutex;  // schützt: UART2 zum A9G
extern QueueHandle_t     smsQueue;      // Warteschlange für SMS-Aufträge

// ---- Einstellungen (Flash) ----
extern Preferences prefs;
extern String wifiSSID;
extern String wifiPass;
extern String phoneNumbers[3];

// ---- GPS (stateMutex) ----
extern String latitude;
extern String longitude;
extern bool   gpsFix;

// ---- Status ----
extern String        smsStatusText;
extern bool          apMode;
extern volatile bool restartPending;
extern unsigned long restartTimer;

// ---- Sturzsensor ----
extern bool          mpuReady;
extern volatile bool sensorOk;
extern float mpuAccX, mpuAccY, mpuAccZ;
extern float mpuGyrX, mpuGyrY, mpuGyrZ;

// ---- Abbruch-Countdown ----
extern volatile bool alarmPending;
extern volatile bool alarmCancelled;

// ---- Anruf ----
extern volatile bool callActive;
extern volatile bool callerPending;
extern volatile bool hangupRequested;
extern String        pendingCaller;

// ---- Ortung / Nachmeldung ----
extern volatile bool     gprsReady;
extern volatile bool     locRefreshRequested;
extern volatile int      alarmLocQuality;   // -1 = keine offene Nachmeldung, 0 = kein, 1 = Zelle, 2 = WLAN
extern volatile uint32_t alarmTime;
extern String            modemNetStatus;    // v7.5.1: Mobilfunk-Status für Webseite (stateMutex)

// ---- Helfer ----
void   loadConfig();
String getSmsStatus();
void   setSmsStatus(const String& s);
void   appendSmsStatus(const String& s);
