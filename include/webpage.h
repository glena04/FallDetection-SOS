// webpage.h – HTML/CSS/JS der Weboberfläche
// Fall Detection & SOS Device – Firmware v7.5.2 (PlatformIO)
#pragma once
#include <Arduino.h>

static const char htmlPage[] PROGMEM = R"rawliteral(
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
          document.getElementById('sensorWarn').innerText =
            d.ok ? '' : '⚠ Sturzsensor ausgefallen – Sturzerkennung inaktiv!';
        })
        .catch(e => console.error('Fehler:', e));
    }
    function sendSOS() {
      if (!confirm('Notruf-SMS an alle gespeicherten Nummern senden?')) return;
      document.getElementById('smsstatus').innerText = 'Sende Notruf...';
      fetch('/sos', { method: 'POST' }).then(r => r.text())
        .then(d => { document.getElementById('smsstatus').innerText = d; })
        .catch(e => { document.getElementById('smsstatus').innerText = 'Fehler: ' + e; });
    }
    function cancelAlarm() {
      fetch('/cancel', { method: 'POST' }).then(r => r.text())
        .then(d => { document.getElementById('smsstatus').innerText = d; })
        .catch(e => console.error('Fehler:', e));
    }
    function hangup() {
      fetch('/hangup', { method: 'POST' }).then(r => r.text())
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
    <p id="sensorWarn" style="color:#dc3545;font-weight:bold;"></p>
    <p class="hint">Automatischer SOS bei Aufprall > __THRESHOLD__ m/s² + Lageänderung (liegend/ruhig)</p>
  </div>

  <div class="box">
    <h1>💬 SMS manuell senden</h1>
    <form action="/sendSMS" method="POST">
      Telefonnummer (mit Ländervorwahl):<br>
      <input type="text" name="number" placeholder="+49123456789" maxlength="20"><br>
      Nachricht (max. 80 Zeichen, der Standort wird angehängt):<br>
      <input type="text" name="message" value="Notfall!" maxlength="80"><br>
      <input type="submit" value="SMS mit Standort senden">
    </form>
  </div>

  <div class="box">
    <h1>📞 Notrufnummern (max. 3)</h1>
    <form action="/savePhones" method="POST">
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
    <form action="/saveWifi" method="POST">
      WLAN-Name (SSID):<br>
      <input type="text" name="ssid" value="__SSID__" maxlength="32"><br>
      WLAN-Passwort:<br>
      <input type="password" name="pass" placeholder="leer lassen = unverändert" maxlength="63" autocomplete="new-password"><br>
      <label><input type="checkbox" name="open" value="1"> Offenes WLAN (ohne Passwort)</label><br>
      <input type="submit" value="Speichern & Neustart">
    </form>
  </div>

</body>
</html>
)rawliteral";
