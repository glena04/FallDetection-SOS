//code3WebserverButtonSendSMS v7.3 - Sturzerkennung, Abbruch, Mithoeren, SMS-Wiederholung, AGPS + WLAN/Funkzellen-Ortung
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ======================= EINSTELLUNGEN =======================
// Platzhalter! Eigene Daten eintragen (oder später über die Webseite speichern).
// Echte Passwörter nie auf GitHub hochladen.
const char* DEFAULT_SSID = "Vodafone-EBDF";
const char* DEFAULT_PASS = "";
const char* AP_SSID      = "ESP32-SOS-Setup";
const char* AP_PASS      = "sos12345";   // min. 8 Zeichen

#define RXD2 16   // ESP32 RX2 -> A9G TX
#define TXD2 17   // ESP32 TX2 -> A9G RX

#define SOS_BUTTON_PIN  4              // Taster gegen GND
// v7.1: Taster wird abgefragt statt per Interrupt -> GSM-Störungen lösen keinen SOS mehr aus
const uint32_t SOS_HOLD_MS    = 2000;   // SOS: Taster 2 s gedrückt halten
const uint32_t CANCEL_HOLD_MS = 100;    // Sturzalarm abbrechen: kurzer Druck
const uint32_t SOS_LOCKOUT_MS = 60000;  // nach einem Taster-SOS 60 s Sperre

// Optional: aktiver Summer oder LED für den Abbruch-Countdown (-1 = nicht vorhanden)
#define BUZZER_PIN 14

// I2C Pins für MPU6050 (ESP32-S3)
#define I2C_SDA 8
#define I2C_SCL 9

// Sturzerkennung
const float FALL_THRESHOLD = 16; // ca. 1.6g - Aufprall-Schwelle ohne Freifall
bool fallDetected = false;
bool mpuReady     = false;          // true, wenn MPU6050 gefunden wurde

// NEU: Abbruchzeit nach erkanntem Sturz (bevor die SMS gesendet wird)
const uint32_t CANCEL_WINDOW_MS = 15000;   // 15 Sekunden

// NEU: Rückruf / Mithören
const bool LISTEN_IN_ENABLED = true;  // Anrufe von gespeicherten Notrufnummern automatisch annehmen
const bool MUTE_SPEAKER      = true;  // Lautsprecher stumm -> Anrufer hört nur zu

const unsigned long GPS_INTERVAL = 30000;  // 30 Sekunden

// v7.3: AGPS + Ersatz-Standort, solange kein GPS-Fix da ist
const char* GPRS_APN          = "internet";  // APN der SIM-Karte (z. B. "internet", "web.vodafone.de", "online.telia.se")
const bool  USE_AGPS          = true;        // AGPS: schnellerer GPS-Fix (braucht mobile Daten)
const bool  USE_WIFI_LOCATION = true;        // 1. Ersatz: WLAN-Ortung (braucht WLAN mit Internet)
const bool  USE_CELL_LOCATION = true;        // 2. Ersatz: Funkzellen-Ortung über A9G (braucht mobile Daten)
// beaconDB: kostenlos, ohne Key, aber experimentell. Alternative: Google Geolocation API
// "https://www.googleapis.com/geolocation/v1/geolocate?key=DEIN_KEY"
const char* GEO_API_URL       = "https://api.beacondb.net/v1/geolocate";
const uint32_t FALLBACK_INTERVAL_MS = 120000;   // Ersatz-Standort alle 2 min erneuern
const uint32_t FALLBACK_MAX_AGE_MS  = 900000;   // Ersatz-Standort höchstens 15 min alt
const uint32_t FOLLOWUP_MAX_MS      = 7200000;  // GPS-Nachmeldung bis 2 h nach dem Alarm

// v7.2: SMS bleiben in der Warteschlange, bis das A9G wieder bereit ist
const uint32_t SMS_RETRY_INTERVAL_MS = 15000;    // alle 15 s neu versuchen
const uint32_t SMS_MAX_RETRY_MS      = 3600000;  // max. 1 Stunde lang versuchen
const uint32_t SMS_SEND_TIMEOUT_MS   = 15000;    // max. Wartezeit auf +CMGS
// =============================================================

AsyncWebServer server(80);
Preferences prefs;
Adafruit_MPU6050 mpu;

// ---- Synchronisation ----
SemaphoreHandle_t stateMutex;     // schützt: lat/lon/gpsFix/phones/smsStatus/pendingCaller
SemaphoreHandle_t serial2Mutex;   // schützt: UART2 + urcBuffer
QueueHandle_t     smsQueue;       // Warteschlange für SMS-Aufträge

// ---- Geteilter Zustand ----
String wifiSSID;
String wifiPass;
String phoneNumbers[3] = {"", "", ""};
String latitude  = "0.0";
String longitude = "0.0";
bool   gpsFix    = false;
String smsStatusText = "Bereit.";
bool   apMode    = false;
volatile bool restartPending = false;
unsigned long restartTimer   = 0;
unsigned long lastWifiCheck  = 0;

// ---- MPU6050 Zustand ----
float mpuAccX = 0, mpuAccY = 0, mpuAccZ = 0;
float mpuGyrX = 0, mpuGyrY = 0, mpuGyrZ = 0;

// ---- NEU: Abbruch-Countdown ----
volatile bool alarmPending   = false;   // Countdown läuft gerade
volatile bool alarmCancelled = false;   // Benutzer hat abgebrochen

// ---- NEU: Anruf-Zustand ----
volatile bool callActive       = false; // Gespräch läuft (Mithören)
volatile bool callerPending    = false; // neuer eingehender Anruf erkannt
volatile bool hangupRequested  = false; // "Anruf beenden" über Webseite
volatile bool modemOnline      = false; // v7.2: A9G antwortet auf "AT"
// ---- v7.3: Ersatz-Standort + Nachmeldung ----
String   fbLat, fbLon, fbSource;           // letzter Ersatz-Standort (stateMutex)
int      fbAccuracy = 0;
uint32_t fbTime     = 0;
bool     fbValid    = false;
volatile bool     gprsReady           = false;
volatile bool     locRefreshRequested = false;
volatile int      alarmLocQuality     = -1;  // -1 = keine offene Nachmeldung, 0 = kein, 1 = Zelle, 2 = WLAN
volatile uint32_t alarmTime           = 0;
String        pendingCaller;            // Nummer des Anrufers (stateMutex)
String        urcBuffer;                // Zeilenpuffer für Modem-Meldungen (serial2Mutex)

// ---- SMS-Auftrag ----
struct SmsRequest {
  char number[32];
  char message[300];
};

