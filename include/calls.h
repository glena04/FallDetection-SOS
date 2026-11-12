// calls.h – Rückruf / Mithören
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
#pragma once
#include <Arduino.h>

bool isAuthorizedCaller(const String& caller);
void callTask(void* pv);
