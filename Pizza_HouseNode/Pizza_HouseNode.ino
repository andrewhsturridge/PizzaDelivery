// Role: HOUSE_NODE (Feather S3)
// Scans RC522 and broadcasts DELIVER_SCAN on new UID.
// Blinks window LEDs and beeps on DELIVER_RESULT(ok).

#define PIZZA_ROLE HOUSE_NODE
#define PIZZA_ENABLE_RFID_MODULE
#define PIZZA_ENABLE_LEDS_MODULE
#define PIZZA_ENABLE_AUDIO_MODULE

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaRfid.h"
#include "PizzaAudio.h"
#include "PizzaOta.h"

static uint16_t g_seq = 1;
Preferences prefs;
static uint8_t g_houseId = 0;  // runtime house id from NVS

// --- Pins (House 1 mapping) ---
static const uint8_t  PIN_WS2812 = 38;
static const uint16_t LED_COUNT  = 90;
static const uint8_t  RC522_CS   = 5;
static const uint8_t  RC522_RST  = 11;
static const uint8_t  SPI_SCK    = 36, SPI_MISO = 37, SPI_MOSI = 35;

// --- OTA deferral ---
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

Adafruit_NeoPixel strip(LED_COUNT, PIN_WS2812, NEO_GRB + NEO_KHZ800);

// --- Beep buffer (22050 Hz, ~200ms, 1kHz sine) ---
static int16_t beepBuf[4410/2]; // ~200 ms (~22050*0.2) -> 4410 samples; halve for size

// LED effect state (driven from loop)
enum Effect { EFFECT_NONE, EFFECT_OK_PULSE, EFFECT_ERR_PULSE, EFFECT_YELLOW_PING };
static Effect   g_fx      = EFFECT_NONE;
static uint32_t g_fxUntil = 0;   // millis() deadline for one-shot effects

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
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, g_houseId, g_seq++, &hp, sizeof(hp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO sent (node) id=%u", (unsigned)g_houseId);
}

static void sendDeliverScan(const uint8_t* uid, uint8_t uidLen) {
  DeliverScanPayload p{};
  p.house_id = g_houseId;
  p.uid_len  = uidLen;
  memcpy(p.uid, uid, min<uint8_t>(uidLen, sizeof(p.uid)));

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(DELIVER_SCAN, (Role)PIZZA_ROLE, g_houseId, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("DELIVER_SCAN uidLen=%u", uidLen);
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) { sendHello(); return; }

  if (hdr.type == DELIVER_RESULT && len >= sizeof(DeliverResultPayload)) {
    const DeliverResultPayload* r = (const DeliverResultPayload*)payload;
    if (r->ok) {
      g_fx = EFFECT_OK_PULSE;
      g_fxUntil = millis() + 600;          // 600ms green pulse
      PizzaAudio::playClip(beepBuf, sizeof(beepBuf)/2, 255);
      PZ_LOGI("DELIVER_RESULT OK");
    } else {
      g_fx = EFFECT_ERR_PULSE;
      g_fxUntil = millis() + 600;          // red pulse
      PZ_LOGI("DELIVER_RESULT ERR reason=%u", r->reason);
    }
  }

  if (hdr.type == SOUND_PLAY && len >= sizeof(SoundPlayPayload)) {
    const SoundPlayPayload* sp = (const SoundPlayPayload*)payload;
    if (sp->house_id == g_houseId) {
      PizzaAudio::playClip(beepBuf, sizeof(beepBuf)/2, sp->vol ? sp->vol : 200);
      g_fx = EFFECT_YELLOW_PING; g_fxUntil = millis() + 240; // short yellow ping
      PZ_LOGI("SOUND_PLAY vol=%u", sp->vol);
    }
  }

  if (hdr.type == CLAIM && len >= sizeof(ClaimPayload)) {
    const ClaimPayload* cp = (const ClaimPayload*)payload;

    uint8_t myMac[6]; PizzaIdentity::mac(myMac);
    bool macMatch = memcmp(myMac, cp->target_mac, 6) == 0;

    if (macMatch && (g_houseId == 0 || cp->force)) {
      PZ_LOGI("CLAIM received: setting house_id=%u (force=%u)", cp->house_id, cp->force);
      cfgSaveHouseId(cp->house_id);
      delay(50);
      ESP.restart();  // come back with new ID and HELLO
    }
  }

  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)) {
    const OtaStartPayload* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;

    // ACK quickly (still ok from callback)
    OtaAckPayload ack{}; ack.accept = 1; ack.code = 0;
    uint8_t out[64];
    size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, g_houseId, g_seq++, &ack, sizeof(ack), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);

    // Defer actual OTA to loop()
    strlcpy(g_otaUrl, p->url, sizeof(g_otaUrl));
    strlcpy(g_otaVer, p->ver, sizeof(g_otaVer));
    g_otaPending = true;
    PZ_LOGI("OTA queued: %s", g_otaUrl);
    return;
  }

}