// ======================= HTML =======================
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>A9G Notruf & GPS Tracker</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background-color: #f4f4f9; color: #333; }
    h1 { color: #0056b3; font-size: 1.2em; margin-top: 0; }
    .box { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); margin-bottom: 20px; }
    #location { font-size: 1.2em; font-weight: bold; color: #28a745; }
    .hint { font-size: 0.85em; color: #666; margin-top: 8px; }
    input[type="text"] { padding: 10px; margin: 5px 0; width: 100%; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px; }
    input[type="submit"], button { background-color: #0056b3; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; font-size: 1em; font-weight: bold; width: 100%; margin-top: 10px; }
    input[type="submit"]:hover, button:hover { opacity: 0.9; }
    .sos { background-color: #dc3545 !important; }
    .sos:hover { background-color: #c82333 !important; }
    .grey { background-color: #6c757d !important; }
    .status { white-space: pre-line; font-size: 0.9em; color: #444; background: #f8f9fa; padding: 10px; border-radius: 4px; min-height: 1.2em; margin-top: 12px; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; font-size: 0.9em; }
    .val { font-weight: bold; color: #0056b3; }
  </style>
  <script>
    function updateLocation() {
      fetch('/getLocation').then(r => r.text())
        .then(d => { document.getElementById('location').innerHTML = d; })
        .catch(e => console.error('Fehler:', e));
    }
    function updateStatus() {
      fetch('/getSMSStatus').then(r => r.text())
        .then(d => { document.getElementById('smsstatus').innerText = d; })
        .catch(e => console.error('Fehler:', e));
    }
    function updateMPU() {
      fetch('/getMPU').then(r => r.json())
        .then(d => {
          document.getElementById('accX').innerText = d.ax.toFixed(2);
          document.getElementById('accY').innerText = d.ay.toFixed(2);
          document.getElementById('accZ').innerText = d.az.toFixed(2);
          document.getElementById('gyrX').innerText = d.gx.toFixed(2);
          document.getElementById('gyrY').innerText = d.gy.toFixed(2);
          document.getElementById('gyrZ').innerText = d.gz.toFixed(2);
        })
        .catch(e => console.error('Fehler:', e));
    }
    function sendSOS() {
      if (!confirm('Notruf-SMS an alle gespeicherten Nummern senden?')) return;
      document.getElementById('smsstatus').innerText = 'Sende Notruf...';
      fetch('/sos').then(r => r.text())
        .then(d => { document.getElementById('smsstatus').innerText = d; })
        .catch(e => { document.getElementById('smsstatus').innerText = 'Fehler: ' + e; });
    }
    function cancelAlarm() {
      fetch('/cancel').then(r => r.text())
        .then(d => { document.getElementById('smsstatus').innerText = d; })
        .catch(e => console.error('Fehler:', e));
    }
    function hangup() {
      fetch('/hangup').then(r => r.text())
        .then(d => { document.getElementById('smsstatus').innerText = d; })
        .catch(e => console.error('Fehler:', e));
    }
    setInterval(updateLocation, 1000);
    setInterval(updateStatus, 1000);
    setInterval(updateMPU, 1000);
    window.onload = function() { updateLocation(); updateStatus(); updateMPU(); };
  </script>
</head>
<body>

  <div class="box">
    <h1>📍 Aktueller Standort</h1>
    <p id="location">Lade GPS-Daten...</p>
    <p class="hint">__MODE__</p>
  </div>

  <div class="box">
    <h1>🚨 Notruf auslösen</h1>
    <button class="sos" onclick="sendSOS()">SOS – SMS an alle gespeicherten Nummern</button>
    <button class="grey" onclick="cancelAlarm()">⛔ Sturzalarm abbrechen</button>
    <p class="hint">Physischer SOS-Taster an GPIO 4 gegen GND: <b>2 Sekunden gedrückt halten</b>.<br>
       Nach einem erkannten Sturz bleiben __CANCEL__ s Zeit: Taster drücken oder „Sturzalarm abbrechen“ = kein Notruf.</p>
    <p id="smsstatus" class="status">Bereit.</p>
  </div>

  <div class="box">
    <h1>🎧 Rückruf / Mithören</h1>
    <p class="hint">Rufen Sie von einer gespeicherten Notrufnummer die SIM-Nummer des Geräts an.
       Der Anruf wird automatisch angenommen und Sie hören über das Mikrofon, was um die Person herum passiert.
       Anrufe von anderen Nummern werden abgelehnt.</p>
    <button class="grey" onclick="hangup()">📴 Laufenden Anruf beenden</button>
  </div>

  <div class="box">
    <h1>📊 MPU6050 Sensor (Sturzerkennung)</h1>
    <div class="grid">
      <div>Acc X: <span class="val" id="accX">0.0</span> m/s²</div>
      <div>Gyr X: <span class="val" id="gyrX">0.0</span> rad/s</div>
      <div>Acc Y: <span class="val" id="accY">0.0</span> m/s²</div>
      <div>Gyr Y: <span class="val" id="gyrY">0.0</span> rad/s</div>
      <div>Acc Z: <span class="val" id="accZ">0.0</span> m/s²</div>
      <div>Gyr Z: <span class="val" id="gyrZ">0.0</span> rad/s</div>
    </div>
    <p class="hint">Automatischer SOS bei Aufprall > __THRESHOLD__ m/s² + Lageänderung (liegend/ruhig)</p>
  </div>

  <div class="box">
    <h1>💬 SMS manuell senden</h1>
    <form action="/sendSMS" method="GET">
      Telefonnummer (mit Ländervorwahl):<br>
      <input type="text" name="number" placeholder="+49123456789"><br>
      Nachricht:<br>
      <input type="text" name="message" value="Notfall! Mein Standort: "><br>
      <input type="submit" value="SMS mit Standort senden">
    </form>
  </div>

  <div class="box">
    <h1>📞 Notrufnummern (max. 3)</h1>
    <form action="/savePhones" method="GET">
      Nummer 1:<br>
      <input type="text" name="p1" value="__PHONE1__" placeholder="+49123456789"><br>
      Nummer 2 (optional):<br>
      <input type="text" name="p2" value="__PHONE2__" placeholder="+49123456789"><br>
      Nummer 3 (optional):<br>
      <input type="text" name="p3" value="__PHONE3__" placeholder="+49123456789"><br>
      <input type="submit" value="Nummern speichern">
    </form>
    <p class="hint">Leer lassen = Nummer wird nicht verwendet. Nur diese Nummern dürfen mithören.</p>
  </div>

  <div class="box">
    <h1>📶 WLAN-Einstellungen</h1>
    <form action="/saveWifi" method="GET">
      WLAN-Name (SSID):<br>
      <input type="text" name="ssid" value="__SSID__"><br>
      WLAN-Passwort:<br>
      <input type="text" name="pass" value="__PASS__"><br>
      <input type="submit" value="Speichern & Neustart">
    </form>
  </div>

</body>
</html>
)rawliteral";

// ======================= HELFER =======================

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

String buildPage() {
  String page = FPSTR(htmlPage);

  String modeInfo;
  if (apMode) {
    modeInfo = "⚠ Konfigurationsmodus aktiv. Verbinde dich mit dem WLAN \"" +
               String(AP_SSID) + "\" (Passwort: " + String(AP_PASS) +
               ") und öffne http://192.168.4.1";
  } else {
    modeInfo = "WLAN verbunden. IP-Adresse: " + WiFi.localIP().toString();
  }

  String sSSID, sPass, sPh[3];
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    sSSID = wifiSSID; sPass = wifiPass;
    for (int i = 0; i < 3; i++) sPh[i] = phoneNumbers[i];
    xSemaphoreGive(stateMutex);
  }

  page.replace("__MODE__",   modeInfo);
  page.replace("__SSID__",   htmlEscape(sSSID));
  page.replace("__PASS__",   htmlEscape(sPass));
  page.replace("__PHONE1__", htmlEscape(sPh[0]));
  page.replace("__PHONE2__", htmlEscape(sPh[1]));
  page.replace("__PHONE3__", htmlEscape(sPh[2]));
  page.replace("__AP__",     String(AP_SSID));
  page.replace("__THRESHOLD__", String(FALL_THRESHOLD, 1));
  page.replace("__CANCEL__", String(CANCEL_WINDOW_MS / 1000));

  return page;
}

String resultPage(const String& title, const String& text) {
  String p = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  p += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  p += "<meta http-equiv='refresh' content='3; url=/'>";
  p += "<style>body{font-family:Arial,sans-serif;margin:20px;background:#f4f4f9;color:#333;}";
  p += ".box{background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}";
  p += "a{color:#0056b3;}</style></head><body><div class='box'>";
  p += "<h2>" + title + "</h2><p>" + text + "</p>";
  p += "<p><a href='/'>Zurück zur Startseite</a></p></div></body></html>";
  return p;
}

void loadConfig() {
  prefs.begin("sosconfig", false);
  wifiSSID = prefs.getString("ssid", DEFAULT_SSID);
  wifiPass = prefs.getString("pass", DEFAULT_PASS);
  phoneNumbers[0] = prefs.getString("phone1", "");
  phoneNumbers[1] = prefs.getString("phone2", "");
  phoneNumbers[2] = prefs.getString("phone3", "");

  Serial.println("--- Gespeicherte Konfiguration ---");
  Serial.println("SSID: " + wifiSSID);
  for (int i = 0; i < 3; i++) {
    Serial.println("Nummer " + String(i + 1) + ": " +
                   (phoneNumbers[i].length() ? phoneNumbers[i] : String("(leer)")));
  }
  Serial.println("----------------------------------");
}

String getSmsStatus() {
  String s;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    s = smsStatusText;
    xSemaphoreGive(stateMutex);
  }
  return s;
}
void setSmsStatus(const String& s) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    smsStatusText = s;
    xSemaphoreGive(stateMutex);
  }
}
void appendSmsStatus(const String& s) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    smsStatusText += s;
    xSemaphoreGive(stateMutex);
  }
}

