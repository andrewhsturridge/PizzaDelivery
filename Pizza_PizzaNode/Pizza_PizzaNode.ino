// Role: PIZZA_NODE (classic ESP32-WROOM)
// Place a pizza box (RFID) on the reader, then use 5 buttons to toggle
// ingredients ON/OFF. Button lamps mirror the current mask. Each change sends
// PIZZA_ING_UPDATE {uid, len, mask} to Central via ESP-NOW.
//
// Ingredients (bit mask):
// b0=Pepperoni, b1=Mushrooms, b2=Peppers, b3=Pineapple, b4=Ham

#define PIZZA_ROLE PIZZA_NODE
#define PIZZA_HOUSE_ID 0  // not used; we store a station id in NVS via CLAIM

#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaRfid.h"
#include "PizzaOta.h"

// ---------------- Pins (your map) ----------------
static const uint8_t RFID_CS  = 21;  // SDA
static const uint8_t RFID_RST = 22;
static const uint8_t SPI_SCK  = 18, SPI_MISO = 19, SPI_MOSI = 23;

// Button inputs (active-LOW to GND)
// Note: GPIO34/35 are input-only and have NO internal pullups → must have external pullups on hardware.
static const uint8_t BTN[5] = {14, 25, 13, 35, 34};

// Button lamp outputs (active-HIGH = ON)
static const uint8_t LMP[5] = {2, 15, 27, 12, 5};

// ---------------- Globals ----------------
static uint16_t g_seq = 1;

// NVS: persistent station id (CLAIM sets this once)
#include <Preferences.h>
Preferences prefs;
static uint8_t g_stationId = 0; // 0 = unclaimed

// OTA deferral
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

// Current tag + mask
static uint8_t g_uid[10]; static uint8_t g_uidLen = 0;
static bool    g_haveTag = false;
static uint32_t g_tagLastSeen = 0;
static uint8_t g_mask = 0; // bits: PEP, MUSH, PEPPERS, PINEAPPLE, HAM

// Debounce
static uint8_t  g_lastBtn[5] = {1,1,1,1,1};
static uint32_t g_lastChg[5] = {0,0,0,0,0};
static const uint32_t DEBOUNCE_MS = 30;

// Ingredient names (for logs)
static const char* INAME[5] = {"Pepperoni","Mushrooms","Peppers","Pineapple","Ham"};

// ---------------- Helpers ----------------
static void cfgLoad() {
  prefs.begin("pizza", false);
  g_stationId = prefs.getUChar("house_id", 0);
  prefs.end();
}
static void cfgSaveStationId(uint8_t id) {
  prefs.begin("pizza", false);
  prefs.putUChar("house_id", id);
  prefs.end();
  g_stationId = id;
}

