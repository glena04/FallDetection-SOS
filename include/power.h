// power.h – Energiesparen: Light Sleep des ESP32, Schlafmodus des A9G
// Fall Detection & SOS Device – Firmware v7.7.3 (PlatformIO)
#pragma once
#include <Arduino.h>

void   powerActivity(uint32_t holdMs);   // mindestens holdMs wach bleiben (thread-sicher)
void   powerTask(void* pv);              // entscheidet, wann geschlafen wird
String powerStatusText();                // kurzer Status für Webseite/SMS