// ======================= A9G: MODEM-MELDUNGEN (URC) =======================
// Das A9G meldet eingehende Anrufe unaufgefordert ("RING", "+CLIP: ...").
// Alles, was vom UART gelesen wird, läuft deshalb durch feedModem().
// WICHTIG: nur aufrufen, während serial2Mutex gehalten wird.

void handleModemLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line.startsWith("+CLIP:")) {                 // +CLIP: "+49176...",145,...
    int q1 = line.indexOf('"');
    int q2 = line.indexOf('"', q1 + 1);
    if (q1 != -1 && q2 > q1) {
      String num = line.substring(q1 + 1, q2);
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        pendingCaller = num;
        xSemaphoreGive(stateMutex);
      }
      callerPending = true;
    }
  } else if (line == "NO CARRIER" || line == "BUSY" || line == "NO ANSWER") {
    if (callActive) {
      callActive = false;
      Serial.println("Anruf beendet.");
      appendSmsStatus("\nAnruf beendet.");
    }
  }
}

void feedModem(const String& data) {
  for (size_t i = 0; i < data.length(); i++) {
    char c = data.charAt(i);
    if (c == '\n') {
      handleModemLine(urcBuffer);
      urcBuffer = "";
    } else if (c != '\r') {
      urcBuffer += c;
      if (urcBuffer.length() > 200) urcBuffer = "";   // Schutz vor Müll
    }
  }
}

// Liest alles Vorhandene vom UART und wertet es aus (statt es wegzuwerfen)
void drainModem() {
  String s;
  while (Serial2.available()) s += (char)Serial2.read();
  if (s.length()) feedModem(s);
}

// ======================= A9G / AT =======================

String sendATCommand(String command, unsigned long timeout) {
  String response = "";
  if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(timeout + 1000)) != pdTRUE) {
    return response;
  }
  drainModem();                       // alte Meldungen auswerten (z. B. RING)
  Serial2.println(command);
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (Serial2.available()) response += (char)Serial2.read();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  feedModem(response + "\n");         // auch Antworten auf Anruf-Meldungen prüfen
  response.trim();
  xSemaphoreGive(serial2Mutex);
  return response;
}

void parseGPSResponse(const String& response) {
  String newLat, newLon;
  bool   newFix = false;

  int index = response.indexOf("+LOCATION:");
  if (index != -1) {
    String data = response.substring(index + 10);
    data.trim();
    int commaIndex = data.indexOf(',');
    if (commaIndex != -1) {
      newLat = data.substring(0, commaIndex);
      newLon = data.substring(commaIndex + 1);
      newLon.trim();
      newFix = true;
    }
  }

  if (!newFix && response.indexOf(',') != -1 && response.indexOf('.') != -1
      && response.indexOf("GPS NOT FIX") == -1
      && response.indexOf("ERROR") == -1) {
    int commaIndex = response.indexOf(',');
    int dotBefore = response.lastIndexOf('.', commaIndex);
    int startIndex = response.lastIndexOf('\n', dotBefore) + 1;
    while (startIndex < commaIndex && !isDigit(response.charAt(startIndex))) startIndex++;
    if (startIndex < commaIndex) {
      newLat = response.substring(startIndex, commaIndex);
      newLon = response.substring(commaIndex + 1);
      newLon.trim();
      int nl = newLon.indexOf('\n'); if (nl != -1) newLon = newLon.substring(0, nl);
      int cr = newLon.indexOf('\r'); if (cr != -1) newLon = newLon.substring(0, cr);
      newLon.trim();
      newFix = true;
    }
  }

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    if (newFix) {
      latitude  = newLat;
      longitude = newLon;
      gpsFix    = true;
    } else if (response.indexOf("GPS NOT FIX") != -1) {
      latitude  = "Warte auf GPS-Fix...";
      longitude = "Bitte ins Freie gehen!";
      gpsFix    = false;
    } else if (response.indexOf("+CME ERROR") != -1) {
      latitude  = "GPS noch nicht bereit...";
      longitude = "Warte auf Satelliten...";
      gpsFix    = false;
    } else {
      latitude  = "Unbekannte Antwort";
      longitude = "Siehe Seriellen Monitor";
      gpsFix    = false;
    }
    xSemaphoreGive(stateMutex);
  }
}

// ======================= SOS =======================

// v7.3: Beste verfügbare Position. Qualität: 3 = GPS, 2 = WLAN, 1 = Funkzelle, 0 = keine
int getBestLocation(String& lat, String& lon, String& src, int& acc) {
  int q = 0;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    if (gpsFix) {
      lat = latitude; lon = longitude; src = "GPS"; acc = 10; q = 3;
    } else if (fbValid && millis() - fbTime < FALLBACK_MAX_AGE_MS) {
      lat = fbLat; lon = fbLon; src = fbSource; acc = fbAccuracy;
      q = (fbSource == "WLAN") ? 2 : 1;
    }
    xSemaphoreGive(stateMutex);
  }
  return q;
}

// Standorttext für SMS (nur ASCII, damit die SMS sicher ankommt)
String buildLocationText(int q, const String& lat, const String& lon, const String& src, int acc) {
  if (q == 0) return " (Kein Standort verfuegbar)";
  String link = "https://maps.google.com/?q=" + lat + "," + lon;
  if (q == 3) return " Standort (GPS): " + link;
  return " Ca.-Standort (" + src + ", ~" + String(acc) + " m): " + link;
}

