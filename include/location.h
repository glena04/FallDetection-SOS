// location.h – GPS, AGPS, WLAN-/Funkzellen-Ortung, GPS-Nachmeldung
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
#pragma once
#include <Arduino.h>

// Qualität: 3 = GPS, 2 = WLAN, 1 = Funkzelle, 0 = keine
int    getBestLocation(String& lat, String& lon, String& src, int& acc);
String buildLocationText(int q, const String& lat, const String& lon, const String& src, int acc);
void   gpsTask(void* pv);

// v7.7: Ortung nach Bedarf (Energiesparen)
void locationPrepare();                                        // Alarm beginnt: GPS + WLAN an, Ersatz-Standort holen
void locationQueueAlarm(const String& baseMessage);            // Notruf an alle, sobald Standort da (max. ALARM_LOCATION_WAIT_MS)
void locationQueueReply(const String& number, const String& prefix);  // Antwort an eine Nummer (SMS "STANDORT")
void locationCancelPrepare();                                  // Alarm abgebrochen: GPS wieder frei
bool locationJobsPending();
bool gpsIsActive();                                            // GPS eingeschaltet oder angefordert
bool modemIsOnline();                                          // A9G antwortet
