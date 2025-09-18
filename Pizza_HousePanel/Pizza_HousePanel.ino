// Role: HOUSE_PANEL (MatrixPortal S3)
// Listens for PANEL_TEXT and displays it
#define PIZZA_ROLE HOUSE_PANEL
#define PIZZA_HOUSE_ID 1
#define PIZZA_ENABLE_PANEL_MODULE

#include <Arduino.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaPanel.h"

static uint16_t g_seq = 1;

static void sendHello() {
  HelloPayload hp{};
  strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION;
  PizzaIdentity::mac(hp.mac);

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, PIZZA_HOUSE_ID, g_seq++, &hp, sizeof(hp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO sent (panel) id=%u", (unsigned)PIZZA_HOUSE_ID);
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) {
    // one-shot reply (no backoff needed here on bench)
    sendHello();
    return;
  }

  if (hdr.type == PANEL_TEXT && len >= sizeof(PanelTextPayload)) {
    const PanelTextPayload* p = (const PanelTextPayload*)payload;
    if (p->house_id == PIZZA_HOUSE_ID) {
      PZ_LOGI("PANEL_TEXT: \"%s\" style=%u speed=%u bright=%u", p->text, p->style, p->speed, p->bright);
      PizzaPanel::showText(p->text, p->style, p->speed, p->bright);
    }
  }
}

void setup() {
  Serial.begin(115200); delay(50);
  PZ_LOGI("HousePanel boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // Matrix
  if (!PizzaPanel::begin64x32(/*brightness*/100)) {
    PZ_LOGE("Panel init failed");
  }

  // Radio
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);

  // Boot HELLO (once)
  sendHello();

  // Optional: show local boot text until first command
  PizzaPanel::showText("House 1 ONLINE", /*style*/1, /*speed*/1, /*bright*/100);
}

void loop() {
  PizzaNow::loop();
  PizzaPanel::loop();
}