static void cfgLoad() {
  prefs.begin("pizza", false);
  g_houseId = prefs.getUChar("house_id", 0);  // 0 = unclaimed
  prefs.end();
}

static void cfgSaveHouseId(uint8_t id) {
  prefs.begin("pizza", false);
  prefs.putUChar("house_id", id);
  prefs.end();
  g_houseId = id;
}

static bool matchOtaTarget(const OtaStartPayload* p) {
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0; i<sizeof(p->ids); i++) if (p->ids[i] == g_houseId) return true;
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(250);  // let USB/serial settle

  // LEDs first
  pinMode(PIN_WS2812, OUTPUT);
  strip.begin();
  strip.setBrightness(128);
  strip.clear(); strip.show();

  // quick proof-of-life
  for (int k=0;k<3;k++){
    uint32_t c=strip.Color(k==0?255:0, k==1?255:0, k==2?255:0);
    for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i,c);
    strip.show(); delay(150);
  }
  strip.clear(); strip.show();

  auto rr = esp_reset_reason();
  PZ_LOGI("HouseNode boot fw=%s mac=%s reset_reason=%d",
          PizzaIdentity::fw(), PizzaIdentity::macStr().c_str(), (int)rr);

  cfgLoad();

  // Radio next
  if (!PizzaNow::begin(ESPNOW_CHANNEL)) { PZ_LOGE("ESPNOW init failed"); }
  PizzaNow::onReceive(onRx);
  sendHello();
  delay(200);

  // RFID
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  if (!PizzaRfid::begin(RC522_CS, RC522_RST)) { PZ_LOGE("RFID init failed"); }
  delay(10);

  // AUDIO last
  fillBeep();
  if (!PizzaAudio::beginI2S()) { PZ_LOGE("I2S init failed"); }
  delay(10);

  PZ_LOGI("Node init complete");
}

void loop() {
  PizzaNow::loop();

  if (g_otaPending) {
    // take the job atomically
    noInterrupts();
    bool run = g_otaPending; g_otaPending = false;
    interrupts();

    if (run) {
      // Quiet LEDs during update
      strip.clear(); strip.show();

      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK) {
        OtaResultPayload rr{}; rr.ok = 0; rr.code = (uint8_t)res;
        uint8_t out[64];
        size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, g_houseId, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
      }
      // success path reboots inside start()
    }
  }

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

  // LED effects (no heartbeat; idle = off)
  uint32_t now = millis();
  switch (g_fx) {
    case EFFECT_OK_PULSE:
      if (now <= g_fxUntil) {
        for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(0,64,0));
        strip.show();
      } else { strip.clear(); strip.show(); g_fx = EFFECT_NONE; }
      break;

    case EFFECT_ERR_PULSE:
      if (now <= g_fxUntil) {
        for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(64,0,0));
        strip.show();
      } else { strip.clear(); strip.show(); g_fx = EFFECT_NONE; }
      break;

    case EFFECT_YELLOW_PING:
      if (now <= g_fxUntil) {
        for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(80,80,0));
        strip.show();
      } else { strip.clear(); strip.show(); g_fx = EFFECT_NONE; }
      break;

    case EFFECT_NONE:
    default:
      // stay off
      break;
  }

  delay(1); // cooperative yield
}
