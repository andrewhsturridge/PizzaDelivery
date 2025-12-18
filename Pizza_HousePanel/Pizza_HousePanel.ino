// Role: HOUSE_PANEL (MatrixPortal S3)
// One binary for all panels; house_id is claimed once and saved to NVS.

#define PIZZA_ROLE HOUSE_PANEL

#include <Arduino.h>
#include <ctype.h>
#include <Preferences.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaPanel.h"
#include "PizzaOta.h"
#include "PizzaNetCfg.h"

static uint16_t g_seq = 1;

// --- Persistent house_id (NVS) ---
Preferences prefs;
static uint8_t g_houseId = 0; // 0 = unclaimed
static uint32_t g_helloDueAt = 0;

// --- OTA deferral ---
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

static bool g_inOta = false;

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
    snprintf(msg, sizeof(msg), "House %u", (unsigned)g_houseId);
    PizzaPanel::showText(msg, /*style*/1, /*speed*/1, /*bright*/120);
  }
}


// -----------------------------
// Icon rendering (Panel T2)
// We treat any text that starts with '@' as an icon token.
// Example tokens: "@STAR", "@HEART", "@KEY".
// -----------------------------

static bool isIconText(const char* s) {
  return s && s[0] == '@' && s[1] != '\0';
}

static bool tokenEq(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = toupper((unsigned char)*a++);
    char cb = toupper((unsigned char)*b++);
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static void drawIcon(const char* tok) {
  // 64x32 coordinate system
  auto& g = PizzaPanel::gfx();
  const uint16_t WHITE = 0xFFFF;

  // Icon box
  const int cx = 32;
  const int cy = 16;
  const int r  = 10;

  // Normalize token (skip leading '@')
  const char* t = tok;
  if (t && t[0] == '@') t++;

  // Simple primitives; readable from across the room.
  if (tokenEq(t, "STAR")) {
    // Asterisk-like star
    g.drawLine(cx-r, cy, cx+r, cy, WHITE);
    g.drawLine(cx, cy-r, cx, cy+r, WHITE);
    g.drawLine(cx-r, cy-r, cx+r, cy+r, WHITE);
    g.drawLine(cx-r, cy+r, cx+r, cy-r, WHITE);
  }
  else if (tokenEq(t, "HEART")) {
    g.fillCircle(cx-5, cy-3, 5, WHITE);
    g.fillCircle(cx+5, cy-3, 5, WHITE);
    g.fillTriangle(cx-10, cy-1, cx+10, cy-1, cx, cy+12, WHITE);
  }
  else if (tokenEq(t, "CROWN")) {
    g.drawRect(cx-14, cy+4, 28, 6, WHITE);
    g.fillTriangle(cx-14, cy+4, cx-6, cy-6, cx+2, cy+4, WHITE);
    g.fillTriangle(cx-2,  cy+4, cx+6, cy-8, cx+14, cy+4, WHITE);
    g.fillTriangle(cx+2,  cy+4, cx+10, cy-6, cx+18, cy+4, WHITE);
  }
  else if (tokenEq(t, "BOLT") || tokenEq(t, "LIGHTNING")) {
    // Zig-zag lightning
    g.drawLine(cx-4, cy-12, cx+2, cy-2, WHITE);
    g.drawLine(cx+2, cy-2,  cx-2, cy-2, WHITE);
    g.drawLine(cx-2, cy-2,  cx+4, cy+12, WHITE);
    g.drawLine(cx+4, cy+12, cx-1, cy+4, WHITE);
  }
  else if (tokenEq(t, "SKULL")) {
    g.drawCircle(cx, cy-2, 10, WHITE);
    g.fillCircle(cx-4, cy-4, 2, WHITE);
    g.fillCircle(cx+4, cy-4, 2, WHITE);
    g.drawRect(cx-6, cy+5, 12, 5, WHITE);
    g.drawLine(cx-2, cy+5, cx-2, cy+10, WHITE);
    g.drawLine(cx+2, cy+5, cx+2, cy+10, WHITE);
  }
  else if (tokenEq(t, "KEY")) {
    g.drawCircle(cx-6, cy-2, 5, WHITE);
    g.drawLine(cx-1, cy-2, cx+12, cy-2, WHITE);
    g.drawLine(cx+6, cy-2, cx+6, cy+4, WHITE);
    g.drawLine(cx+10, cy-2, cx+10, cy+2, WHITE);
  }
  else if (tokenEq(t, "SMILE")) {
    g.drawCircle(cx, cy-2, 11, WHITE);
    g.fillCircle(cx-4, cy-6, 1, WHITE);
    g.fillCircle(cx+4, cy-6, 1, WHITE);
    g.drawLine(cx-5, cy+2, cx+5, cy+2, WHITE);
    g.drawLine(cx-4, cy+3, cx+4, cy+3, WHITE);
  }
  else if (tokenEq(t, "FROWN")) {
    g.drawCircle(cx, cy-2, 11, WHITE);
    g.fillCircle(cx-4, cy-6, 1, WHITE);
    g.fillCircle(cx+4, cy-6, 1, WHITE);
    g.drawLine(cx-5, cy+4, cx+5, cy+4, WHITE);
    g.drawLine(cx-4, cy+3, cx+4, cy+3, WHITE);
  }
  else if (tokenEq(t, "UP")) {
    g.fillTriangle(cx, cy-12, cx-10, cy, cx+10, cy, WHITE);
    g.drawLine(cx, cy, cx, cy+10, WHITE);
  }
  else if (tokenEq(t, "DOWN")) {
    g.fillTriangle(cx-10, cy-2, cx+10, cy-2, cx, cy+10, WHITE);
    g.drawLine(cx, cy-12, cx, cy-2, WHITE);
  }
  else if (tokenEq(t, "CHECK")) {
    g.drawLine(cx-10, cy, cx-2, cy+8, WHITE);
    g.drawLine(cx-2,  cy+8, cx+12, cy-8, WHITE);
    g.drawLine(cx-10, cy+1, cx-2, cy+9, WHITE);
    g.drawLine(cx-2,  cy+9, cx+12, cy-7, WHITE);
  }
  else if (tokenEq(t, "X")) {
    g.drawLine(cx-10, cy-10, cx+10, cy+10, WHITE);
    g.drawLine(cx-10, cy+10, cx+10, cy-10, WHITE);
  }
  else {
    // Unknown icon token -> show as text (fallback)
    // (We do nothing here; caller will render as text.)
  }
}

static void showIconText(const char* iconTok, uint8_t bright) {
  // Stop any previous scroll, set brightness, clear, draw icon, show.
  PizzaPanel::setColor(255,255,255);
  PizzaPanel::showText("", /*style*/1, /*speed*/1, bright);
  auto& g = PizzaPanel::gfx();
  g.fillScreen(0);
  drawIcon(iconTok);
  PizzaPanel::show();
}

static void renderPanelText(const char* text, uint8_t style, uint8_t speed, uint8_t bright) {
  if (isIconText(text)) {
    // If token is unknown, drawIcon() draws nothing; fall back to text in that case.
    const char* t = text;
    if (t && t[0]=='@') {
      // quick known-token check
      const char* name = t + 1;
      const char* known[] = {"STAR","HEART","CROWN","BOLT","LIGHTNING","SKULL","KEY","SMILE","FROWN","UP","DOWN","CHECK","X"};
      bool ok=false;
      for (auto k: known) { if (tokenEq(name, k)) { ok=true; break; } }
      if (ok) {
        showIconText(text, bright);
        return;
      }
    }
  }

  // Default text rendering
  PizzaPanel::showText(text ? text : "", style, speed, bright);
}

static bool matchOtaTarget(const OtaStartPayload* p) {
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0; i<sizeof(p->ids); i++) if (p->ids[i] == g_houseId) return true;
  return false;
}

