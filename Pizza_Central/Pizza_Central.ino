// Role: CENTRAL (Feather S3)
// Roster + clean DELIVER_SCAN → DELIVER_RESULT (auto-OK for now).
// CLI: list | hello-req | panel <id> "text" [style] [speed] [bright] | sound <id> [clip] [vol]
//      update <ROLE> all|id=<n> [url] | claim <MAC> <id> [force]

#define PIZZA_ROLE CENTRAL
#define PIZZA_HOUSE_ID 0

#include <Arduino.h>
#include <WiFi.h>
#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"

static uint16_t g_seq = 1;

// -------------------- ROSTER --------------------
struct DeviceInfo {
  uint8_t  mac[6];
  Role     role;
  uint8_t  house_id;
  char     fw[12];
  uint32_t last_seen_ms;
  bool     used;
};

static const uint8_t MAX_DEVICES = 24;
static DeviceInfo g_devices[MAX_DEVICES];

static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static int findSlotByMac(const uint8_t mac[6]) {
  for (int i=0;i<MAX_DEVICES;i++)
    if (g_devices[i].used && memcmp(g_devices[i].mac, mac, 6)==0) return i;
  return -1;
}

static int allocSlotForMac(const uint8_t mac[6]) {
  int idx = findSlotByMac(mac);
  if (idx >= 0) return idx;
  for (int i=0;i<MAX_DEVICES;i++) {
    if (!g_devices[i].used) {
      memset(&g_devices[i], 0, sizeof(DeviceInfo));
      memcpy(g_devices[i].mac, mac, 6);
      g_devices[i].used = true;
      return i;
    }
  }
  return -1; // full
}

static void rosterUpdateFromHello(const MsgHeader& hdr, const HelloPayload* h, const uint8_t srcMac[6]) {
  int idx = allocSlotForMac(srcMac);
  if (idx < 0) return;
  DeviceInfo& d = g_devices[idx];
  d.role = (Role)hdr.role;
  d.house_id = hdr.house_id;
  strlcpy(d.fw, h->fw, sizeof(d.fw));
  d.last_seen_ms = millis();
}

static void rosterPrint() {
  Serial.println(F("\nROLE          ID   FW        MAC                LAST_SEEN(ms)\n--------------------------------------------------------------"));
  for (int i=0;i<MAX_DEVICES;i++) if (g_devices[i].used) {
    char macbuf[18]; macToStr(g_devices[i].mac, macbuf);
    const char* rn = "UNKNOWN";
    switch (g_devices[i].role) {
      case HOUSE_PANEL: rn="HOUSE_PANEL"; break;
      case HOUSE_NODE:  rn="HOUSE_NODE "; break;
      case ORDERS_PANEL:rn="ORDERS_PANEL"; break;
      case ORDERS_NODE: rn="ORDERS_NODE "; break;
      case PIZZA_NODE:  rn="PIZZA_NODE "; break;
      case CENTRAL:     rn="CENTRAL    "; break;
    }
    Serial.printf("%-12s  %2u   %-8s  %-17s  %9u\n",
      rn, g_devices[i].house_id, g_devices[i].fw, macbuf, (unsigned)g_devices[i].last_seen_ms);
  }
  Serial.println();
}

