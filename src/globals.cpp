// globals.cpp – Definition des gemeinsamen Zustands + Status-Helfer
// Fall Detection & SOS Device – Firmware v7.5.1 (PlatformIO)
#include "globals.h"

SemaphoreHandle_t stateMutex   = nullptr;
SemaphoreHandle_t serial2Mutex = nullptr;
QueueHandle_t     smsQueue     = nullptr;

Preferences prefs;
String wifiSSID;
String wifiPass;
String phoneNumbers[3] = {"", "", ""};

String latitude  = "0.0";
String longitude = "0.0";
bool   gpsFix    = false;

String        smsStatusText  = "Bereit.";
bool          apMode         = false;
volatile bool restartPending = false;
unsigned long restartTimer   = 0;

bool          mpuReady = false;
volatile bool sensorOk = false;
float mpuAccX = 0, mpuAccY = 0, mpuAccZ = 0;
float mpuGyrX = 0, mpuGyrY = 0, mpuGyrZ = 0;

volatile bool alarmPending   = false;
volatile bool alarmCancelled = false;

volatile bool callActive      = false;
volatile bool callerPending   = false;
volatile bool hangupRequested = false;
String        pendingCaller;

volatile bool     gprsReady           = false;
volatile bool     locRefreshRequested = false;
volatile int      alarmLocQuality     = -1;
volatile uint32_t alarmTime           = 0;
String            modemNetStatus      = "Mobilfunk: unbekannt";

void loadConfig() {
  prefs.begin("sosconfig", false);
  wifiSSID = prefs.getString("ssid", DEFAULT_SSID);
  wifiPass = prefs.getString("pass", DEFAULT_PASS);
  phoneNumbers[0] = prefs.getString("phone1", "");
  phoneNumbers[1] = prefs.getString("phone2", "");
  phoneNumbers[2] = prefs.getString("phone3", "");

  Serial.println("--- Gespeicherte Konfiguration ---");
  Serial.println("SSID: " + wifiSSID);
  for (int i = 0; i < 3; i++) {
    Serial.println("Nummer " + String(i + 1) + ": " +
                   (phoneNumbers[i].length() ? phoneNumbers[i] : String("(leer)")));
  }
  Serial.println("----------------------------------");
}

String getSmsStatus() {
  String s;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    s = smsStatusText;
    xSemaphoreGive(stateMutex);
  }
  return s;
}
void setSmsStatus(const String& s) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    smsStatusText = s;
    xSemaphoreGive(stateMutex);
  }
}
void appendSmsStatus(const String& s) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    smsStatusText += s;
    // v7.4: Text begrenzen, damit der Speicher über Wochen nicht vollläuft
    if (smsStatusText.length() > STATUS_MAX_LEN) {
      int from = smsStatusText.indexOf('\n', smsStatusText.length() - (STATUS_MAX_LEN * 3) / 4);
      smsStatusText = "..." + (from >= 0 ? smsStatusText.substring(from)
                                         : smsStatusText.substring(smsStatusText.length() - STATUS_MAX_LEN / 2));
    }
    xSemaphoreGive(stateMutex);
  }
}