static void sendHello() {
  HelloPayload hp{}; strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION; PizzaIdentity::mac(hp.mac);
  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, g_stationId, g_seq++, &hp, sizeof(hp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO (pizza node) id=%u", g_stationId);
}

static void lampsApply(uint8_t mask) {
  for (int i=0;i<5;i++) digitalWrite(LMP[i], (mask & (1<<i)) ? HIGH : LOW);
}

static void sendIngrUpdate() {
  if (!g_haveTag || g_uidLen == 0) return;
  PizzaIngrUpdatePayload p{}; p.uid_len = g_uidLen; memcpy(p.uid, g_uid, g_uidLen); p.mask = g_mask;
  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(PIZZA_ING_UPDATE, (Role)PIZZA_ROLE, g_stationId, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("ING_UPDATE mask=%02X (P=%d M=%d Pe=%d Pi=%d H=%d)",
          g_mask, !!(g_mask&1), !!(g_mask&2), !!(g_mask&4), !!(g_mask&8), !!(g_mask&16));
}

static void tagAttached(const uint8_t* uid, uint8_t len) {
  memcpy(g_uid, uid, len); g_uidLen = len; g_haveTag = true; g_tagLastSeen = millis();
  g_mask = 0; lampsApply(g_mask); // new card starts blank (all OFF)
  PZ_LOGI("TAG ATTACH uidLen=%u", len);
}

static void tagDetached() {
  g_haveTag = false; g_uidLen = 0;
  g_mask = 0; lampsApply(0); // clear lamps when no card
  PZ_LOGI("TAG DETACH");
}

// ---------------- RX handler ----------------
static bool matchOtaTarget(const OtaStartPayload* p) {
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0;i<sizeof(p->ids); i++) if (p->ids[i] == g_stationId) return true;
  return false;
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) { sendHello(); return; }

  // CLAIM (same as houses; sets a persistent station id)
  if (hdr.type == CLAIM && len >= sizeof(ClaimPayload)) {
    const ClaimPayload* cp = (const ClaimPayload*)payload;
    uint8_t my[6]; PizzaIdentity::mac(my);
    if (memcmp(my, cp->target_mac, 6) == 0 && (g_stationId == 0 || cp->force)) {
      PZ_LOGI("CLAIM: station_id=%u (force=%u)", cp->house_id, cp->force);
      cfgSaveStationId(cp->house_id);
      delay(50); ESP.restart();
    }
    return;
  }

  // OTA_START (defer to loop)
  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)) {
    const OtaStartPayload* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;
    OtaAckPayload ack{}; ack.accept = 1; ack.code = 0;
    uint8_t out[64]; size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, g_stationId, g_seq++, &ack, sizeof(ack), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);
    strlcpy(g_otaUrl, p->url, sizeof(g_otaUrl)); strlcpy(g_otaVer, p->ver, sizeof(g_otaVer));
    g_otaPending = true; PZ_LOGI("OTA queued: %s", g_otaUrl);
    return;
  }
}

// ---------------- Setup/Loop ----------------
void setup() {
  Serial.begin(115200); delay(100);
  PZ_LOGI("Pizza Node boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // Lamps off
  for (int i=0;i<5;i++) { pinMode(LMP[i], OUTPUT); digitalWrite(LMP[i], LOW); }

  // Buttons (active-LOW). 34/35 have no internal pullups → set INPUT.
  for (int i=0;i<5;i++) {
    if (BTN[i]==34 || BTN[i]==35) pinMode(BTN[i], INPUT);
    else                          pinMode(BTN[i], INPUT_PULLUP);
  }

  cfgLoad();

  // RFID
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  PizzaRfid::begin(RFID_CS, RFID_RST);

  // Radio
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);
  sendHello();
}

void loop() {
  PizzaNow::loop();

  // OTA deferral
  if (g_otaPending) {
    noInterrupts(); bool run = g_otaPending; g_otaPending = false; interrupts();
    if (run) {
      // (No panel here; just run OTA)
      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK) {
        OtaResultPayload rr{}; rr.ok = 0; rr.code = (uint8_t)res;
        uint8_t out[64]; size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, g_stationId, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
      }
    }
  }

  // --- RFID presence / attach / detach ---
  uint8_t uid[10]; uint8_t len=0;
  if (PizzaRfid::readUid(uid, len)) {
    g_tagLastSeen = millis();
    if (!g_haveTag || len != g_uidLen || memcmp(uid, g_uid, len) != 0) {
      tagAttached(uid, len);
    }
  } else {
    if (g_haveTag && (millis() - g_tagLastSeen) > 500) {
      tagDetached();
    }
  }

  // --- Buttons: toggle bits when a card is on the reader ---
  for (int i=0;i<5;i++) {
    uint8_t raw = digitalRead(BTN[i]); // HIGH idle (pullup), LOW pressed (active)
    // Debounce
    if (raw != g_lastBtn[i] && (millis() - g_lastChg[i]) > DEBOUNCE_MS) {
      g_lastBtn[i] = raw; g_lastChg[i] = millis();
      if (raw == LOW) { // pressed
        if (g_haveTag) {
          g_mask ^= (1<<i);
          lampsApply(g_mask);
          sendIngrUpdate();
          PZ_LOGI("Toggled %s -> %s", INAME[i], (g_mask&(1<<i))?"ON":"OFF");
        } else {
          // No tag present → ignore (lamps remain off)
          PZ_LOGI("Button %d pressed but no tag present", i+1);
        }
      }
    }
  }
}
