// location.h – GPS, AGPS, WLAN-/Funkzellen-Ortung, GPS-Nachmeldung
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#pragma once
#include <Arduino.h>

// Qualität: 3 = GPS, 2 = WLAN, 1 = Funkzelle, 0 = keine
int    getBestLocation(String& lat, String& lon, String& src, int& acc);
String buildLocationText(int q, const String& lat, const String& lon, const String& src, int acc);
void   gpsTask(void* pv);
