// fall_detection.h – Sturzerkennung mit MPU6050
// Fall Detection & SOS Device – Firmware v7.5.1 (PlatformIO)
#pragma once
#include <Arduino.h>

bool initMPU();          // Sensor initialisieren (Wire muss gestartet sein)
void mpuTask(void* pv);  // Sturzerkennung + Sensor-Überwachung + 15 s Abbruch