// Nachricht an alle gespeicherten Nummern in die Warteschlange
int enqueueToAll(const String& message) {
  String nums[3];
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++) nums[i] = phoneNumbers[i];
    xSemaphoreGive(stateMutex);
  }
  int queued = 0;
  for (int i = 0; i < 3; i++) {
    if (nums[i].length() == 0) continue;
    SmsRequest req;
    memset(&req, 0, sizeof(req));
    strncpy(req.number,  nums[i].c_str(), sizeof(req.number)  - 1);
    strncpy(req.message, message.c_str(), sizeof(req.message) - 1);
    if (xQueueSend(smsQueue, &req, 0) == pdTRUE) queued++;
  }
  return queued;
}

int enqueueSOSForAllNumbers(const String& baseMessage) {
  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);
  int queued = enqueueToAll(baseMessage + buildLocationText(q, lat, lon, src, acc));
  if (queued > 0 && q < 3) {
    // Kein GPS beim Alarm -> genaueren Standort automatisch nachsenden
    alarmLocQuality     = q;
    alarmTime           = millis();
    locRefreshRequested = true;   // sofort neuen Ersatz-Standort holen
  }
  return queued;
}

// Sendet den genaueren Standort nach, sobald er da ist (GPS sofort,
// einen Ersatz-Standort nur, wenn der Alarm gar keinen Standort hatte)
void checkFollowUp() {
  if (alarmLocQuality < 0) return;
  if (millis() - alarmTime > FOLLOWUP_MAX_MS) {
    alarmLocQuality = -1;
    Serial.println("Nachmeldung beendet: kein GPS innerhalb der Frist.");
    return;
  }
  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);
  bool better = (q == 3) || (alarmLocQuality == 0 && q > 0);
  if (!better) return;

  int queued = enqueueToAll("UPDATE NOTRUF:" + buildLocationText(q, lat, lon, src, acc));
  if (queued > 0) {
    Serial.println("Nachmeldung gesendet (" + src + ").");
    appendSmsStatus("\n📍 Neuer Standort (" + src + ") nachgesendet.");
    alarmLocQuality = (q == 3) ? -1 : q;    // nach GPS fertig, sonst weiter auf GPS warten
  }
}

// ======================= ERSATZ-ORTUNG =======================

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

// 1) WLAN-Ortung: umliegende WLANs scannen und an Geolocation-API senden
bool wifiLocate(String& lat, String& lon, int& acc) {
  if (!USE_WIFI_LOCATION || apMode || WiFi.status() != WL_CONNECTED) return false;

  int n = WiFi.scanNetworks(false, true);
  if (n < 2) { WiFi.scanDelete(); return false; }

  String body = "{\"considerIp\":false,\"wifiAccessPoints\":[";
  int used = 0;
  for (int i = 0; i < n && used < 12; i++) {
    if (WiFi.SSID(i).endsWith("_nomap")) continue;     // Opt-out respektieren
    if (used) body += ",";
    body += "{\"macAddress\":\"" + WiFi.BSSIDstr(i) + "\",\"signalStrength\":" + String(WiFi.RSSI(i)) + "}";
    used++;
  }
  body += "]}";
  WiFi.scanDelete();
  if (used < 2) return false;

  WiFiClientSecure client;
  client.setInsecure();                 // Prototyp: Zertifikat wird nicht geprüft
  HTTPClient http;
  if (!http.begin(client, GEO_API_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  http.setUserAgent("ESP32-SOS-Device/7.3");
  http.setTimeout(8000);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code != 200) {
    Serial.printf("WLAN-Ortung fehlgeschlagen: HTTP %d %s\n", code, resp.c_str());
    return false;
  }
  String a;
  if (!jsonNumber(resp, "lat", lat) || !jsonNumber(resp, "lng", lon)) return false;
  acc = jsonNumber(resp, "accuracy", a) ? (int)a.toFloat() : 100;
  return true;
}

// Mobile Daten (GPRS) aktivieren - nötig für AGPS und Funkzellen-Ortung
bool ensureGprs() {
  if (gprsReady) return true;
  sendATCommand("AT+CGATT=1", 5000);
  sendATCommand(String("AT+CGDCONT=1,\"IP\",\"") + GPRS_APN + "\"", 1000);
  String r = sendATCommand("AT+CGACT=1,1", 5000);
  gprsReady = r.indexOf("OK") != -1;
  Serial.println(gprsReady ? "GPRS aktiv." : "GPRS fehlgeschlagen: " + r);
  return gprsReady;
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

// 2) Funkzellen-Ortung über das A9G (AT+LOCATION=1)
bool cellLocate(String& lat, String& lon) {
  if (!ensureGprs()) return false;
  String r = sendATCommand("AT+LOCATION=1", 6000);
  Serial.println("A9G Funkzellen-Antwort: " + r);
  if (extractLatLon(r, lat, lon)) return true;
  gprsReady = false;                    // beim nächsten Mal neu verbinden
  return false;
}

// Ersatz-Standort holen: zuerst WLAN, dann Funkzelle
void updateFallbackLocation(bool modemAlive) {
  String lat, lon, src; int acc = 0;
  bool ok = false;
  if (wifiLocate(lat, lon, acc)) { src = "WLAN"; ok = true; }
  else if (USE_CELL_LOCATION && modemAlive && cellLocate(lat, lon)) { src = "Funkzelle"; acc = 1000; ok = true; }

  if (ok) {
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
      fbLat = lat; fbLon = lon; fbSource = src; fbAccuracy = acc;
      fbTime = millis(); fbValid = true;
      xSemaphoreGive(stateMutex);
    }
    Serial.println("Ersatz-Standort (" + src + ", ~" + String(acc) + " m): " + lat + "," + lon);
  } else {
    Serial.println("Kein Ersatz-Standort verfuegbar.");
  }
}

// ======================= RÜCKRUF / MITHÖREN =======================

String digitsOnly(const String& s) {
  String d;
  for (size_t i = 0; i < s.length(); i++) if (isDigit(s.charAt(i))) d += s.charAt(i);
  return d;
}

// Vergleicht die letzten 9 Ziffern -> "+4917612345678" == "017612345678"
bool isAuthorizedCaller(const String& caller) {
  String c = digitsOnly(caller);
  if (c.length() < 6) return false;              // unterdrückte/ungültige Nummer
  String cTail = c.substring(c.length() > 9 ? c.length() - 9 : 0);

  bool ok = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    for (int i = 0; i < 3; i++) {
      String p = digitsOnly(phoneNumbers[i]);
      if (p.length() < 6) continue;
      String pTail = p.substring(p.length() > 9 ? p.length() - 9 : 0);
      if (pTail == cTail) { ok = true; break; }
    }
    xSemaphoreGive(stateMutex);
  }
  return ok;
}

void callTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(4000));
  for (;;) {
    // 1) Unaufgeforderte Meldungen lesen, wenn der UART frei ist
    if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      drainModem();
      xSemaphoreGive(serial2Mutex);
    }

    // 2) Eingehender Anruf
    if (callerPending) {
      callerPending = false;
      String caller;
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        caller = pendingCaller;
        xSemaphoreGive(stateMutex);
      }

      if (!callActive) {
        if (LISTEN_IN_ENABLED && isAuthorizedCaller(caller)) {
          Serial.println("Anruf von Notrufnummer " + caller + " -> annehmen (Mithoeren)");
          String r = sendATCommand("ATA", 3000);
          Serial.println("ATA Antwort: " + r);
          if (r.indexOf("ERROR") == -1 && r.indexOf("NO CARRIER") == -1) {
            callActive = true;
            appendSmsStatus("\n🎧 Anruf von " + caller + " angenommen (Mithören aktiv).");
          }
        } else {
          Serial.println("Anruf von " + caller + " abgelehnt (keine Notrufnummer).");
          sendATCommand("ATH", 2000);
        }
      }
    }

    // 3) Auflegen über Webseite
    if (hangupRequested) {
      hangupRequested = false;
      sendATCommand("ATH", 2000);
      if (callActive) {
        callActive = false;
        appendSmsStatus("\nAnruf über Webseite beendet.");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ======================= TASKS =======================

// v7.2: Modem (neu) einrichten - auch nach Stromausfall/Neustart des A9G
void modemInit() {
  sendATCommand("AT+CMGF=1", 1000);                    // SMS-Textmodus
  sendATCommand("AT+CLIP=1", 1000);                    // Anrufernummer anzeigen (+CLIP)
  if (MUTE_SPEAKER) sendATCommand("AT+CLVL=0", 1000);  // Lautsprecher leise (falls unterstützt)

  // v7.3: AGPS (braucht mobile Daten). Laut Handbuch vorher GPS ausschalten.
  bool agpsOn = false;
  if (USE_AGPS && ensureGprs()) {
    sendATCommand("AT+GPS=0", 1000);
    String r = sendATCommand("AT+AGPS=1", 5000);
    agpsOn = r.indexOf("OK") != -1;
    Serial.println(agpsOn ? "AGPS aktiv." : "AGPS nicht moeglich: " + r);
  }
  if (!agpsOn) sendATCommand("AT+GPS=1", 2000);        // normales GPS
}

void gpsTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(3000));   // let WiFi settle first
  uint32_t lastFallback = 0;
  bool     fallbackTried = false;

  for (;;) {
    // A9G erreichbar?
    bool alive = sendATCommand("AT", 500).indexOf("OK") != -1;
    if (alive && !modemOnline) {
      Serial.println("A9G erreichbar - wird eingerichtet.");
      gprsReady = false;
      modemInit();
      modemOnline = true;
    } else if (!alive && modemOnline) {
      Serial.println("A9G nicht erreichbar!");
      modemOnline = false;
      gprsReady   = false;
    }

    // 1) GPS
    if (alive) {
      String resp = sendATCommand("AT+LOCATION=2", 2000);
      Serial.print("A9G GPS Antwort: ");
      Serial.println(resp);
      parseGPSResponse(resp);
    } else if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      latitude  = "A9G nicht erreichbar...";
      longitude = "Stromversorgung/Kabel prüfen";
      gpsFix    = false;
      xSemaphoreGive(stateMutex);
    }

    // 2) Ohne GPS-Fix: Ersatz-Standort (WLAN, sonst Funkzelle)
    bool fix = false;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) { fix = gpsFix; xSemaphoreGive(stateMutex); }
    if (fix) {
      locRefreshRequested = false;
    } else if (!fallbackTried || locRefreshRequested ||
               millis() - lastFallback > FALLBACK_INTERVAL_MS) {
      locRefreshRequested = false;
      fallbackTried = true;
      lastFallback  = millis();
      updateFallbackLocation(alive);
    }

    // 3) Genaueren Standort nach einem Alarm nachsenden
    checkFollowUp();

    // Warten: nach einem Alarm öfter prüfen; bei Alarm sofort weiter
    uint32_t waitMs = !alive ? 5000 : (alarmLocQuality >= 0 ? 10000 : GPS_INTERVAL);
    for (uint32_t w = 0; w < waitMs && !locRefreshRequested; w += 500) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
}

// v7.2: Ist das A9G bereit zum Senden? (antwortet + im Netz angemeldet)
bool modemReadyForSms() {
  if (sendATCommand("AT", 500).indexOf("OK") == -1) return false;
  String r = sendATCommand("AT+CREG?", 800);           // +CREG: x,1 oder x,5 = angemeldet
  return r.indexOf(",1") != -1 || r.indexOf(",5") != -1;
}

