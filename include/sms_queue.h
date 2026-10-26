// sms_queue.h – SMS-Warteschlange (im Flash gesichert)
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#pragma once
#include <Arduino.h>

int  smsQueueBegin();                                      // Warteschlange anlegen, gespeicherte SMS laden
bool queueSms(const String& number, const String& message);
void releaseSlot(int8_t slot);
int  enqueueToAll(const String& message);                  // an alle gespeicherten Nummern