// -------------------- SENDERS --------------------
static void sendHelloReq() {
  uint8_t buf[64];
  size_t n = PizzaProtocol::pack(HELLO_REQ, CENTRAL, 0, g_seq++, nullptr, 0, buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO_REQ broadcast");
}

static void sendPanelText(uint8_t houseId, const String& text, uint8_t style, uint8_t speed, uint8_t bright) {
  PanelTextPayload p{};
  p.house_id = houseId;
  strlcpy(p.text, text.c_str(), sizeof(p.text));
  p.style = style; p.speed = speed; p.bright = bright;
  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(PANEL_TEXT, CENTRAL, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("PANEL_TEXT -> house %u: \"%s\"", houseId, p.text);
}

static void sendSoundPlay(uint8_t houseId, uint8_t clipId, uint8_t vol) {
  SoundPlayPayload p{};
  p.house_id = houseId; p.clip_id = clipId; p.vol = vol;
  uint8_t buf[64];
  size_t n = PizzaProtocol::pack(SOUND_PLAY, CENTRAL, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("SOUND_PLAY -> house %u clip=%u vol=%u", houseId, clipId, vol);
}

static void sendDeliverResult(uint8_t houseId, bool ok, uint8_t reason) {
  DeliverResultPayload p{}; p.ok = ok ? 1 : 0; p.reason = reason;
  uint8_t buf[64];
  size_t n = PizzaProtocol::pack(DELIVER_RESULT, CENTRAL, houseId, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("DELIVER_RESULT -> house %u ok=%u reason=%u", houseId, p.ok, p.reason);
}

static Role parseRole(const String& s) {
  if (s=="HOUSE_PANEL") return HOUSE_PANEL;
  if (s=="HOUSE_NODE")  return HOUSE_NODE;
  if (s=="ORDERS_PANEL")return ORDERS_PANEL;
  if (s=="ORDERS_NODE") return ORDERS_NODE;
  if (s=="PIZZA_NODE")  return PIZZA_NODE;
  if (s=="CENTRAL")     return CENTRAL;
  return CENTRAL;
}

static bool parseMac(const String& s, uint8_t out[6]) {
  int b[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6) return false;
  for (int i=0;i<6;i++) out[i] = (uint8_t)b[i];
  return true;
}

static void sendOtaStart(Role target, bool all, uint8_t id, const char* url) {
  OtaStartPayload p{};
  p.target_role = (uint8_t)target;
  p.scope = all ? 0 : 1;
  memset(p.ids, 0, sizeof(p.ids));
  if (!all) p.ids[0] = id;
  strlcpy(p.url, url, sizeof(p.url));
  strlcpy(p.ver, FW_VERSION, sizeof(p.ver));
  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(OTA_START, CENTRAL, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("OTA_START role=%u scope=%s url=%s", (unsigned)target, all?"ALL":"LIST", p.url);
}

static void sendClaim(const uint8_t mac[6], uint8_t newId, bool force=false) {
  ClaimPayload cp{};
  memcpy(cp.target_mac, mac, 6);
  cp.house_id = newId;
  cp.force    = force ? 1 : 0;
  uint8_t buf[64];
  size_t n = PizzaProtocol::pack(CLAIM, CENTRAL, 0, g_seq++, &cp, sizeof(cp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  char macbuf[18]; macToStr(mac, macbuf);
  PZ_LOGI("CLAIM sent -> mac=%s id=%u force=%u", macbuf, cp.house_id, cp.force);
}

// -------------------- RX HANDLER --------------------
static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t srcMac[6]) {
  char macbuf[18]; macToStr(srcMac, macbuf);

  if (hdr.type == HELLO && len >= sizeof(HelloPayload)) {
    const HelloPayload* h = (const HelloPayload*)payload;
    rosterUpdateFromHello(hdr, h, srcMac);
    PZ_LOGI("HELLO role=%u id=%u fw=%s mac=%s", hdr.role, hdr.house_id, h->fw, macbuf);
    return;
  }

  if (hdr.type == DELIVER_SCAN && len >= sizeof(DeliverScanPayload)) {
    const DeliverScanPayload* s = (const DeliverScanPayload*)payload;
    PZ_LOGI("SCAN mac=%s house=%u uidLen=%u", macbuf, s->house_id, s->uid_len);

    // TODO: real validation later. For now: always OK.
    sendDeliverResult(s->house_id, /*ok*/true, /*reason*/0);

    // Optional: nudge panel or play sound (commented out for now)
    // sendPanelText(s->house_id, "Delivered!", 1, 1, 120);
    // sendSoundPlay(s->house_id, 1, 220);
    return;
  }

  if (hdr.type == OTA_ACK && len >= sizeof(OtaAckPayload)) {
    const OtaAckPayload* a = (const OtaAckPayload*)payload;
    PZ_LOGI("OTA_ACK from role=%u id=%u accept=%u code=%u", hdr.role, hdr.house_id, a->accept, a->code);
    return;
  }

  if (hdr.type == OTA_RESULT && len >= sizeof(OtaResultPayload)) {
    const OtaResultPayload* r = (const OtaResultPayload*)payload;
    PZ_LOGI("OTA_RESULT from role=%u id=%u ok=%u code=%u", hdr.role, hdr.house_id, r->ok, r->code);
    return;
  }
}

// -------------------- CLI --------------------
static String readLine() {
  static String s;
  while (Serial.available()) {
    char c = Serial.read(); if (c=='\r') continue;
    if (c=='\n') { String out=s; s=""; return out; }
    s += c;
  }
  return String();
}

static int parseInt(const String& s, int def=0){ char* e=nullptr; long v=strtol(s.c_str(), &e, 10); return (e && *e==0)? (int)v : def; }

void setup() {
  Serial.begin(115200); delay(100);
  PZ_LOGI("Central boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // Radio
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);

  // Help
  Serial.println(F("CLI:\n  list\n  hello-req\n  panel <id> \"text\" [style] [speed] [bright]\n  sound <id> [clip] [vol]\n  update <ROLE> all|id=<n> [url]\n  claim <MAC> <id> [force]\n"));
}

void loop() {
  PizzaNow::loop();

  String line = readLine();
  if (!line.length()) return;
  line.trim();

  if (line == "list") { rosterPrint(); return; }
  if (line == "hello-req") { sendHelloReq(); return; }

  if (line.startsWith("panel ")) {
    // panel <id> "text" [style] [speed] [bright]
    int s1=line.indexOf(' '); if (s1<0) return;
    int s2=line.indexOf(' ', s1+1);
    String idStr=line.substring(s1+1, s2>0?s2:line.length());
    int id=parseInt(idStr,1);
    int q1=line.indexOf('"', s2), q2=line.indexOf('"', q1+1);
    String txt=(q1>=0 && q2>q1)? line.substring(q1+1,q2): String("TEST");
    String rest=(q2>0)? line.substring(q2+1):""; rest.trim();
    int style=1,speed=1,bright=120;
    if (rest.length()){
      int sp1=rest.indexOf(' '); String sStyle=sp1>=0?rest.substring(0,sp1):rest; style=parseInt(sStyle,1);
      if (sp1>=0){ String r2=rest.substring(sp1+1); r2.trim(); int sp2=r2.indexOf(' ');
        String sSpeed=sp2>=0? r2.substring(0,sp2):r2; speed=parseInt(sSpeed,1);
        if (sp2>=0){ String sBright=r2.substring(sp2+1); sBright.trim(); bright=parseInt(sBright,120); }
      }
    }
    sendPanelText((uint8_t)id, txt, (uint8_t)style, (uint8_t)speed, (uint8_t)bright);
    return;
  }

  if (line.startsWith("sound ")) {
    // sound <id> [clip] [vol]
    int s1 = line.indexOf(' '); if (s1<0) return;
    String rest = line.substring(s1+1); rest.trim();
    int sp1 = rest.indexOf(' ');
    String idStr = sp1>=0 ? rest.substring(0,sp1) : rest;
    int id = idStr.length()? idStr.toInt() : 1;
    int clip = 1, vol = 200;
    if (sp1>=0) {
      String r2 = rest.substring(sp1+1); r2.trim();
      int sp2 = r2.indexOf(' ');
      if (sp2>=0) { clip = r2.substring(0,sp2).toInt(); vol = r2.substring(sp2+1).toInt(); }
      else { clip = r2.toInt(); }
    }
    sendSoundPlay((uint8_t)id, (uint8_t)clip, (uint8_t)vol);
    return;
  }

  if (line.startsWith("update ")) {
    // update <ROLE> all|id=<n> [url]
    int s1=line.indexOf(' '); if (s1<0) return;
    int s2=line.indexOf(' ', s1+1);
    String roleStr=line.substring(s1+1, s2>0?s2:line.length()); roleStr.trim();
    Role role=parseRole(roleStr);
    String target=(s2>0)? line.substring(s2+1) : "all"; target.trim();
    bool all = target.startsWith("all"); int id=0;
    const char* defUrl=nullptr;
    if (role==HOUSE_PANEL) defUrl = OTA_BASE_URL OTA_REL_HOUSE_PANEL;
    else if (role==HOUSE_NODE) defUrl = OTA_BASE_URL OTA_REL_HOUSE_NODE;
    else if (role==ORDERS_PANEL) defUrl = OTA_BASE_URL OTA_REL_ORDERS_PANEL;
    else if (role==ORDERS_NODE) defUrl = OTA_BASE_URL OTA_REL_ORDERS_NODE;
    else if (role==PIZZA_NODE) defUrl = OTA_BASE_URL OTA_REL_PIZZA_NODE;
    else if (role==CENTRAL) defUrl = OTA_BASE_URL OTA_REL_CENTRAL;

    String urlOverride;
    int sp=target.indexOf(' ');
    if (!all) {
      int eq=target.indexOf('='); if (eq>0) id=parseInt(target.substring(eq+1, sp>0?sp:target.length()),1);
      if (sp>0) urlOverride = target.substring(sp+1);
    } else {
      if (sp>0) urlOverride = target.substring(sp+1);
    }
    urlOverride.trim();
    String url = urlOverride.length()? urlOverride : String(defUrl);
    sendOtaStart(role, all, (uint8_t)id, url.c_str());
    return;
  }

  if (line.startsWith("claim ")) {
    // claim AA:BB:CC:DD:EE:FF 3 [force]
    int s1=line.indexOf(' '); if (s1<0) return;
    String rest=line.substring(s1+1); rest.trim();
    int s2=rest.indexOf(' ');
    if (s2<0){ Serial.println(F("usage: claim <MAC> <id> [force]")); return; }
    String macStr=rest.substring(0,s2);
    String rest2=rest.substring(s2+1); rest2.trim();
    int s3=rest2.indexOf(' ');
    String idStr = (s3>=0)? rest2.substring(0,s3) : rest2;
    int id = idStr.toInt();
    bool force = (s3>=0) ? (rest2.substring(s3+1)=="force") : false;

    uint8_t mac[6];
    if (!parseMac(macStr, mac) || id<=0) { Serial.println(F("usage: claim <MAC> <id> [force]")); return; }
    sendClaim(mac, (uint8_t)id, force);
    return;
  }
}