// v7.2: Ein Sendeversuch. true = erfolgreich, sonst Fehlertext in err
bool trySendSms(const SmsRequest& req, String& err) {
  if (!modemReadyForSms()) { err = "A9G nicht bereit oder kein Netz"; return false; }

  if (xSemaphoreTake(serial2Mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
    err = "UART belegt";
    return false;
  }

  drainModem();
  Serial2.println("AT+CMGF=1");
  vTaskDelay(pdMS_TO_TICKS(500));
  drainModem();

  Serial2.print("AT+CMGS=\"");
  Serial2.print(req.number);
  Serial2.println("\"");

  unsigned long start = millis();
  bool promptSeen = false;
  while (millis() - start < 5000) {
    if (Serial2.available()) {
      char c = (char)Serial2.read();
      if (c == '>') { promptSeen = true; break; }
      feedModem(String(c));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (!promptSeen) {
    Serial2.write(0x1B);                 // ESC: evtl. offene Eingabe abbrechen
    xSemaphoreGive(serial2Mutex);
    err = "keine Bereitschaft ('>')";
    return false;
  }

  Serial2.print(req.message);
  Serial2.write(0x1A);

  // Auf Bestätigung (+CMGS) oder Fehler warten
  String confirm = "";
  start = millis();
  while (millis() - start < SMS_SEND_TIMEOUT_MS) {
    while (Serial2.available()) confirm += (char)Serial2.read();
    if (confirm.indexOf("+CMGS") != -1 || confirm.indexOf("ERROR") != -1) {
      vTaskDelay(pdMS_TO_TICKS(100));
      while (Serial2.available()) confirm += (char)Serial2.read();
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  feedModem(confirm + "\n");
  confirm.trim();
  Serial.println("SMS-Antwort: " + confirm);
  xSemaphoreGive(serial2Mutex);

  if (confirm.indexOf("+CMGS") != -1) return true;
  err = confirm.indexOf("ERROR") != -1 ? "Netz meldet Fehler" : "keine Bestätigung vom A9G";
  return false;
}

// v7.2: SMS werden wiederholt, bis sie gesendet sind (max. SMS_MAX_RETRY_MS)
void smsTask(void* pv) {
  SmsRequest req;
  for (;;) {
    if (xQueueReceive(smsQueue, &req, portMAX_DELAY) != pdTRUE) continue;

    uint32_t firstTry = millis();
    int attempt = 0;
    for (;;) {
      attempt++;
      String err;
      if (trySendSms(req, err)) {
        appendSmsStatus("\nSMS gesendet an: " + String(req.number) +
                        (attempt > 1 ? " (Versuch " + String(attempt) + ")" : ""));
        break;
      }

      Serial.printf("SMS an %s fehlgeschlagen (%s), Versuch %d\n",
                    req.number, err.c_str(), attempt);

      if (millis() - firstTry > SMS_MAX_RETRY_MS) {
        appendSmsStatus("\nAufgegeben nach " + String(attempt) + " Versuchen: " + String(req.number));
        break;
      }
      if (attempt == 1) {
        appendSmsStatus("\nWarte auf A9G: " + String(req.number) + " (" + err +
                        ") – neuer Versuch alle " + String(SMS_RETRY_INTERVAL_MS / 1000) + " s.");
      }
      vTaskDelay(pdMS_TO_TICKS(SMS_RETRY_INTERVAL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// SOS per Taster senden
static void sendButtonSOS() {
  Serial.println(">>> SOS-TASTER (2 s gehalten) <<<");

  bool anyNumber = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++)
      if (phoneNumbers[i].length() > 0) anyNumber = true;
    xSemaphoreGive(stateMutex);
  }

  if (!anyNumber) {
    Serial.println("SOS abgebrochen: Keine Notrufnummer gespeichert!");
    setSmsStatus("SOS-Taster gedrückt, aber keine Nummer gespeichert!");
    return;
  }
  int queued = enqueueSOSForAllNumbers("NOTRUF! Ich brauche Hilfe.");
  if (queued > 0) {
    setSmsStatus("SOS-Taster: Notruf an " + String(queued) + " Nummer(n) gesendet.");
    Serial.println("SOS-SMS an " + String(queued) + " Nummer(n) in Warteschlange.");
  } else {
    setSmsStatus("SOS-Taster: SMS-Warteschlange voll!");
  }
}

// v7.1: Taster wird alle 20 ms abgefragt.
// Ein SOS zählt nur, wenn der Taster 2 s DURCHGEHEND gedrückt ist.
// Kurze Störimpulse (z. B. beim Senden des A9G) werden ignoriert.
void buttonTask(void* pv) {
  uint32_t pressStart = 0;
  bool     handled    = false;   // pro Tastendruck nur eine Aktion
  uint32_t lastSos    = 0;
  bool     sosSent    = false;

  for (;;) {
    bool pressed = (digitalRead(SOS_BUTTON_PIN) == LOW);
    uint32_t now = millis();

    if (pressed) {
      if (pressStart == 0) pressStart = now;
      uint32_t held = now - pressStart;

      if (!handled) {
        if (alarmPending && held >= CANCEL_HOLD_MS) {
          // Während des Countdowns: kurzer Druck bricht den Sturzalarm ab
          alarmCancelled = true;
          handled = true;
          Serial.println(">>> Sturzalarm per Taster ABGEBROCHEN <<<");
        } else if (!alarmPending && held >= SOS_HOLD_MS) {
          handled = true;
          if (sosSent && now - lastSos < SOS_LOCKOUT_MS) {
            Serial.println("SOS-Taster ignoriert: Sperrzeit nach letztem SOS.");
          } else {
            lastSos = now;
            sosSent = true;
            sendButtonSOS();
          }
        }
      }
    } else {
      pressStart = 0;            // losgelassen -> nächster Druck möglich
      handled    = false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ======================= STURZERKENNUNG =======================

// ---- Parameter Sturzerkennung ----
const float    G_EARTH                 = 9.81f;
const uint32_t SAMPLE_MS               = 20;      // 50 Hz (vorher 10 Hz)
const float    FREEFALL_THRESHOLD      = 0.5f * G_EARTH; // < 0,5 g = Freifall
const uint32_t FREEFALL_MIN_MS         = 60;      // Mindestdauer Freifall
const uint32_t FF_TO_IMPACT_WINDOW_MS  = 800;     // Aufprall muss danach kommen
const float    IMPACT_THRESHOLD        = FALL_THRESHOLD; // 25 m/s² ohne Freifall
const float    IMPACT_THRESHOLD_AFTER_FF = 20.0f;  // niedriger, wenn Freifall gesehen
const uint32_t SETTLE_MS               = 1500;    // Ausrollen nach Aufprall
const float    ORIENTATION_CHANGE_DEG  = 45.0f;   // Lageänderung vorher/nachher
const float    STILL_ACC_DEV           = 1.5f;    // m/s² Abweichung von 1 g
const float    STILL_GYRO              = 0.5f;    // rad/s

// Werte für das Webinterface speichern
static void storeMpuValues(const sensors_event_t& a, const sensors_event_t& g) {
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    mpuAccX = a.acceleration.x; mpuAccY = a.acceleration.y; mpuAccZ = a.acceleration.z;
    mpuGyrX = g.gyro.x;         mpuGyrY = g.gyro.y;         mpuGyrZ = g.gyro.z;
    xSemaphoreGive(stateMutex);
  }
}

// Winkel zwischen zwei Schwerkraft-Vektoren (unabhängig von der Einbaulage)
static float angleBetweenDeg(float ax, float ay, float az, float bx, float by, float bz) {
  float n = sqrtf(ax * ax + ay * ay + az * az) * sqrtf(bx * bx + by * by + bz * bz);
  if (n < 0.001f) return 0.0f;
  float c = (ax * bx + ay * by + az * bz) / n;
  c = constrain(c, -1.0f, 1.0f);
  return acosf(c) * 180.0f / PI;
}

// Misst ~1,5 s: mittlere Lage + ob die Person ruhig liegt
static bool measureRest(float& gx, float& gy, float& gz) {
  const int N = 75;
  int stillCount = 0;
  gx = gy = gz = 0.0f;
  for (int i = 0; i < N; i++) {
    sensors_event_t a, g, t;
    mpu.getEvent(&a, &g, &t);
    storeMpuValues(a, g);
    gx += a.acceleration.x; gy += a.acceleration.y; gz += a.acceleration.z;
    float tot = sqrtf(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z);
    float gyr = sqrtf(g.gyro.x * g.gyro.x + g.gyro.y * g.gyro.y + g.gyro.z * g.gyro.z);
    if (fabsf(tot - G_EARTH) < STILL_ACC_DEV && gyr < STILL_GYRO) stillCount++;
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
  }
  gx /= N; gy /= N; gz /= N;
  return stillCount > (N * 8) / 10;   // 80 % der Zeit ruhig
}

// NEU: Countdown vor dem Alarm. Rückgabe true = vom Benutzer abgebrochen
static bool runCancelCountdown() {
  alarmCancelled = false;
  alarmPending   = true;

  uint32_t start = millis();
  int lastShown = -1;
  while (millis() - start < CANCEL_WINDOW_MS && !alarmCancelled) {
    uint32_t elapsed = millis() - start;
    int remain = (CANCEL_WINDOW_MS - elapsed + 999) / 1000;
    if (remain != lastShown) {
      lastShown = remain;
      setSmsStatus("⚠ STURZ ERKANNT! Notruf in " + String(remain) +
                   " s.\nZum Abbrechen SOS-Taster drücken oder „Sturzalarm abbrechen“ klicken.");
      Serial.printf("Sturzalarm in %d s - Taster druecken zum Abbrechen\n", remain);
    }
    if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, ((elapsed / 250) % 2) ? HIGH : LOW); // piepen/blinken
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, LOW);
  alarmPending = false;
  return alarmCancelled;
}

// Alarm auslösen. Rückgabe true = SMS wurden gesendet
static bool triggerFallAlarm() {
  fallDetected = true;
  Serial.println("Sturz erkannt - starte Abbruch-Countdown.");

  bool anyNumber = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++)
      if (phoneNumbers[i].length() > 0) anyNumber = true;
    xSemaphoreGive(stateMutex);
  }
  if (!anyNumber) {
    setSmsStatus("STURZ ERKANNT, aber keine Nummer gespeichert!");
    return false;
  }

  // NEU: 15 s Zeit zum Abbrechen
  if (runCancelCountdown()) {
    setSmsStatus("Sturzalarm abgebrochen – kein Notruf gesendet.");
    Serial.println("Sturzalarm abgebrochen.");
    return false;
  }

  Serial.println("SOS: Sturz bestaetigt - sende Notruf!");
  int queued = enqueueSOSForAllNumbers("STURZ ERKANNT! Ich brauche Hilfe.");
  if (queued > 0) {
    setSmsStatus("STURZ ERKANNT: Notruf an " + String(queued) + " Nummer(n) gesendet.");
    return true;
  }
  setSmsStatus("STURZ ERKANNT: SMS-Warteschlange voll!");
  return false;
}

// ---- MPU6050 Task (Sturzerkennung) ----
void mpuTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(2000)); // Warten bis I2C stabil ist

  if (!mpuReady) {                  // ohne Sensor keine Sturzerkennung
    Serial.println("mpuTask beendet: MPU6050 nicht verfuegbar.");
    vTaskDelete(NULL);
  }

  float refX = 0, refY = 0, refZ = G_EARTH; // Referenzlage (vor dem Sturz)
  bool  refInit = false;
  bool  inFreefall = false;
  uint32_t ffStart = 0, ffEnd = 0;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    storeMpuValues(a, g);

    float ax = a.acceleration.x, ay = a.acceleration.y, az = a.acceleration.z;
    float totalAcc = sqrtf(ax * ax + ay * ay + az * az);
    uint32_t now = millis();

    // 1) Referenzlage nur in ruhigen Phasen (~1 g) langsam nachführen
    if (fabsf(totalAcc - G_EARTH) < 1.0f) {
      if (!refInit) { refX = ax; refY = ay; refZ = az; refInit = true; }
      else {
        refX = 0.95f * refX + 0.05f * ax;
        refY = 0.95f * refY + 0.05f * ay;
        refZ = 0.95f * refZ + 0.05f * az;
      }
    }

    // 2) Freifall-Phase erkennen
    if (totalAcc < FREEFALL_THRESHOLD) {
      if (!inFreefall) { inFreefall = true; ffStart = now; }
    } else if (inFreefall) {
      inFreefall = false;
      if (now - ffStart >= FREEFALL_MIN_MS) ffEnd = now;
    }
    bool recentFreefall = (ffEnd != 0) && (now - ffEnd < FF_TO_IMPACT_WINDOW_MS);

    // 3) Aufprall
    float impactThr = recentFreefall ? IMPACT_THRESHOLD_AFTER_FF : IMPACT_THRESHOLD;
    if (totalAcc > impactThr && refInit && !fallDetected) {
      float r0x = refX, r0y = refY, r0z = refZ;   // Lage VOR dem Sturz einfrieren
      Serial.printf("Aufprall %.1f m/s^2 (Freifall: %s) - pruefe Lage...\n",
                    totalAcc, recentFreefall ? "ja" : "nein");

      vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));        // Ausrollen lassen

      // 4) Lage danach + Ruhe prüfen
      float px, py, pz;
      bool still = measureRest(px, py, pz);
      float change = angleBetweenDeg(r0x, r0y, r0z, px, py, pz);
      Serial.printf("Lageaenderung %.0f Grad, ruhig: %s\n", change, still ? "ja" : "nein");

      if (change > ORIENTATION_CHANGE_DEG && (still || recentFreefall)) {
        bool sent = triggerFallAlarm();                    // inkl. 15 s Abbruch-Countdown
        vTaskDelay(pdMS_TO_TICKS(sent ? 15000 : 3000));    // Cooldown
        fallDetected = false;
      } else {
        Serial.println("Entwarnung: kein Sturz.");
      }

      ffEnd = 0;
      lastWake = xTaskGetTickCount();              // Takt nach langer Pause neu setzen
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

// ======================= HTTP-HANDLER =======================

void handleRoot(AsyncWebServerRequest* request) {
  request->send(200, "text/html; charset=utf-8", buildPage());
}

void handleGetLocation(AsyncWebServerRequest* request) {
  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);

  String gpsState;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    gpsState = latitude + " " + longitude;
    xSemaphoreGive(stateMutex);
  }

  String text;
  if (q == 3) {
    text = "GPS: Breitengrad: " + lat + " | Längengrad: " + lon;
  } else if (q > 0) {
    text = "≈ " + src + " (ca. " + String(acc) + " m): " + lat + ", " + lon +
           "<br><small>GPS: " + gpsState + "</small>";
  } else {
    text = gpsState;
  }
  request->send(200, "text/plain; charset=utf-8", text);
}

// JSON für MPU-Daten
void handleGetMPU(AsyncWebServerRequest* request) {
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    ax = mpuAccX; ay = mpuAccY; az = mpuAccZ;
    gx = mpuGyrX; gy = mpuGyrY; gz = mpuGyrZ;
    xSemaphoreGive(stateMutex);
  }

  String json = "{";
  json += "\"ax\":" + String(ax, 2) + ",";
  json += "\"ay\":" + String(ay, 2) + ",";
  json += "\"az\":" + String(az, 2) + ",";
  json += "\"gx\":" + String(gx, 2) + ",";
  json += "\"gy\":" + String(gy, 2) + ",";
  json += "\"gz\":" + String(gz, 2);
  json += "}";

  request->send(200, "application/json", json);
}

void handleGetSMSStatus(AsyncWebServerRequest* request) {
  request->send(200, "text/plain; charset=utf-8", getSmsStatus());
}

// NEU: Sturzalarm über Webseite abbrechen
void handleCancel(AsyncWebServerRequest* request) {
  if (alarmPending) {
    alarmCancelled = true;
    request->send(200, "text/plain; charset=utf-8", "Sturzalarm abgebrochen – kein Notruf gesendet.");
  } else {
    request->send(200, "text/plain; charset=utf-8", "Kein Sturzalarm aktiv.");
  }
}

// NEU: Laufenden Anruf beenden (wird im callTask ausgeführt)
void handleHangup(AsyncWebServerRequest* request) {
  hangupRequested = true;
  request->send(200, "text/plain; charset=utf-8",
                callActive ? "Anruf wird beendet..." : "Kein Anruf aktiv.");
}

void handleSavePhones(AsyncWebServerRequest* request) {
  String p1 = request->hasParam("p1") ? request->getParam("p1")->value() : "";
  String p2 = request->hasParam("p2") ? request->getParam("p2")->value() : "";
  String p3 = request->hasParam("p3") ? request->getParam("p3")->value() : "";
  p1.trim(); p2.trim(); p3.trim();

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    phoneNumbers[0] = p1;
    phoneNumbers[1] = p2;
    phoneNumbers[2] = p3;
    xSemaphoreGive(stateMutex);
  }

  prefs.putString("phone1", p1);
  prefs.putString("phone2", p2);
  prefs.putString("phone3", p3);

  request->send(200, "text/html; charset=utf-8",
                resultPage("✅ Gespeichert", "Die Notrufnummern wurden gespeichert."));
}

void handleSaveWifi(AsyncWebServerRequest* request) {
  String newSSID = request->hasParam("ssid") ? request->getParam("ssid")->value() : "";
  String newPass = request->hasParam("pass") ? request->getParam("pass")->value() : "";
  newSSID.trim();

  if (newSSID.length() == 0) {
    request->send(400, "text/html; charset=utf-8",
                  resultPage("❌ Fehler", "Der WLAN-Name (SSID) darf nicht leer sein."));
    return;
  }

  prefs.putString("ssid", newSSID);
  prefs.putString("pass", newPass);

  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    wifiSSID = newSSID;
    wifiPass = newPass;
    xSemaphoreGive(stateMutex);
  }

  request->send(200, "text/html; charset=utf-8",
                resultPage("✅ Gespeichert",
                  "Die WLAN-Daten wurden gespeichert.<br>Der ESP32 startet jetzt neu und verbindet sich mit <b>" +
                  htmlEscape(newSSID) + "</b>.<br><br>Falls die Verbindung fehlschlägt, öffnet sich der Access Point <b>" +
                  String(AP_SSID) + "</b> mit dem Passwort <b>" + String(AP_PASS) + "</b>."));

  restartPending = true;
  restartTimer = millis();
}

