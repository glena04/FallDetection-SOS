// modem.h – A9G: AT-Befehle, Modem-Meldungen, SMS-Versand
// Fall Detection & SOS Device – Firmware v7.6 (PlatformIO)
#pragma once
#include <Arduino.h>
#include "globals.h"

String sendATCommand(String command, unsigned long timeout);
void   feedModem(const String& data);    // nur mit serial2Mutex aufrufen
void   drainModem();                     // nur mit serial2Mutex aufrufen
void   modemInit();                      // SMS-Modus, Anrufer-ID, AGPS/GPS
bool   ensureGprs();                     // mobile Daten aktivieren
bool   trySendSms(const SmsRequest& req, String& err);
void   smsTask(void* pv);
