// Role: ORDERS_NODE (ESP32-WROOM) – auto-displays orders list + button cycles items
#define PIZZA_ROLE ORDERS_NODE
#define PIZZA_HOUSE_ID 0

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaOta.h"

#define BTN_PIN  13
#define LAMP_PIN 27

// ---------- Generic NeoPixel Ring Config ----------
#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN        4       // DIN (yours is GPIO 4)
#endif
#ifndef NEOPIXEL_COUNT
#define NEOPIXEL_COUNT      20      // your ring size
#endif
#ifndef NEOPIXEL_BRIGHTNESS
#define NEOPIXEL_BRIGHTNESS 100
#endif

static Adafruit_NeoPixel neopixelRing(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

static uint16_t g_seq = 1;
static uint32_t g_helloDueAt = 0;

// Orders list
static uint8_t g_expected = 0;
static uint8_t g_count    = 0;
static uint8_t g_index    = 0;
static PzOrderItemSetPayload g_items[PZ_ORDERS_MAX];

// Dedup / anti-spam
static char     g_lastSent[PZ_ORDER_TEXT_MAX] = {0};
static uint32_t g_lastSentAt = 0;
static const uint32_t RESEND_MIN_MS = 5000; // don't resend identical text more often than this

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

// ---------- helpers ----------
static void sendHello(){
  HelloPayload hp{}; strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION; PizzaIdentity::mac(hp.mac);
  uint8_t out[128]; size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n); PZ_LOGI("HELLO sent (orders-node)");
}

static void sendShowText(const char* s, bool force=false){
  const char* safe = (s && *s) ? s : "NO ORDERS";

  // STRICT DEDUPE: if text is identical, do nothing unless forced
  if (!force && strncmp(g_lastSent, safe, sizeof(g_lastSent)) == 0) {
    return;
  }

  PzOrderShowTextPayload p{}; strlcpy(p.text, safe, sizeof(p.text));

  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, (Role)PIZZA_ROLE, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);

  strlcpy(g_lastSent, safe, sizeof(g_lastSent));
  g_lastSentAt = millis();   // keep if you also use time-based suppression elsewhere
  Serial.printf("[OrdersNode] ORDER_SHOW_TEXT len=%u\n", (unsigned)strlen(p.text));
}

static void showNoOrders() {
  sendShowText("NO ORDERS");
}

static void clearItems() {
  for (uint8_t i=0;i<PZ_ORDERS_MAX;i++){
    g_items[i].index=i; g_items[i].house_id=0; g_items[i].mask=0; memset(g_items[i].text,0,sizeof(g_items[i].text));
  }
}

static void showCurrent(bool force) {
  if (g_expected == 0 || g_count == 0) { showNoOrders(); return; }

  // Only consider the visible window: at most 3
  uint8_t win = (g_expected > 3) ? 3 : g_expected;

  // If current is invalid or empty, jump to the first used slot in the window
  if (g_index >= win || g_items[g_index].text[0] == 0) {
    for (uint8_t i=0; i<win; ++i) {
      if (g_items[i].text[0] != 0) { g_index = i; break; }
    }
  }

  // Still nothing valid? then no orders to show
  if (g_index >= win || g_items[g_index].text[0] == 0) {
    showNoOrders(); return;
  }

  sendShowText(g_items[g_index].text, force);
}

static void recomputeCountContiguous() {
  uint8_t used = 0;
  uint8_t win  = (g_expected > 3) ? 3 : g_expected;  // we only ever display up to 3
  for (uint8_t i=0; i<win; ++i) {
    if (g_items[i].text[0] != 0) used++;
  }
  g_count = used;   // number of present items in the window (can have gaps)
}

// ---------- Neopixel helpers ----------
static void neoRingInit() {
  neopixelRing.begin();
  neopixelRing.setBrightness(NEOPIXEL_BRIGHTNESS);
  neopixelRing.show();
}

static void neoRingClear() {
  for (uint16_t i=0;i<NEOPIXEL_COUNT;i++) neopixelRing.setPixelColor(i, 0);
  neopixelRing.show();
}

// Call when OTA campaign *for this device* begins (after target check)
static void neoRingBegin() {
  otaActive = true;
  otaDone   = 0;
  otaTotal  = 0; // spinner mode until total known
}

// Render directly from the progress callback (works even while OTA is blocking)
static void neoRingProgressDirectCB(size_t done, size_t total) {
  // (optional) keep vars updated too
  otaDone = done; otaTotal = total; otaActive = true;

  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 30) return;   // throttle to ~33 FPS
  last = now;

  if (total == 0) {
    // Spinner while size unknown
    static uint8_t pos = 0;
    for (uint16_t i=0;i<NEOPIXEL_COUNT;i++) neopixelRing.setPixelColor(i, 0);
    neopixelRing.setPixelColor(pos % NEOPIXEL_COUNT,     neopixelRing.Color(0,0,40)); // head
    neopixelRing.setPixelColor((pos+1) % NEOPIXEL_COUNT, neopixelRing.Color(0,0,10)); // tail
    neopixelRing.show();
    pos++;
    return;
  }

  // Fill based on progress
  uint16_t lit = (uint32_t)done * NEOPIXEL_COUNT / total;  // 0..N
  for (uint16_t i=0;i<NEOPIXEL_COUNT;i++)
    neopixelRing.setPixelColor(i, (i < lit) ? neopixelRing.Color(0,50,50) : 0);
  if (lit < NEOPIXEL_COUNT) neopixelRing.setPixelColor(lit, neopixelRing.Color(0,10,40)); // head
  neopixelRing.show();

  if (done == total) {
    // Brief success flash; reboot typically follows
    for (uint16_t i=0;i<NEOPIXEL_COUNT;i++) neopixelRing.setPixelColor(i, neopixelRing.Color(0,60,0));
    neopixelRing.show();
  }
}