static void panelOtaBottomBar(size_t written, size_t total) {
  // PizzaOta calls s_cb(1,1) right before reboot on success → treat as 100%
  uint8_t pct;
  if (total == 1 && written == 1) {
    pct = 100;
  } else if (total > 0) {
    pct = (uint8_t)((written * 100ULL) / total);
  } else {
    // Unknown size: we don't update mid-download; just show 0% at start
    return;
  }

  // Snap to 20% milestones (0,20,40,60,80,100)
  static uint8_t lastStep = 255;
  uint8_t step = pct / 20;             // 0..5
  if (step == lastStep) return;
  lastStep = step;

  PizzaPanel::showBottomBarPercent(step * 20);
}

// --- RX handler ---
static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) { 
    uint32_t jitter = 50 + (esp_random() % 300);           // 50..350 ms
    g_helloDueAt = millis() + jitter + ((PIZZA_HOUSE_ID & 7) * 40);
    return;
   }

  if (hdr.type == PANEL_TEXT && !g_inOta && len >= sizeof(PanelTextPayload)) {
    const PanelTextPayload* p = (const PanelTextPayload*)payload;
    if (p->house_id == g_houseId) {
      PZ_LOGI("PANEL_TEXT: id=%u \"%s\" style=%u speed=%u bright=%u",
              p->house_id, p->text, p->style, p->speed, p->bright);
      renderPanelText(p->text, p->style, p->speed, p->bright);
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

  if (hdr.type == HOUSE_DIGITAL_SET && len >= sizeof(HouseDigitalSetPayload)) {
    const HouseDigitalSetPayload* p = (const HouseDigitalSetPayload*)payload;
    if (p->house_id != g_houseId) return;

    if (p->flags & 0x02) {
      if (p->panel_mode == PANEL_MODE_TEXT) {
        renderPanelText(p->panel_text, p->panel_style, p->panel_speed, p->panel_bright);
      } else if (p->panel_mode == PANEL_MODE_NUMBER) {
        // numbers as static text (could add a big-digits mode later)
        PizzaPanel::showText(p->panel_text, /*style*/1, /*speed*/1, p->panel_bright);
      } else if (p->panel_mode == PANEL_MODE_LETTER) {
        PizzaPanel::showText(p->panel_text, /*style*/1, /*speed*/1, p->panel_bright);
      }
    }
    return;
  }

  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)) {
    const OtaStartPayload* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;

    // ACK fast
    OtaAckPayload ack{}; ack.accept = 1; ack.code = 0;
    uint8_t out[64];
    size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, g_houseId, g_seq++, &ack, sizeof(ack), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);

    // Post the job
    strlcpy(g_otaUrl, p->url, sizeof(g_otaUrl));
    strlcpy(g_otaVer, p->ver, sizeof(g_otaVer));
    g_otaPending = true;
    PZ_LOGI("OTA queued: %s", g_otaUrl);
    return;
  }

  if (hdr.type == NET_CFG_SET && len >= sizeof(NetCfgSetPayload)) {
    const NetCfgSetPayload* p = (const NetCfgSetPayload*)payload;

    NetCfg::Value v{};
    strlcpy(v.ssid, p->ssid, sizeof(v.ssid));
    strlcpy(v.pass, p->pass, sizeof(v.pass));
    strlcpy(v.base, p->base, sizeof(v.base));

    bool ok = NetCfg::save(v);
    Serial.printf("[HousePanel] NET_CFG_SET: ssid=\"%s\" base=\"%s\" save=%s\n",
                  v.ssid, v.base, ok ? "OK" : "FAIL");

    // Simple: reboot to apply new Wi-Fi/host settings next time OTA/assets are needed
    esp_restart();
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

  PizzaOta::setProgressCallback(panelOtaBottomBar);

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

  if (g_helloDueAt && (int32_t)(millis() - g_helloDueAt) >= 0) {
    sendHello();
    g_helloDueAt = 0;
  }

  if (!g_inOta) {
    PizzaPanel::loop();                  // will early-return unless style==0
  }

  if (g_otaPending) {
    noInterrupts(); bool run = g_otaPending; g_otaPending = false; interrupts();
    if (run) {
      g_inOta = true;

      // Start with a clean row & 0% bar, then do OTA
      PizzaPanel::progressBarReset();
      PizzaPanel::showBottomBarPercent(0);

      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);

      if (res != PizzaOta::OK) {
        OtaResultPayload rr{}; rr.ok = 0; rr.code = (uint8_t)res;
        uint8_t out[64];
        size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, g_houseId, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
        PizzaPanel::showText("OTA FAIL", 1, 1, 120);  // one static frame after OTA ends
      }
      g_inOta = false; // won't run if we rebooted on success
    }
  }
}
