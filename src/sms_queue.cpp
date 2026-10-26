// sms_queue.cpp – SMS werden vor dem Senden im Flash gespeichert
// Fall Detection & SOS Device – Firmware v7.5.1 (PlatformIO)
#include "sms_queue.h"
#include "globals.h"
#include "util.h"

static SemaphoreHandle_t nvsMutex = nullptr;
static Preferences       qprefs;
static bool              slotUsed[SMS_SLOTS] = {false};

static String slotKey(int i) { return "s" + String(i); }

void releaseSlot(int8_t slot) {
  if (slot < 0 || slot >= SMS_SLOTS) return;
  if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    qprefs.remove(slotKey(slot).c_str());
    slotUsed[slot] = false;
    xSemaphoreGive(nvsMutex);
  }
}

// Speichern + in die Warteschlange. true = eingereiht
bool queueSms(const String& numberIn, const String& messageIn) {
  // v7.4 Stufe 2: Nummer prüfen und Text bereinigen (gegen AT-Befehl-Einschleusung)
  String number;
  if (!normalizePhone(numberIn, number)) {
    Serial.println("SMS verworfen: ungültige Nummer " + numberIn);
    return false;
  }
  String message = sanitizeSmsText(messageIn);

  SmsRequest req;
  memset(&req, 0, sizeof(req));
  strncpy(req.number,  number.c_str(),  sizeof(req.number)  - 1);
  strncpy(req.message, message.c_str(), sizeof(req.message) - 1);
  req.slot = -1;

  if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    for (int i = 0; i < SMS_SLOTS; i++) {
      if (!slotUsed[i]) {
        slotUsed[i] = true;
        req.slot = i;
        qprefs.putBytes(slotKey(i).c_str(), &req, sizeof(req));
        break;
      }
    }
    xSemaphoreGive(nvsMutex);
  }

  if (xQueueSend(smsQueue, &req, 0) != pdTRUE) {
    releaseSlot(req.slot);
    return false;
  }
  return true;
}

// Beim Start: nicht gesendete SMS aus dem Flash wieder einreihen
static int restorePendingSms() {
  int restored = 0;
  for (int i = 0; i < SMS_SLOTS; i++) {
    String k = slotKey(i);
    if (!qprefs.isKey(k.c_str())) continue;
    SmsRequest req;
    if (qprefs.getBytesLength(k.c_str()) == sizeof(req) &&
        qprefs.getBytes(k.c_str(), &req, sizeof(req)) == sizeof(req)) {
      req.number[sizeof(req.number) - 1]   = 0;
      req.message[sizeof(req.message) - 1] = 0;
      req.slot = i;
      slotUsed[i] = true;
      if (xQueueSend(smsQueue, &req, 0) == pdTRUE) { restored++; continue; }
    }
    qprefs.remove(k.c_str());   // ungültig/alt -> löschen
    slotUsed[i] = false;
  }
  return restored;
}

int smsQueueBegin() {
  nvsMutex = xSemaphoreCreateMutex();
  smsQueue = xQueueCreate(SMS_SLOTS, sizeof(SmsRequest));
  qprefs.begin("smsqueue", false);
  return restorePendingSms();
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
    if (queueSms(nums[i], message)) queued++;
  }
  return queued;
}