void handleSendSMS(AsyncWebServerRequest* request) {
  String number  = request->hasParam("number")  ? request->getParam("number")->value()  : "";
  String message = request->hasParam("message") ? request->getParam("message")->value() : "";
  number.trim(); message.trim();

  if (number.length() == 0 || message.length() == 0) {
    request->send(400, "text/plain; charset=utf-8", "Fehler: Nummer oder Nachricht fehlt.");
    return;
  }

  String lat, lon, src; int acc = 0;
  int q = getBestLocation(lat, lon, src, acc);
  message += buildLocationText(q, lat, lon, src, acc);

  SmsRequest req;
  memset(&req, 0, sizeof(req));
  strncpy(req.number,  number.c_str(),  sizeof(req.number)  - 1);
  strncpy(req.message, message.c_str(), sizeof(req.message) - 1);

  if (xQueueSend(smsQueue, &req, 0) != pdTRUE) {
    request->send(503, "text/plain; charset=utf-8", "SMS-Warteschlange voll.");
    return;
  }

  request->send(200, "text/plain; charset=utf-8",
                "SMS an " + number + " in Warteschlange.");
}

void handleSOS(AsyncWebServerRequest* request) {
  bool any = false;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
    for (int i = 0; i < 3; i++) if (phoneNumbers[i].length() > 0) any = true;
    xSemaphoreGive(stateMutex);
  }
  if (!any) {
    request->send(400, "text/plain; charset=utf-8", "Fehler: Keine Notrufnummer gespeichert!");
    return;
  }

  int queued = enqueueSOSForAllNumbers("NOTRUF! Ich brauche Hilfe.");
  if (queued == 0) {
    request->send(503, "text/plain; charset=utf-8", "SMS-Warteschlange voll.");
    return;
  }

  setSmsStatus("Notruf wird an " + String(queued) + " Nummer(n) gesendet...");
  request->send(200, "text/plain; charset=utf-8",
                "Notruf-SMS an " + String(queued) + " Nummer(n) in Warteschlange.");
}

