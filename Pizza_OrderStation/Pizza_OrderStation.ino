// Role: ORDERS_NODE (ESP32-WROOM)
// - Receives ORDER_LIST_RESET + ORDER_ITEM_SET (0..2 slots)
// - Cycles display with button
// - Adds local countdown prefix "NNs " based on remain_s + receive time
// - Sends ORDER_SHOW_TEXT as if from CENTRAL so OrdersPanel always accepts

#define PIZZA_ROLE ORDERS_NODE
#define PIZZA_HOUSE_ID 0

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <cstddef>  // offsetof

#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaOta.h"
#include "PizzaNetCfg.h"

#define BTN_PIN  13
#define LAMP_PIN 27

// ---------- Generic NeoPixel Ring Config ----------
#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN        4
#endif
#ifndef NEOPIXEL_COUNT
#define NEOPIXEL_COUNT      20
#endif
#ifndef NEOPIXEL_BRIGHTNESS
#define NEOPIXEL_BRIGHTNESS 100
#endif

static Adafruit_NeoPixel neopixelRing(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

static uint16_t g_seq = 1;
static uint32_t g_helloDueAt = 0;

// ===== Orders window (max 3 items displayed/cycled) =====
static PzOrderItemSetPayload OS_orders[3];       // 0..2
static uint32_t              OS_recvMs[3] = {0}; // when ITEM_SET last received per slot
static uint8_t               OS_count    = 0;    // 0..3
static uint8_t               OS_index    = 0;    // 0..OS_count-1
static bool                  OS_loading  = false;

// De-dupe + send pacing
static char     OS_lastText[PZ_ORDER_TEXT_MAX] = "";
static uint32_t OS_lastSendMs = 0;

// Refresh cadence for countdown display
static uint32_t OS_nextRefreshMs = 0;

// Debounce
static bool     g_lastRaw = true, g_btn = true;
static uint32_t g_lastEdgeMs = 0;

// OTA deferral
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

// OTA progress state (updated by callback, rendered in loop)
static volatile size_t otaDone  = 0;
static volatile size_t otaTotal = 0;
static volatile bool   otaActive = false;

static uint32_t g_lampUntil = 0;

// v1 size of ORDER_ITEM_SET payload (before v2 trailing fields).
static const uint16_t PZ_ORDER_ITEM_V1_SIZE = (uint16_t)offsetof(PzOrderItemSetPayload, order_id);

// ---------- helpers ----------
static void sendHello() {
  HelloPayload hp{};
  strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION;
  PizzaIdentity::mac(hp.mac);

  uint8_t out[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++,
                                 &hp, sizeof(hp), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n);
  PZ_LOGI("HELLO sent (orders-node)");
}

// --- Unified: always send ORDER_SHOW_TEXT as if from CENTRAL ---
static void OS_sendShowTextUnified(const char* s) {
  const char* safe = (s && *s) ? s : "NO ORDERS";

  PzOrderShowTextPayload p{};
  strlcpy(p.text, safe, sizeof(p.text));

  uint8_t buf[256];
  static uint16_t seq_for_show = 1;

  // NOTE: use CENTRAL here so Orders Panel never filters us out
  size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, CENTRAL, 0, seq_for_show++,
                                 &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  Serial.printf("[OS] SHOW -> \"%s\"\n", p.text);
}

static void showNoOrders(bool force = false) {
  (void)force;
  OS_sendShowTextUnified("NO ORDERS");
}

// Remaining seconds for a slot, based on the last remain_s received + elapsed millis().
static uint16_t OS_remainS(uint8_t slot) {
  if (slot >= 3) return 0;

  uint16_t base = OS_orders[slot].remain_s;
  if (base == 0) return 0;

  uint32_t rx = OS_recvMs[slot];
  if (rx == 0) return base;

  uint32_t elapsedS = (millis() - rx) / 1000UL;
  if (elapsedS >= base) return 0;
  return (uint16_t)(base - elapsedS);
}

// Build the display string for a slot, including time prefix (e.g., "09s <text>")
static bool OS_buildDisplayForSlot(uint8_t slot, char* out, size_t outSz) {
  if (!out || outSz == 0) return false;

  if (OS_count == 0 || OS_loading) {
    strlcpy(out, "NO ORDERS", outSz);
    return true;
  }
  if (slot >= OS_count) slot = 0;

  const char* t = OS_orders[slot].text;
  if (!t[0]) {
    // Item has not arrived yet
    return false;
  }

  uint16_t rem = OS_remainS(slot);

  // If Central didn't send remain_s yet, just show text
  if (OS_orders[slot].remain_s == 0) {
    strlcpy(out, t, outSz);
    return true;
  }

  if (rem > 999) rem = 999;
  snprintf(out, outSz, "%02us %s", (unsigned)rem, t);
  return true;
}

// Draw current selection (with de-dupe)
static void OS_showCurrent(bool force) {
  char buf[PZ_ORDER_TEXT_MAX] = {0};

  if (OS_count == 0) {
    if (force || strcmp(OS_lastText, "NO ORDERS") != 0) {
      showNoOrders(true);
      strlcpy(OS_lastText, "NO ORDERS", sizeof(OS_lastText));
      OS_lastSendMs = millis();
    }
    return;
  }

  if (OS_loading) {
    // Don’t spam display while loading slots.
    return;
  }

  if (OS_index >= OS_count) OS_index = 0;

  if (!OS_buildDisplayForSlot(OS_index, buf, sizeof(buf))) {
    Serial.printf("[OS] slot %u empty; waiting for ITEM_SET...\n", OS_index);
    return;
  }

  if (!force) {
    if (strncmp(OS_lastText, buf, sizeof(OS_lastText)) == 0) return;
    if ((millis() - OS_lastSendMs) < 80) return;
  }

  OS_sendShowTextUnified(buf);
  strlcpy(OS_lastText, buf, sizeof(OS_lastText));
  OS_lastSendMs = millis();
}

// Periodic refresh so the displayed countdown decreases in real time
static void OS_tickRefresh() {
  if (OS_count == 0 || OS_loading) return;

  uint32_t now = millis();
  if ((int32_t)(now - OS_nextRefreshMs) < 0) return;

  OS_nextRefreshMs = now + 250; // 4Hz check; de-dupe yields ~1Hz sends
  OS_showCurrent(false);
}

// ---------- Neopixel helpers (OTA) ----------
static void neoRingInit() {
  neopixelRing.begin();
  neopixelRing.setBrightness(NEOPIXEL_BRIGHTNESS);
  neopixelRing.clear();
  neopixelRing.show();
}

static void neoRingClear() {
  for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) neopixelRing.setPixelColor(i, 0);
  neopixelRing.show();
}

