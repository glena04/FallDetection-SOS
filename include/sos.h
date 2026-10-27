// sos.h – SOS-Taster und Notruf-SMS
// Fall Detection & SOS Device – Firmware v7.6 (PlatformIO)
#pragma once
#include <Arduino.h>

int  enqueueSOSForAllNumbers(const String& baseMessage);   // Notruf + bester Standort an alle
void buttonTask(void* pv);
