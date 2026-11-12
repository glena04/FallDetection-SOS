// wifi_manager.h – WLAN-Verbindung und Setup-Access-Point
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
#pragma once
#include <Arduino.h>

void wifiBegin();                          // beim Start: verbinden (Energiesparen: Setup-Fenster)
void wifiLoop();                           // im loop(): Verbindung, AP, Abschalten
void wifiRequest(uint32_t ms, bool allowAp); // WLAN für ms einschalten/verlängern (thread-sicher)
bool wifiActive();                         // WLAN-Funk eingeschaltet
bool wifiConnecting();                     // Verbindung wird gerade aufgebaut
