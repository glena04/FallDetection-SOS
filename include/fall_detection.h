// fall_detection.h – Sturzerkennung mit MPU6050
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
#pragma once
#include <Arduino.h>

bool initMPU();          // Sensor initialisieren (Wire muss gestartet sein)
void mpuTask(void* pv);
bool sensorBusLock(uint32_t timeoutMs);   // v7.7: I2C für den Schlaf sperren
void sensorBusUnlock();  // Sturzerkennung + Sensor-Überwachung + 15 s Abbruch
