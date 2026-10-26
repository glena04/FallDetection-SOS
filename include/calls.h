// calls.h – Rückruf / Mithören
// Fall Detection & SOS Device – Firmware v7.5.1 (PlatformIO)
#pragma once
#include <Arduino.h>

bool isAuthorizedCaller(const String& caller);
void callTask(void* pv);
