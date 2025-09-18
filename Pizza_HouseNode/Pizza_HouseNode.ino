// Role: HOUSE_NODE (Feather S3)
// Scans RC522 and broadcasts DELIVER_SCAN on new UID.
// Blinks window LEDs and beeps on DELIVER_RESULT(ok).

#define PIZZA_ROLE HOUSE_NODE
#define PIZZA_HOUSE_ID 1
#define PIZZA_ENABLE_RFID_MODULE
#define PIZZA_ENABLE_LEDS_MODULE
#define PIZZA_ENABLE_AUDIO_MODULE

#include <Arduino.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaRfid.h"
#include "PizzaWindowLeds.h"
#include "PizzaAudio.h"
#include <SPI.h>

static uint16_t g_seq = 1;

// --- Pins (House 1 mapping) ---
static const uint8_t PIN_WS2812 = 38;
static const uint16_t LED_COUNT = 60;
static const uint8_t RC522_CS = 5;
static const uint8_t RC522_RST = 11;
static const uint8_t SPI_SCK = 36, SPI_MISO = 37, SPI_MOSI = 35;

// --- Beep buffer (22050 Hz, ~200ms, 1kHz sine) ---
static int16_t beepBuf[4410/2]; // ~200 ms (~22050*0.2) -> 4410 samples; halve for size

static void fillBeep() {
  const float freq = 1000.0f, sr = 22050.0f;
  for (size_t i=0; i<sizeof(beepBuf)/sizeof(beepBuf[0]); ++i) {
    float t = (float)i / sr;
    float s = sinf(2.0f*PI*freq*t);
    beepBuf[i] = (int16_t)(s * 16000); // ~-12 dBFS
  }
}

static void sendHello() {
  HelloPayload hp{};
  strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION;
  PizzaIdentity::mac(hp.mac);

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO sent (node) id=%u", (unsigned)PIZZA_HOUSE_ID);
}

static void sendDeliverScan(const uint8_t* uid, uint8_t uidLen) {
  DeliverScanPayload p{};
  p.house_id = PIZZA_HOUSE_ID;
  p.uid_len = uidLen;
  memcpy(p.uid, uid, min<uint8_t>(uidLen, sizeof(p.uid)));

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(DELIVER_SCAN, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("DELIVER_SCAN uidLen=%u", uidLen);
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) {
    sendHello();
    return;
  }
  if (hdr.type == DELIVER_RESULT && len >= sizeof(DeliverResultPayload)) {
    const DeliverResultPayload* r = (const DeliverResultPayload*)payload;
    if (r->ok) {
      // Green blink + beep
      PizzaWindowLeds::blink(PizzaWindowLeds::rgb(0,255,0), 150, 150);
      PizzaAudio::playClip(beepBuf, sizeof(beepBuf)/2, 255);
      PZ_LOGI("DELIVER_RESULT OK");
    } else {
      // Red blink (no beep)
      PizzaWindowLeds::blink(PizzaWindowLeds::rgb(255,0,0), 150, 150);
      PZ_LOGI("DELIVER_RESULT ERR reason=%u", r->reason);
    }
  }
}

void setup() {
  Serial.begin(115200); delay(50);
  PZ_LOGI("HouseNode boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // LEDs + audio
  PizzaWindowLeds::begin(PIN_WS2812, LED_COUNT);
  fillBeep();
  PizzaAudio::beginI2S();

  // RFID (explicit SPI pins for Feather S3)
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  PizzaRfid::begin(RC522_CS, RC522_RST);

  // Radio
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);

  // Boot HELLO
  sendHello();
}

void loop() {
  PizzaNow::loop();
  PizzaWindowLeds::loop();

  // Poll RFID ~10 Hz
  static uint32_t nextPoll = 0;
  if ((int32_t)(millis() - nextPoll) >= 0) {
    nextPoll = millis() + 100;
    uint8_t uid[10]; uint8_t uidLen = 0;
    if (PizzaRfid::readUid(uid, uidLen)) {
      // rate-limit repeats of the same UID (2s)
      static uint8_t lastUid[10]; static uint8_t lastLen=0; static uint32_t lastAt=0;
      bool same = (uidLen == lastLen) && (memcmp(uid, lastUid, uidLen) == 0) && (millis() - lastAt < 2000);
      if (!same) {
        memcpy(lastUid, uid, uidLen); lastLen = uidLen; lastAt = millis();
        sendDeliverScan(uid, uidLen);
      }
    }
  }
}
