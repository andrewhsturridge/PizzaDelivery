// Role: ORDERS_PANEL (MatrixPortal S3) – show text from Orders Node using the SAME path/font as HousePanels
#define PIZZA_ROLE ORDERS_PANEL
#define PIZZA_HOUSE_ID 0

#include <Arduino.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaPanel.h"
#include "PizzaOta.h"
#include "PizzaNetCfg.h"

static uint16_t g_seq = 1;
static uint32_t g_helloDueAt = 0;

// OTA deferral
static volatile bool g_otaPending = false;
static bool g_inOta = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

// Dedupe to avoid log/render spam
static char g_lastShown[PZ_ORDER_TEXT_MAX] = {0};

static void sendHello(){
  HelloPayload hp{}; strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION; PizzaIdentity::mac(hp.mac);
  uint8_t out[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n);
  PZ_LOGI("HELLO sent (orders-panel)");
}

// Same bottom progress bar behavior as HousePanel
static void panelOtaBottomBar(size_t written, size_t total){
  if (!total) return;
  uint8_t pct = (uint8_t)((written * 100ULL) / total);
  PizzaPanel::showBottomBarPercent(pct);
}

static bool matchOtaTarget(const OtaStartPayload* p){
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0;i<sizeof(p->ids);i++) if (p->ids[i] == PIZZA_HOUSE_ID) return true;
  return false;
}

// Signature matches PizzaNow::RxHandler
static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]){
  if (hdr.type == HELLO_REQ){ 
    uint32_t jitter = 50 + (esp_random() % 300);           // 50..350 ms
    g_helloDueAt = millis() + jitter + ((PIZZA_HOUSE_ID & 7) * 40);
    return;
   }

  if (hdr.type == ORDER_SHOW_TEXT && !g_inOta && len >= sizeof(PzOrderShowTextPayload)) {
    const auto* p = (const PzOrderShowTextPayload*)payload;

    // De-dupe: only redraw/log when the text actually changes
    if (strncmp(g_lastShown, p->text, sizeof(g_lastShown)) != 0) {
      strlcpy(g_lastShown, p->text, sizeof(g_lastShown));
      PizzaPanel::setWeight(0);  // 1 is subtle bold
      // EXACT look as HousePanels: style=0 (scroll), speed=2, bright=100
      PizzaPanel::showText(p->text, /*style*/2, /*speed*/2, /*bright*/100);
      PZ_LOGI("SHOW_TEXT: \"%s\"", p->text);
    }
    return;
  }

  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)){
    const auto* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;

    // ACK fast so Central proceeds
    OtaAckPayload ack{}; ack.accept=1; ack.code=0;
    uint8_t out[64];
    size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &ack, sizeof(ack), out, sizeof(out));
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
    Serial.printf("[OrdersPanel] NET_CFG_SET: ssid=\"%s\" base=\"%s\" save=%s\n",
                  v.ssid, v.base, ok ? "OK" : "FAIL");

    // Reboot to apply next time OTA/assets are needed
    esp_restart();
    return;
  }

}

void setup(){
  Serial.begin(115200); delay(100);
  PZ_LOGI("OrdersPanel boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // Initialize panel EXACTLY like HousePanels
  if (!PizzaPanel::begin64x32(/*brightness*/100)) {
    PZ_LOGE("Panel init failed");
  }

  PizzaPanel::setColor(0, 160, 255);  // **blue** (RGB)
  
  // Boot banner via the same helper
  PizzaPanel::showText("BOOT", /*style*/0, /*speed*/2, /*bright*/100);

  PizzaOta::setProgressCallback(panelOtaBottomBar);

  if (!PizzaNow::begin(ESPNOW_CHANNEL)) {
    PZ_LOGE("ESPNOW init failed");
  }
  PizzaNow::onReceive(onRx);

  sendHello();
}

void loop(){
  PizzaNow::loop();

  if (g_helloDueAt && (int32_t)(millis() - g_helloDueAt) >= 0) {
    sendHello();
    g_helloDueAt = 0;
  }

  if (!g_inOta) {
    // IMPORTANT: drives scroll/animation inside PizzaPanel
    PizzaPanel::loop();
  }

  // Deferred OTA (same flow as HousePanels)
  if (g_otaPending){
    noInterrupts(); bool run = g_otaPending; g_otaPending=false; interrupts();
    if (run){
      g_inOta = true;
      PizzaPanel::progressBarReset();
      PizzaPanel::showBottomBarPercent(0);

      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK){
        OtaResultPayload rr{}; rr.ok=0; rr.code=(uint8_t)res;
        uint8_t out[64];
        size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
        PizzaPanel::showText("OTA FAIL", 1, 1, 120);
      }
      // success path: device reboots after OTA
    }
  }
}