// ======================= SETUP =======================

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  delay(500);

  Serial.println();
  Serial.println("ESP32 startet...");

  // Synchronisation
  stateMutex   = xSemaphoreCreateMutex();
  serial2Mutex = xSemaphoreCreateMutex();
  smsQueue     = xQueueCreate(8, sizeof(SmsRequest));

  // SOS-Taster
  pinMode(SOS_BUTTON_PIN, INPUT_PULLUP);   // kein Interrupt mehr (v7.1)

  // Optionaler Summer / LED für den Countdown
  if (BUZZER_PIN >= 0) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // MPU6050 Initialisierung (I2C auf GPIO 8 und 9)
  Wire.begin(I2C_SDA, I2C_SCL);
  if (mpu.begin()) {
    Serial.println("MPU6050 erfolgreich initialisiert.");
    mpuReady = true;
  } else {
    // Klon-Chips (z.B. MPU6500) melden eine andere ID -> WHO_AM_I selbst prüfen
    Wire.beginTransmission(0x68);
    Wire.write(0x75);                        // WHO_AM_I Register
    Wire.endTransmission(false);
    Wire.requestFrom(0x68, 1);
    uint8_t id = Wire.available() ? Wire.read() : 0xFF;
    Serial.printf("MPU WHO_AM_I = 0x%02X\n", id);

    if (id != 0x00 && id != 0xFF) {          // Chip antwortet -> verwenden
      Wire.beginTransmission(0x68);
      Wire.write(0x6B); Wire.write(0x00);    // PWR_MGMT_1: aus dem Sleep-Modus wecken
      Wire.endTransmission();
      delay(100);
      Serial.println("Kompatibler MPU-Klon erkannt - wird verwendet.");
      mpuReady = true;
    } else {
      Serial.println("MPU6050 nicht gefunden! Verkabelung prüfen.");
    }
  }
  if (mpuReady) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G); // Bis zu 8g (gut für Stürze)
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);   // 44 Hz: Aufprall-Spitzen bleiben sichtbar
  }

  // Konfiguration laden
  loadConfig();

  // Tasks
  xTaskCreate(gpsTask,    "gpsTask",   10240, NULL, 1, NULL); // GPS + Ersatz-Ortung (HTTPS braucht Stack)
  xTaskCreate(smsTask,    "smsTask",    4096, NULL, 2, NULL);
  xTaskCreate(buttonTask, "buttonTask", 4096, NULL, 1, NULL);
  xTaskCreate(mpuTask,    "mpuTask",    8192, NULL, 1, NULL); // Sturzerkennung (50 Hz)
  xTaskCreate(callTask,   "callTask",   4096, NULL, 2, NULL); // NEU: Rückruf / Mithören

  // WLAN verbinden
  Serial.print("Verbinde mit WLAN \"");
  Serial.print(wifiSSID);
  Serial.print("\" ");

  WiFi.mode(WIFI_STA);
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(true);
  delay(100);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.print("Verbunden! IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    Serial.println("WLAN-Verbindung fehlgeschlagen!");
    Serial.println("Konfigurations-Access-Point gestartet.");
    Serial.println("WLAN: " + String(AP_SSID));
    Serial.println("Passwort: " + String(AP_PASS));
    Serial.println("IP-Adresse: " + WiFi.softAPIP().toString());
  }

  // HTTP-Routen
  server.on("/",             HTTP_GET, handleRoot);
  server.on("/getLocation",  HTTP_GET, handleGetLocation);
  server.on("/getMPU",       HTTP_GET, handleGetMPU);
  server.on("/getSMSStatus", HTTP_GET, handleGetSMSStatus);
  server.on("/savePhones",   HTTP_GET, handleSavePhones);
  server.on("/saveWifi",     HTTP_GET, handleSaveWifi);
  server.on("/sendSMS",      HTTP_GET, handleSendSMS);
  server.on("/sos",          HTTP_GET, handleSOS);
  server.on("/cancel",       HTTP_GET, handleCancel);   // NEU
  server.on("/hangup",       HTTP_GET, handleHangup);   // NEU

  server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Nicht gefunden");
  });

  server.begin();
  Serial.println("Async HTTP-Server gestartet. Oeffne die IP im Browser!");
}

void loop() {
  if (restartPending && millis() - restartTimer > 2000) {
    Serial.println("Neustart...");
    delay(100);
    ESP.restart();
  }

  if (!apMode && millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN verloren - versuche Neuverbindung...");
      WiFi.reconnect();
    }
  }

  vTaskDelay(pdMS_TO_TICKS(500));
}
