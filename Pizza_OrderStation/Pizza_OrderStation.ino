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

// OTA deferral
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

// OTA progress state (updated by callback, rendered in loop)
static volatile size_t otaDone  = 0;
static volatile size_t otaTotal = 0;
static volatile bool   otaActive = false;

///// Orders list state (ORDERS_NODE) /////
struct LocalOrder {
  bool     used;
  uint8_t  house_id;
  uint8_t  mask;
  char     text[120];
};

static LocalOrder g_olist[6];
static uint8_t    g_oExpected = 0;     // how many items Central said we'll get
static uint8_t    g_oHave     = 0;     // how many unique indices we’ve stored
static bool       g_oReady    = false; // list complete
static int8_t     g_oCur      = -1;    // current shown index, -1 if none
static uint16_t   g_oResetSeq = 0;     // seq of last ORDER_LIST_RESET we accepted

///// assembly watchdog /////
static uint32_t g_oResetAtMs = 0;
static const uint16_t ORDER_ASSEMBLY_TIMEOUT_MS = 500; // ms; tune 300–800 if you like

// ---------- helpers ----------
static void sendHello(){
  HelloPayload hp{}; strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION; PizzaIdentity::mac(hp.mac);
  uint8_t out[128]; size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n); PZ_LOGI("HELLO sent (orders-node)");
}

static void showNoOrders() {
  ordersSendShowText_Node(nullptr);
}

// Panel bridge: send ORDER_SHOW_TEXT to Orders Panel(s)
static void ordersSendShowText_Node(const char* s) {
  PzOrderShowTextPayload p{};
  if (s && *s) strlcpy(p.text, s, sizeof(p.text));
  else         strcpy(p.text, "NO ORDERS");
  uint8_t out[192];
  size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, ORDERS_NODE, 0, g_seq++, &p, sizeof(p), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n);
}

// Clear local list
static void ordersLocalClear(){
  for (auto &it : g_olist) it = LocalOrder{};
  g_oExpected = 0; g_oHave = 0; g_oReady = false; g_oCur = -1;
}

// Show by index
static void ordersShowIndex(int idx){
  if (idx < 0 || idx >= (int)g_oExpected || !g_olist[idx].used) return;
  g_oCur = idx;
  ordersSendShowText_Node(g_olist[idx].text);
}

// Debounced button edge (call this every loop with your raw button level)
static bool ordersButtonEdge(bool pressed){
  static uint8_t  st=0, last=0;
  static uint32_t tStable=0;
  if (pressed != last){ last = pressed; tStable = millis(); }
  if ((int32_t)(millis()-tStable) < 20) return false;  // 20ms debounce
  if (st != pressed){ st = pressed; if (st) return true; } // rising edge
  return false;
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

  if (hdr.type == ORDER_LIST_RESET && len >= sizeof(PzOrderListResetPayload)) {
    const PzOrderListResetPayload* r = (const PzOrderListResetPayload*)payload;

    ordersLocalClear();
    g_oExpected = (r->count > 6) ? 6 : r->count;
    g_oResetSeq = hdr.seq;
    g_oResetAtMs = millis();       // <-- start assembly window
    PZ_LOGI("ORDERS reset: expect=%u seq=%u", g_oExpected, g_oResetSeq);

    if (g_oExpected == 0) {
      ordersSendShowText_Node(nullptr); // "NO ORDERS"
    }
    return;
  }

  if (hdr.type == ORDER_ITEM_SET && len >= sizeof(PzOrderItemSetPayload)) {
    const PzOrderItemSetPayload* it = (const PzOrderItemSetPayload*)payload;

    // Ignore items if we haven't seen a RESET yet
    if (g_oExpected == 0 && g_oResetSeq == 0) return;

    // (Optional) ignore items clearly older than most recent RESET (if you use seq wrap, drop this)
    if (hdr.seq < g_oResetSeq) {
      PZ_LOGI("ORDERS item ignored (older seq)");
      return;
    }

    if (it->index >= g_oExpected) {
      PZ_LOGI("ORDERS item out of range idx=%u >= %u", it->index, g_oExpected);
      return;
    }

    LocalOrder &slot = g_olist[it->index];
    if (!slot.used) {
      g_oHave++;
      slot.used     = true;
      slot.house_id = it->house_id;
      slot.mask     = it->mask;
      memset(slot.text, 0, sizeof(slot.text));
      strlcpy(slot.text, it->text, sizeof(slot.text));
    } else {
      // refresh (rare)
      slot.house_id = it->house_id;
      slot.mask     = it->mask;
      strlcpy(slot.text, it->text, sizeof(slot.text));
    }

    // When complete, mark ready and show the first valid index exactly once
    if (!g_oReady && g_oHave >= g_oExpected) {
      g_oReady = true;
      // find the first used index
      int first = -1;
      for (uint8_t i=0;i<g_oExpected;i++) if (g_olist[i].used){ first = i; break; }
      if (first >= 0) ordersShowIndex(first);
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

  // Adjust read for your wiring: LOW if active-low button, HIGH if active-high
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  // Watchdog: if still assembling after timeout, finalize with what we have
  if (g_oExpected > 0 && !g_oReady) {
    if ((int32_t)(millis() - g_oResetAtMs) > (int32_t)ORDER_ASSEMBLY_TIMEOUT_MS) {
      g_oReady = true;
      // If we got something, show first; else show NO ORDERS (rare)
      if (g_oHave > 0) {
        int first = -1;
        for (uint8_t i=0;i<g_oExpected;i++) if (g_olist[i].used){ first = i; break; }
        if (first >= 0) ordersShowIndex(first);
      } else {
        ordersSendShowText_Node(nullptr);
      }
    }
  }

  if (ordersButtonEdge(pressed)) {
    digitalWrite(LAMP_PIN, LOW);
    delay(60);
    digitalWrite(LAMP_PIN, HIGH);
    if (g_oReady && g_oExpected > 0) {
      // normal cycling among used slots
      for (uint8_t step=1; step<=g_oExpected; ++step) {
        int idx = ( (g_oCur < 0 ? 0 : g_oCur + step) % g_oExpected );
        if (g_olist[idx].used) { ordersShowIndex(idx); break; }
      }
    } else {
      // Not "ready" yet — graceful fallback:
      if (g_oHave > 0) {
        // show the first item we already have
        int first = -1;
        for (uint8_t i=0;i<g_oExpected;i++) if (g_olist[i].used){ first = i; break; }
        if (first >= 0) ordersShowIndex(first);
      } else if (g_oExpected == 0) {
        ordersSendShowText_Node(nullptr);
      }
    }
  }

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
}