static void neoRingBegin() {
  otaActive = true;
  otaDone   = 0;
  otaTotal  = 0;
}

static void neoRingProgressDirectCB(size_t done, size_t total) {
  otaDone = done;
  otaTotal = total;
  otaActive = true;

  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 30) return;
  last = now;

  if (total == 0) {
    static uint8_t pos = 0;
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) neopixelRing.setPixelColor(i, 0);
    neopixelRing.setPixelColor(pos % NEOPIXEL_COUNT,     neopixelRing.Color(0,0,40));
    neopixelRing.setPixelColor((pos+1) % NEOPIXEL_COUNT, neopixelRing.Color(0,0,10));
    neopixelRing.show();
    pos++;
    return;
  }

  uint16_t lit = (uint32_t)done * NEOPIXEL_COUNT / total;
  for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
    neopixelRing.setPixelColor(i, (i < lit) ? neopixelRing.Color(0,50,50) : 0);
  }
  if (lit < NEOPIXEL_COUNT) neopixelRing.setPixelColor(lit, neopixelRing.Color(0,10,40));
  neopixelRing.show();

  if (done == total) {
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) neopixelRing.setPixelColor(i, neopixelRing.Color(0,60,0));
    neopixelRing.show();
  }
}

static void neoRingBlinkError() {
  for (int k = 0; k < 3; k++) {
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) neopixelRing.setPixelColor(i, neopixelRing.Color(80,0,0));
    neopixelRing.show();
    delay(120);
    neoRingClear();
    delay(120);
  }
}

static inline void lampPulseStart(uint16_t ms) {
  digitalWrite(LAMP_PIN, LOW);
  g_lampUntil = millis() + ms;
}

static inline void lampPulseTick() {
  if (g_lampUntil && (int32_t)(millis() - g_lampUntil) >= 0) {
    digitalWrite(LAMP_PIN, HIGH);
    g_lampUntil = 0;
  }
}