static void neoRingBlinkError() {
  for (int k=0;k<3;k++) {
    for (uint16_t i=0;i<NEOPIXEL_COUNT;i++) neopixelRing.setPixelColor(i, neopixelRing.Color(80,0,0));
    neopixelRing.show(); delay(120);
    neoRingClear(); delay(120);
  }
}

// ---------- Rx handler ----------
static bool matchOtaTarget(const OtaStartPayload* p){
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0;i<sizeof(p->ids);i++) if (p->ids[i] == PIZZA_HOUSE_ID) return true;
  return false;
}

// signature matches PizzaNow::RxHandler
static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]){
  if (hdr.type == HELLO_REQ){ 
    uint32_t jitter = 50 + (esp_random() % 300);           // 50..350 ms
    g_helloDueAt = millis() + jitter + ((PIZZA_HOUSE_ID & 7) * 40);
    return;
   }

  if (hdr.type == ORDER_LIST_RESET && len >= sizeof(PzOrderListResetPayload)){
    const auto* p = (const PzOrderListResetPayload*)payload;
    g_expected = min<uint8_t>(p->count, PZ_ORDERS_MAX);
    clearItems();
    g_count = 0; g_index = 0;
    Serial.printf("[OrdersNode] RESET expect=%u\n", g_expected);
    if (g_expected == 0) showNoOrders();
    return;
  }

  if (hdr.type == ORDER_ITEM_SET && len >= sizeof(PzOrderItemSetPayload)){
    const auto* p = (const PzOrderItemSetPayload*)payload;
    if (p->index < PZ_ORDERS_MAX && p->index < g_expected){
      uint8_t oldCount = g_count;
      g_items[p->index] = *p;
      recomputeCountContiguous();

      // If list grew from 0 -> >0, start at first item immediately
      if (oldCount == 0 && g_count > 0) {
        // show the first used slot (may not be index 0)
        for (uint8_t i=0; i<g_expected; ++i) {
          if (g_items[i].text[0] != 0) { g_index = i; break; }
        }
        showCurrent(/*force*/true);
        return;
      }

      // If current index went out of range (list shrank), clamp & show
      if (g_count == 0) { showNoOrders(); return; }
      if (g_index >= g_expected || g_items[g_index].text[0] == 0) {
        // hop to first used slot and draw
        for (uint8_t i=0; i<g_expected; ++i) {
          if (g_items[i].text[0] != 0) { g_index = i; break; }
        }
        showCurrent(/*force*/true);
        return;
      }

      // If the item we are displaying was updated, refresh the panel (dedup guards inside)
      if (p->index == g_index) {
        showCurrent(false);
      }
    }
    return;
  }

  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)){
    const auto* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;
    digitalWrite(LAMP_PIN, LOW);   // immediately signal "starting OTA"
    neoRingBegin(); 
    OtaAckPayload ack{}; ack.accept=1; ack.code=0;
    uint8_t out[64]; size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &ack, sizeof(ack), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);
    strlcpy(g_otaUrl, p->url, sizeof(g_otaUrl)); strlcpy(g_otaVer, p->ver, sizeof(g_otaVer));
    g_otaPending = true; PZ_LOGI("OTA queued: %s", g_otaUrl);
    return;
  }
}

void setup(){
  Serial.begin(115200); delay(50);
  Serial.printf("[OrdersNode] fw=%s mac=%s\n", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LAMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, HIGH);

  if (!PizzaNow::begin(ESPNOW_CHANNEL)) { PZ_LOGE("ESPNOW init failed"); }
  PizzaNow::onReceive(onRx);

  neoRingInit();
  PizzaOta::setProgressCallback(neoRingProgressDirectCB);

  showNoOrders(); // calls sendShowText(..., force=false)
  sendHello();
}

void loop(){
  PizzaNow::loop();

  if (g_helloDueAt && (int32_t)(millis() - g_helloDueAt) >= 0) {
    sendHello();
    g_helloDueAt = 0;
  }

  // OTA (deferred)
  if (g_otaPending){
    noInterrupts(); bool run = g_otaPending; g_otaPending=false; interrupts();
    if (run){
      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK){
        neoRingBlinkError();
        otaActive = false;
        neoRingClear();
        OtaResultPayload rr{}; rr.ok=0; rr.code=(uint8_t)res;
        uint8_t out[64]; size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
      }
    }
  }

  // Button: manual next (still supported)
  bool raw = digitalRead(BTN_PIN);
  if (raw != g_lastRaw){ g_lastRaw=raw; g_lastEdgeMs=millis(); }
  if (millis() - g_lastEdgeMs > 35){
    if (raw != g_btn){
      g_btn = raw;
      if (g_btn == LOW){
        digitalWrite(LAMP_PIN, LOW); delay(60); digitalWrite(LAMP_PIN, HIGH);

        if (g_expected == 0 || g_count == 0) {
          showNoOrders();
        } else {
          // scan forward for the next used slot, wrapping
          uint8_t win = g_expected;   // we only ever receive a window here (typically 3)
          for (uint8_t step=1; step<=win; ++step) {
            uint8_t idx = (g_index + step) % win;
            if (g_items[idx].text[0] != 0) { g_index = idx; break; }
          }
          showCurrent(/*force*/true);
        }
      }
    }
  }

}
