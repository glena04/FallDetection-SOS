// modem.h – A9G: AT-Befehle, Modem-Meldungen, SMS-Versand
// Fall Detection & SOS Device – Firmware v7.8.2 (PlatformIO)
#pragma once
#include <Arduino.h>
#include "globals.h"

String sendATCommand(String command, unsigned long timeout);
int    modemPing(uint32_t timeoutMs);    // v7.8.2: 1 = OK, 0 = keine Antwort, -1 = UART belegt
void   feedModem(const String& data);    // nur mit serial2Mutex aufrufen
void   drainModem();                     // nur mit serial2Mutex aufrufen
void   modemInit();                      // SMS-Modus, Anrufer-ID, SMS-Speicher
void   modemEnableSleep();               // v7.7: A9G-Schlafmodus 1 (AT+SLEEP=1) einschalten
void   modemPinsBegin();                 // v7.8: Weck- und Power-Key-Pin einrichten
void   a9gSleepPin();                    // v7.8: Schlafmodus 2 – A9G per Pin schlafen legen
void   a9gWakePin();                     // v7.8: Schlafmodus 2 – A9G per Pin wecken
void   a9gPressPowerKey(uint32_t ms);    // v7.8: Power-Taste "drücken"
void   consoleSetWakePin(bool high);     // v7.8: Test über die Konsole
bool   ensureGprs();                     // mobile Daten aktivieren
bool   trySendSms(const SmsRequest& req, String& err);
void   smsTask(void* pv);
