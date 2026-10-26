// main.cpp – Start: Hardware, Tasks, WLAN, Webserver
// Fall Detection & SOS Device – Firmware v7.5 (PlatformIO)
#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include "globals.h"
#include "sms_queue.h"
#include "modem.h"
#include "location.h"
#include "calls.h"
#include "sos.h"
#include "fall_detection.h"
#include "web.h"
#include "wifi_manager.h"

// Watchdog konfigurieren (Neustart, wenn ein überwachter Task hängt)
static void watchdogBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = { .timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  delay(500);

  Serial.println();
  Serial.println("ESP32 startet... (Firmware v7.5)");

  // Synchronisation
  stateMutex   = xSemaphoreCreateMutex();
  serial2Mutex = xSemaphoreCreateMutex();

  watchdogBegin();

  // SOS-Taster und Summer
  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);
  if (BUZZER_PIN >= 0) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // MPU6050 (I2C) – falls nicht gefunden, versucht mpuTask es weiter
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);
  mpuReady = initMPU();

  // Einstellungen + gespeicherte SMS laden
  loadConfig();
  int restored = smsQueueBegin();
  if (restored > 0) {
    Serial.printf("%d nicht gesendete SMS aus dem Flash wiederhergestellt.\n", restored);
    setSmsStatus("Nach Neustart: " + String(restored) + " noch nicht gesendete SMS werden erneut gesendet.");
  }

  // Tasks
  xTaskCreate(gpsTask,    "gpsTask",   10240, NULL, 1, NULL); // GPS + Ersatz-Ortung (HTTPS braucht Stack)
  xTaskCreate(smsTask,    "smsTask",    4096, NULL, 2, NULL);
  xTaskCreate(buttonTask, "buttonTask", 4096, NULL, 1, NULL);
  xTaskCreate(mpuTask,    "mpuTask",    8192, NULL, 1, NULL); // Sturzerkennung (50 Hz)
  xTaskCreate(callTask,   "callTask",   4096, NULL, 2, NULL); // Rückruf / Mithören

  wifiBegin();
  webBegin();
}

void loop() {
  if (restartPending && millis() - restartTimer > 2000) {
    Serial.println("Neustart...");
    delay(100);
    ESP.restart();
  }
  wifiLoop();
  vTaskDelay(pdMS_TO_TICKS(500));
}
