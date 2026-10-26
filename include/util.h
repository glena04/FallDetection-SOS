// util.h – Text-Helfer
// Fall Detection & SOS Device – Firmware v7.5 (PlatformIO)
#pragma once
#include <Arduino.h>

String htmlEscape(const String& s);
bool   normalizePhone(String in, String& out);   // +49... prüfen, Zeichen bereinigen
String sanitizeSmsText(String in);               // nur ASCII, keine Steuerzeichen
String digitsOnly(const String& s);
bool   jsonNumber(const String& json, const String& key, String& out);
bool   extractLatLon(const String& resp, String& lat, String& lon);