// ---------- Rx handler ----------
static bool matchOtaTarget(const OtaStartPayload* p) {
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true;
  for (uint8_t i = 0; i < sizeof(p->ids); i++) if (p->ids[i] == PIZZA_HOUSE_ID) return true;
  return false;
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) {
    uint32_t jitter = 50 + (esp_random() % 300);
    g_helloDueAt = millis() + jitter + ((PIZZA_HOUSE_ID & 7) * 40);
    return;
  }

  // === ORDERS: window RESET ===
  if (hdr.type == ORDER_LIST_RESET && len >= sizeof(PzOrderListResetPayload)) {
    const PzOrderListResetPayload* r = (const PzOrderListResetPayload*)payload;

    OS_count   = (r->count > 3) ? 3 : r->count;
    OS_index   = 0;
    OS_loading = (OS_count > 0);

    for (uint8_t i = 0; i < 3; ++i) {
      OS_orders[i].index    = i;
      OS_orders[i].house_id = 0;
      OS_orders[i].mask     = 0;
      OS_orders[i].text[0]  = '\0';
      OS_orders[i].order_id = 0;
      OS_orders[i].remain_s = 0;
      OS_recvMs[i] = 0;
    }

    OS_lastText[0] = '\0';
    OS_nextRefreshMs = 0;

    Serial.printf("[OS] RESET: count=%u (loading=%u)\n", OS_count, OS_loading);
    if (OS_count == 0) OS_showCurrent(true);
    return;
  }

  // === ORDERS: item arrives ===
  if (hdr.type == ORDER_ITEM_SET && len >= PZ_ORDER_ITEM_V1_SIZE) {
    PzOrderItemSetPayload tmp{};
    size_t toCopy = (len < sizeof(tmp)) ? len : sizeof(tmp);
    memcpy(&tmp, payload, toCopy);

    if (tmp.index < 3) {
      OS_orders[tmp.index] = tmp;
      OS_recvMs[tmp.index] = millis();

      Serial.printf("[OS] ITEM_SET: i=%u H%u mask=0x%02X order_id=%u remain=%us text=\"%s\"\n",
                    tmp.index, tmp.house_id, tmp.mask,
                    (unsigned)tmp.order_id, (unsigned)tmp.remain_s, tmp.text);

      OS_loading = false;
      if (tmp.index == OS_index || tmp.index == 0) {
        OS_showCurrent(true);
      }
    }
    return;
  }

  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)) {
    const auto* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;

    digitalWrite(LAMP_PIN, LOW);
    neoRingBegin();

    OtaAckPayload ack{}; ack.accept = 1; ack.code = 0;
    uint8_t out[64];
    size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++,
                                   &ack, sizeof(ack), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);

    strlcpy(g_otaUrl, p->url, sizeof(g_otaUrl));
    strlcpy(g_otaVer, p->ver, sizeof(g_otaVer));
    g_otaPending = true;
    PZ_LOGI("OTA queued: %s", g_otaUrl);
    return;
  }

  // Accept runtime SSID/PASS/BASE from Central
  if (hdr.type == NET_CFG_SET && len >= sizeof(NetCfgSetPayload)) {
    const NetCfgSetPayload* p = (const NetCfgSetPayload*)payload;

    NetCfg::Value v{};
    strlcpy(v.ssid, p->ssid, sizeof(v.ssid));
    strlcpy(v.pass, p->pass, sizeof(v.pass));
    strlcpy(v.base, p->base, sizeof(v.base));

    bool ok = NetCfg::save(v);
    Serial.printf("[OrdersNode] NET_CFG_SET: ssid=\"%s\" base=\"%s\" save=%s\n",
                  v.ssid, v.base, ok ? "OK" : "FAIL");

    esp_restart();
    return;
  }
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("[OrdersNode] fw=%s mac=%s\n", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LAMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, HIGH);

  if (!PizzaNow::begin(ESPNOW_CHANNEL)) {
    PZ_LOGE("ESPNOW init failed");
  }
  PizzaNow::onReceive(onRx);

  neoRingInit();
  PizzaOta::setProgressCallback(neoRingProgressDirectCB);

  showNoOrders(true);
  sendHello();
}

void loop() {
  PizzaNow::loop();

  if (g_helloDueAt && (int32_t)(millis() - g_helloDueAt) >= 0) {
    sendHello();
    g_helloDueAt = 0;
  }

  // OTA (deferred)
  if (g_otaPending) {
    noInterrupts();
    bool run = g_otaPending;
    g_otaPending = false;
    interrupts();

    if (run) {
      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK) {
        neoRingBlinkError();
        otaActive = false;
        neoRingClear();

        OtaResultPayload rr{}; rr.ok = 0; rr.code = (uint8_t)res;
        uint8_t out[64];
        size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++,
                                       &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
      }
    }
  }

  // === ORDER STATION: debounced button handler (90ms), no delay() ===
  bool raw = digitalRead(BTN_PIN);
  if (raw != g_lastRaw) {
    g_lastRaw = raw;
    g_lastEdgeMs = millis();
  }

  if (millis() - g_lastEdgeMs > 90) {
    if (raw != g_btn) {
      g_btn = raw;
      if (g_btn == LOW) {
        lampPulseStart(60);

        if (OS_count == 0 || OS_loading) {
          Serial.println("[OS] press ignored (no items or still loading)");
        } else {
          if (OS_index >= OS_count) OS_index = 0;
          OS_index = (OS_index + 1) % OS_count;
          Serial.printf("[OS] PRESS -> index=%u of %u\n", OS_index, OS_count);
          OS_showCurrent(true);
        }
      }
    }
  }

  lampPulseTick();

  // keep countdown current
  OS_tickRefresh();
}
