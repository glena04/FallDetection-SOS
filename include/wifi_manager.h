// wifi_manager.h – WLAN-Verbindung und Setup-Access-Point
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#pragma once

void wifiBegin();   // verbinden, sonst Setup-AP starten
void wifiLoop();    // Verbindung überwachen, aus dem AP-Modus erneut verbinden
