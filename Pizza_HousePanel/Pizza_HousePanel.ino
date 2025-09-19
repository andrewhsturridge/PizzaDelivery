// Role: HOUSE_PANEL (MatrixPortal S3)
// One binary for all panels; house_id is claimed once and saved to NVS.

#define PIZZA_ROLE HOUSE_PANEL

#include <Arduino.h>
#include <Preferences.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaPanel.h"

static uint16_t g_seq = 1;

// --- Persistent house_id (NVS) ---
Preferences prefs;
static uint8_t g_houseId = 0; // 0 = unclaimed

static void cfgLoad() {
  prefs.begin("pizza", false);
  g_houseId = prefs.getUChar("house_id", 0);
  prefs.end();
}
static void cfgSaveHouseId(uint8_t id) {
  prefs.begin("pizza", false);
  prefs.putUChar("house_id", id);
  prefs.end();
  g_houseId = id;
}

// --- Messaging helpers ---
static void sendHello() {
  HelloPayload hp{};
  strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION;
  PizzaIdentity::mac(hp.mac);

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, g_houseId, g_seq++,
                                 &hp, sizeof(hp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO sent (panel) id=%u", (unsigned)g_houseId);
}

static void showStatus() {
  if (g_houseId == 0) {
    PizzaPanel::showText("UNCLAIMED - RUN claim", /*style*/1, /*speed*/1, /*bright*/120);
  } else {
    char msg[32];
    snprintf(msg, sizeof(msg), "House %u ONLINE", (unsigned)g_houseId);
    PizzaPanel::showText(msg, /*style*/1, /*speed*/1, /*bright*/120);
  }
}

// --- RX handler ---
static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) { sendHello(); return; }

  if (hdr.type == PANEL_TEXT && len >= sizeof(PanelTextPayload)) {
    const PanelTextPayload* p = (const PanelTextPayload*)payload;
    if (p->house_id == g_houseId) {
      PZ_LOGI("PANEL_TEXT: id=%u \"%s\" style=%u speed=%u bright=%u",
              p->house_id, p->text, p->style, p->speed, p->bright);
      PizzaPanel::showText(p->text, p->style, p->speed, p->bright);
    }
    return;
  }

  if (hdr.type == CLAIM && len >= sizeof(ClaimPayload)) {
    const ClaimPayload* cp = (const ClaimPayload*)payload;
    uint8_t my[6]; PizzaIdentity::mac(my);
    bool macMatch = (memcmp(my, cp->target_mac, 6) == 0);
    if (macMatch && (g_houseId == 0 || cp->force)) {
      PZ_LOGI("CLAIM: set house_id=%u (force=%u)", cp->house_id, cp->force);
      cfgSaveHouseId(cp->house_id);
      delay(50);
      ESP.restart();
    }
    return;
  }
}

void setup() {
  Serial.begin(115200); delay(100);
  PZ_LOGI("HousePanel boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  cfgLoad();

  // Panel hardware
  if (!PizzaPanel::begin64x32(/*brightness*/100)) {
    PZ_LOGE("Panel init failed");
  }
  showStatus(); // "UNCLAIMED" or "House N ONLINE"

  // Radio
  if (!PizzaNow::begin(ESPNOW_CHANNEL)) {
    PZ_LOGE("ESPNOW init failed");
  }
  PizzaNow::onReceive(onRx);

  // Say hello once at boot
  sendHello();
}

void loop() {
  PizzaNow::loop();
  PizzaPanel::loop(); // animates scroll if style==0
}
