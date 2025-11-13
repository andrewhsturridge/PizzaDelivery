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
#include "PizzaNetCfg.h"

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

static uint32_t g_lampUntil = 0;

static bool os_windowLoading = false;

static PzOrderItemSetPayload OS_orders[3];   // window buffer 0..2
static uint8_t  OS_count    = 0;             // how many items in the window (0..3)
static uint8_t  OS_index    = 0;             // selected index 0..OS_count-1
static bool     OS_loading  = false;         // waiting for ITEM_SETs after RESET
static char     OS_lastText[128] = "";       // de-dupe buffer
static uint32_t OS_lastSendMs = 0;
static uint16_t OS_seq = 1;                  // local sequence for packets we send

// Local window buffer for the 0..2 items we display on the Order Station
static PzOrderItemSetPayload g_orders[3] = {};

// ---------- helpers ----------
static void sendHello(){
  HelloPayload hp{}; strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION; PizzaIdentity::mac(hp.mac);
  uint8_t out[128]; size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n); PZ_LOGI("HELLO sent (orders-node)");
}

static void sendShowText(const char* s, bool /*force*/=false) { OS_sendShowTextUnified(s); }
static void OS_sendShowText(const char* s) { OS_sendShowTextUnified(s); }

// --- Unified: always send ORDER_SHOW_TEXT as if from CENTRAL ---
static void OS_sendShowTextUnified(const char* s) {
  const char* safe = (s && *s) ? s : "NO ORDERS";

  PzOrderShowTextPayload p{};
  strlcpy(p.text, safe, sizeof(p.text));

  uint8_t buf[256];
  // NOTE: use CENTRAL here so Orders Panel never filters us out
  static uint16_t seq_for_show = 1;
  size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, CENTRAL, 0, seq_for_show++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  Serial.printf("[OS] SHOW -> \"%s\"\n", p.text);
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

// Draw current selection (with light de-dupe & empty-slot guard)
static void OS_showCurrent(bool force) {
  const char* s = "NO ORDERS";

  if (OS_count > 0) {
    const char* t = OS_orders[OS_index].text;   // text filled by ITEM_SET
    if (!t[0]) {                                // empty means item not arrived yet
      Serial.printf("[OS] slot %u empty; waiting for ITEM_SET...\n", OS_index);
      return;
    }
    s = t;
  }

  if (!force) {
    if (strcmp(OS_lastText, s) == 0 && (millis() - OS_lastSendMs) < 1000) {
      return; // de-dupe within 1s unless forced
    }
  }
  OS_sendShowText(s);
  strlcpy(OS_lastText, s, sizeof(OS_lastText));
  OS_lastSendMs = millis();
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
  neopixelRing.clear();
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
      OS_orders[i].text[0]  = '\0';       // mark as "not yet arrived"
    }
    OS_lastText[0] = '\0';                // reset de-dupe on new window

    Serial.printf("[OS] RESET: count=%u (loading=%u)\n", OS_count, OS_loading);

    // If count==0 show "NO ORDERS" once
    if (OS_count == 0) OS_showCurrent(/*force*/true);
    return;
  }

  // === ORDERS: item arrives ===
  if (hdr.type == ORDER_ITEM_SET && len >= sizeof(PzOrderItemSetPayload)) {
    const PzOrderItemSetPayload* it = (const PzOrderItemSetPayload*)payload;

    if (it->index < 3) {
      // Store entire payload for this slot
      OS_orders[it->index] = *it;
      Serial.printf("[OS] ITEM_SET: i=%u H%u mask=0x%02X text=\"%s\"\n",
                    it->index, it->house_id, it->mask, it->text);

      // First useful content has landed
      OS_loading = false;

      // Draw if it's the current selection (or first slot on fresh window)
      if (it->index == OS_index || it->index == 0) {
        OS_showCurrent(/*force*/true);
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

    // Reboot to apply new Wi-Fi/host settings next time OTA/assets are needed
    esp_restart();
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

  // === ORDER STATION: debounced button handler (90ms), no delay() ===
  bool raw = digitalRead(BTN_PIN);
  if (raw != g_lastRaw) {            // edge detected
    g_lastRaw   = raw;
    g_lastEdgeMs= millis();
  }

  // accept state change only if stable for 90ms
  if (millis() - g_lastEdgeMs > 90) {
    if (raw != g_btn) {
      g_btn = raw;
      if (g_btn == LOW) {            // PRESS (active-low button)
        lampPulseStart(60);          // quick visible pulse, non-blocking

        if (OS_count == 0 || OS_loading) {
          Serial.println("[OS] press ignored (no items or still loading)");
        } else {
          if (OS_index >= OS_count) OS_index = 0;      // clamp if window shrank
          OS_index = (OS_index + 1) % OS_count;        // cycle 0..OS_count-1
          Serial.printf("[OS] PRESS -> index=%u of %u\n", OS_index, OS_count);
          OS_showCurrent(/*force*/true);
        }
      }
    }
  }

  // keep lamp pulse non-blocking
  lampPulseTick();

}
