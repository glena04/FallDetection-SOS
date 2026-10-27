// util.cpp – Text-Helfer
// Fall Detection & SOS Device – Firmware v7.6 (PlatformIO)
#include "util.h"

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

// Telefonnummer prüfen: optional '+', dann 6–15 Ziffern. Leerzeichen, - / ( ) werden entfernt.
bool normalizePhone(String in, String& out) {
  in.trim();
  out = "";
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (isDigit(c)) out += c;
    else if (c == '+' && out.length() == 0) out += c;
    else if (c == ' ' || c == '-' || c == '/' || c == '(' || c == ')') continue;
    else return false;
  }
  int digits = out.length() - (out.startsWith("+") ? 1 : 0);
  return digits >= 6 && digits <= 15;
}

// SMS-Text bereinigen: Umlaute ersetzen, nur druckbare ASCII-Zeichen.
// Wichtig: verhindert, dass Steuerzeichen (z. B. Strg+Z) AT-Befehle ins Modem einschleusen.
String sanitizeSmsText(String in) {
  in.replace("ä", "ae"); in.replace("ö", "oe"); in.replace("ü", "ue");
  in.replace("Ä", "Ae"); in.replace("Ö", "Oe"); in.replace("Ü", "Ue"); in.replace("ß", "ss");
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (c >= 0x20 && c <= 0x7E) out += c;
  }
  return out;
}

String digitsOnly(const String& s) {
  String d;
  for (size_t i = 0; i < s.length(); i++) if (isDigit(s.charAt(i))) d += s.charAt(i);
  return d;
}

// Einfache JSON-Zahl lesen: "key": 12.34
bool jsonNumber(const String& json, const String& key, String& out) {
  int i = json.indexOf("\"" + key + "\"");
  if (i == -1) return false;
  i = json.indexOf(':', i);
  if (i == -1) return false;
  int j = i + 1;
  while (j < (int)json.length() && (json.charAt(j) == ' ')) j++;
  int k = j;
  while (k < (int)json.length() && (isDigit(json.charAt(k)) || json.charAt(k) == '.' || json.charAt(k) == '-')) k++;
  if (k == j) return false;
  out = json.substring(j, k);
  return true;
}

static bool isNumberStr(const String& s) {
  if (s.length() < 3) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (!isDigit(c) && c != '.' && c != '-') return false;
  }
  return true;
}

// "lat,lon" aus einer Modem-Antwort lesen
bool extractLatLon(const String& resp, String& lat, String& lon) {
  int from = 0;
  while (from < (int)resp.length()) {
    int nl = resp.indexOf('\n', from);
    if (nl == -1) nl = resp.length();
    String line = resp.substring(from, nl);
    from = nl + 1;
    line.replace("+LOCATION:", "");
    line.trim();
    int c = line.indexOf(',');
    if (c <= 0) continue;
    String a = line.substring(0, c), b = line.substring(c + 1);
    a.trim(); b.trim();
    if (isNumberStr(a) && isNumberStr(b) && a.toFloat() != 0.0f) { lat = a; lon = b; return true; }
  }
  return false;
}
