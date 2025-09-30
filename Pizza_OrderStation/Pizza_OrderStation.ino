// Role: ORDERS_NODE (ESP32-WROOM) – single button cycles orders; shows text on panel
#define PIZZA_ROLE ORDERS_NODE
#define PIZZA_HOUSE_ID 0

#include <Arduino.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaOta.h"

#define BTN_PIN  13
#define LAMP_PIN 27

static uint16_t g_seq = 1;

// Orders list
static uint8_t g_expected = 0, g_count = 0, g_index = 0;
static PzOrderItemSetPayload g_items[PZ_ORDERS_MAX];

// Debounce
static bool g_lastRaw = true, g_btn = true;
static uint32_t g_lastEdgeMs = 0;

// OTA deferral
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

static void sendHello(){
  HelloPayload hp{}; strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION; PizzaIdentity::mac(hp.mac);
  uint8_t out[128]; size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n); PZ_LOGI("HELLO sent (orders-node)");
}

static void sendShowText(const char* s){
  PzOrderShowTextPayload p{}; if (s) strlcpy(p.text, s, sizeof(p.text));
  uint8_t buf[256]; size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, (Role)PIZZA_ROLE, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  Serial.printf("[OrdersNode] ORDER_SHOW_TEXT len=%u\n", (unsigned)strlen(p.text));
}

static bool matchOtaTarget(const OtaStartPayload* p){
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0;i<sizeof(p->ids);i++) if (p->ids[i] == PIZZA_HOUSE_ID) return true;
  return false;
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]){
  if (hdr.type == HELLO_REQ){ sendHello(); return; }

  if (hdr.type == ORDER_LIST_RESET && len >= sizeof(PzOrderListResetPayload)){
    const auto* p = (const PzOrderListResetPayload*)payload;
    g_expected = min<uint8_t>(p->count, PZ_ORDERS_MAX); g_count = 0; g_index = 0;
    for (uint8_t i=0;i<PZ_ORDERS_MAX;i++){ g_items[i].index=i; g_items[i].house_id=0; g_items[i].mask=0; memset(g_items[i].text,0,sizeof(g_items[i].text)); }
    Serial.printf("[OrdersNode] RESET expect=%u\n", g_expected);
    return;
  }

  if (hdr.type == ORDER_ITEM_SET && len >= sizeof(PzOrderItemSetPayload)){
    const auto* p = (const PzOrderItemSetPayload*)payload;
    if (p->index < PZ_ORDERS_MAX && p->index < g_expected){
      g_items[p->index] = *p;
      uint8_t newCount=0; for (uint8_t i=0;i<g_expected;i++){ if (g_items[i].text[0]==0 && i>=g_count) break; newCount=i+1; }
      g_count = newCount;
      Serial.printf("[OrdersNode] ITEM idx=%u (count=%u/%u)\n", p->index, g_count, g_expected);
    }
    return;
  }

  // OTA_START
  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)){
    const auto* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;
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
  pinMode(BTN_PIN, INPUT_PULLUP); pinMode(LAMP_PIN, OUTPUT); digitalWrite(LAMP_PIN, HIGH);
  PizzaNow::begin(ESPNOW_CHANNEL); PizzaNow::onReceive(onRx);
  sendHello();
}

void loop(){
  PizzaNow::loop();

  // OTA (deferred)
  if (g_otaPending){
    noInterrupts(); bool run = g_otaPending; g_otaPending=false; interrupts();
    if (run){
      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK){
        OtaResultPayload rr{}; rr.ok=0; rr.code=(uint8_t)res;
        uint8_t out[64]; size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
      }
    }
  }

  // Debounced short press → next order
  bool raw = digitalRead(BTN_PIN);
  if (raw != g_lastRaw){ g_lastRaw=raw; g_lastEdgeMs=millis(); }
  if (millis() - g_lastEdgeMs > 35){
    if (raw != g_btn){
      g_btn = raw;
      if (g_btn == LOW){
        digitalWrite(LAMP_PIN, LOW); delay(60); digitalWrite(LAMP_PIN, HIGH);
        if (g_count > 0){ g_index = (g_index + 1) % g_count; sendShowText(g_items[g_index].text); }
        else            { sendShowText("NO ORDERS"); }
      }
    }
  }
}
