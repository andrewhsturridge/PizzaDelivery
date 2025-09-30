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

/*** Block A: GLOBAL STATE + HELPERS ***/
static const uint8_t MASK_NONE = 0xFF;      // "no order set"
static uint8_t g_orderMask[256];            // house_id -> target mask

// Small ring buffer of recent pizza tags we’ve seen (uid -> mask)
struct TagEntry { uint8_t len; uint8_t uid[10]; uint8_t mask; uint32_t ts; };
static TagEntry g_tags[32];
static uint8_t  g_tagCount = 0;

/*** CENTRAL: Orders storage ***/
static PzOrderItemSetPayload g_orders[PZ_ORDERS_MAX];
static uint8_t g_orderCount = 0;
static uint16_t g_seq_orders = 1;

static void ordersResetLocal() {
  g_orderCount = 0;
  for (uint8_t i=0;i<PZ_ORDERS_MAX;i++) {
    g_orders[i].index = i;
    g_orders[i].house_id = 0;
    g_orders[i].mask = 0;
    memset(g_orders[i].text, 0, sizeof(g_orders[i].text));
  }
}

static bool ordersAddLocal(uint8_t house_id, uint8_t mask, const char* text) {
  if (g_orderCount >= PZ_ORDERS_MAX) return false;
  PzOrderItemSetPayload &it = g_orders[g_orderCount];
  it.index = g_orderCount;
  it.house_id = house_id;
  it.mask = mask;
  memset(it.text, 0, sizeof(it.text));
  if (text && *text) strncpy(it.text, text, sizeof(it.text)-1);
  g_orderCount++;
  return true;
}


enum DeliverReason : uint8_t {
  DR_OK = 0,
  DR_UNKNOWN_PIZZA = 1,
  DR_NO_ORDER = 2,
  DR_WRONG_PIZZA = 3,
};

static void stateInit() {
  for (int i = 0; i < 256; ++i) g_orderMask[i] = MASK_NONE;
  g_tagCount = 0;
}

static int tagFind(const uint8_t* uid, uint8_t len) {
  for (uint8_t i = 0; i < g_tagCount; ++i) {
    if (g_tags[i].len == len && memcmp(g_tags[i].uid, uid, len) == 0) return i;
  }
  return -1;
}

static void tagUpsert(const uint8_t* uid, uint8_t len, uint8_t mask) {
  int idx = tagFind(uid, len);
  if (idx >= 0) {
    g_tags[idx].mask = mask;
    g_tags[idx].ts   = millis();
    return;
  }
  if (g_tagCount < (uint8_t)(sizeof(g_tags)/sizeof(g_tags[0]))) {
    idx = g_tagCount++;
  } else {
    // Evict oldest
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < g_tagCount; ++i) if (g_tags[i].ts < g_tags[oldest].ts) oldest = i;
    idx = oldest;
  }
  g_tags[idx].len  = len;
  memcpy(g_tags[idx].uid, uid, len);
  g_tags[idx].mask = mask;
  g_tags[idx].ts   = millis();
}

static void printMask(uint8_t m) {
  if (m == MASK_NONE) { Serial.print(F("(none)")); return; }
  Serial.print(F("0x")); if (m < 16) Serial.print('0'); Serial.print(m, HEX);
  Serial.print(F(" ["));
  Serial.print((m &  1) ? 'P' : '.'); // Pepperoni
  Serial.print((m &  2) ? 'M' : '.'); // Mushrooms
  Serial.print((m &  4) ? 'p' : '.'); // Peppers
  Serial.print((m &  8) ? 'i' : '.'); // Pineapple
  Serial.print((m & 16) ? 'H' : '.'); // Ham
  Serial.print(']');
}

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

/*** Central: send ingredient snapshot ***/
static void sendIngrSnapshot(const PizzaIngrSnapshotPayload& sp) {
  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(PIZZA_ING_SNAPSHOT, CENTRAL, 0, g_seq++, &sp, sizeof(sp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("ING_SNAPSHOT ok=%u mask=0x%02X", sp.ok, sp.mask);
}

/*** CENTRAL: send RESET and ITEM_SET packets (broadcast) ***/
static void ordersSendReset(uint8_t count) {
  PzOrderListResetPayload p{ count };
  uint8_t buf[64];
  size_t n = PizzaProtocol::pack(ORDER_LIST_RESET, CENTRAL, 0, g_seq_orders++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  Serial.printf("[Central] ORDER_LIST_RESET count=%u\n", count);
}

static void ordersSendItem(const PzOrderItemSetPayload& item) {
  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(ORDER_ITEM_SET, CENTRAL, 0, g_seq_orders++, &item, sizeof(item), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  Serial.printf("[Central] ORDER_ITEM_SET idx=%u H%u mask=0x%02X\n", item.index, item.house_id, item.mask);
}

/*** CENTRAL: push local list to the Orders Node(s) ***/
static void ordersPushAll() {
  ordersSendReset(g_orderCount);
  for (uint8_t i=0;i<g_orderCount;i++) ordersSendItem(g_orders[i]);
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

  /*** Block B: onRx – PIZZA_ING_UPDATE -> remember uid→mask ***/
  if (hdr.type == PIZZA_ING_UPDATE && len >= sizeof(PizzaIngrUpdatePayload)) {
    const PizzaIngrUpdatePayload* u = (const PizzaIngrUpdatePayload*)payload;
    Serial.printf("[Central] ING_UPDATE from id=%u mask=0x%02X\n", hdr.house_id, u->mask);
    // Payload is expected to provide u->uid (bytes), u->uid_len, u->mask
    tagUpsert(u->uid, u->uid_len, u->mask);
    return;
  }

  if (hdr.type == HELLO && len >= sizeof(HelloPayload)) {
    const HelloPayload* h = (const HelloPayload*)payload;
    rosterUpdateFromHello(hdr, h, srcMac);
    PZ_LOGI("HELLO role=%u id=%u fw=%s mac=%s", hdr.role, hdr.house_id, h->fw, macbuf);
    return;
  }

  /*** Block C: onRx – DELIVER_SCAN -> validate against order & cache ***/
  if (hdr.type == DELIVER_SCAN && len >= sizeof(DeliverScanPayload)) {
    const DeliverScanPayload* s = (const DeliverScanPayload*)payload;
    // Payload is expected to provide s->house_id, s->uid (bytes), s->uid_len
    uint8_t reason = DR_OK;
    uint8_t order  = g_orderMask[s->house_id];
    int idx = tagFind(s->uid, s->uid_len);

    if (idx < 0)                    reason = DR_UNKNOWN_PIZZA; // never saw this UID
    else if (order == MASK_NONE)    reason = DR_NO_ORDER;      // no order set for that house
    else if (g_tags[idx].mask != order) reason = DR_WRONG_PIZZA; // wrong toppings

    // Central tells the House if it’s ok + why
    sendDeliverResult(s->house_id, (reason == DR_OK), reason);
    return;
  }

  if (hdr.type == PIZZA_ING_UPDATE && len >= sizeof(PizzaIngrUpdatePayload)) {
    const PizzaIngrUpdatePayload* u = (const PizzaIngrUpdatePayload*)payload;
    PZ_LOGI("ING_UPDATE from id=%u mask=0x%02X (P=%d M=%d Pe=%d Pi=%d H=%d)",
            hdr.house_id, u->mask,
            !!(u->mask&1), !!(u->mask&2), !!(u->mask&4), !!(u->mask&8), !!(u->mask&16));
    // TODO: store uid->mask in a small map so deliveries can be validated later.
    return;
  }

  /*** Central: handle PIZZA_ING_QUERY -> broadcast snapshot ***/
  if (hdr.type == PIZZA_ING_QUERY && len >= sizeof(PizzaIngrQueryPayload)) {
    const PizzaIngrQueryPayload* q = (const PizzaIngrQueryPayload*)payload;

    PizzaIngrSnapshotPayload s{};
    s.uid_len = q->uid_len;
    memcpy(s.uid, q->uid, q->uid_len);

    // Look up in our in-memory cache (added earlier)
    int idx = tagFind(q->uid, q->uid_len);
    if (idx >= 0) { s.ok = 1; s.mask = g_tags[idx].mask; }
    else          { s.ok = 0; s.mask = 0; }

    sendIngrSnapshot(s);
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

  stateInit();

  // Help
  Serial.println(F(
    "CLI:\n"
    "  list\n"
    "  hello-req\n"
    "  panel <id> \"text\" [style] [speed] [bright]\n"
    "  sound <id> [clip] [vol]\n"
    "  update <ROLE> all|id=<n> [url]\n"
    "  claim <MAC> <id> [force]\n"
    "  order <id> <mask>\n"
    "  order show [id]\n"
    "  order clear <id>\n"
    "  tags\n"
  ));
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

  /*** Block E: CLI – order/tags ***/
  if (line.startsWith("order ")) {
    String rest = line.substring(6); rest.trim();

    if (rest.startsWith("show")) {
      rest = rest.substring(4); rest.trim();
      if (rest.length()) {
        int id = rest.toInt();
        if (id < 0 || id > 255) { Serial.println(F("usage: order show [id]")); return; }
        Serial.print(F("order[")); Serial.print(id); Serial.print(F("]=")); printMask(g_orderMask[id]); Serial.println();
      } else {
        for (int i = 0; i < 256; ++i) if (g_orderMask[i] != MASK_NONE) {
          Serial.print(F("order[")); Serial.print(i); Serial.print(F("]=")); printMask(g_orderMask[i]); Serial.println();
        }
      }
      return;
    }

    if (rest.startsWith("clear")) {
      rest = rest.substring(5); rest.trim();
      int id = rest.toInt();
      if (id < 0 || id > 255) { Serial.println(F("usage: order clear <id>")); return; }
      g_orderMask[id] = MASK_NONE;
      Serial.printf("order[%d] cleared\n", id);
      return;
    }

    // order <id> <mask>
    int space = rest.indexOf(' ');
    if (space < 0) { Serial.println(F("usage: order <id> <mask>  (mask: 0..31 or 0xNN)")); return; }
    int id = rest.substring(0, space).toInt();
    String mstr = rest.substring(space + 1); mstr.trim();
    long mask;
    if (mstr.startsWith("0x") || mstr.startsWith("0X")) mask = strtol(mstr.c_str(), nullptr, 16);
    else mask = mstr.toInt();
    if (id < 0 || id > 255 || mask < 0 || mask > 31) { Serial.println(F("usage: order <id> <mask>")); return; }
    g_orderMask[id] = (uint8_t)mask;
    Serial.print(F("order[")); Serial.print(id); Serial.print(F("]=")); printMask(g_orderMask[id]); Serial.println();
    return;
  }

  if (line == "tags") {
    Serial.printf("tags (%u)\n", g_tagCount);
    for (uint8_t i = 0; i < g_tagCount; ++i) {
      Serial.printf("#%u ", i);
      for (uint8_t k = 0; k < g_tags[i].len; ++k) {
        if (g_tags[i].uid[k] < 16) Serial.print('0');
        Serial.print(g_tags[i].uid[k], HEX);
        if (k + 1 < g_tags[i].len) Serial.print(':');
      }
      Serial.print(F(" → ")); printMask(g_tags[i].mask); Serial.println();
    }
    return;
  }

  /*** CENTRAL CLI: orders ... ***/
  if (line.startsWith("orders ")) {
    String rest = line.substring(7); rest.trim();

    if (rest == "reset") { ordersResetLocal(); Serial.println("orders: reset"); return; }

    if (rest == "show") {
      Serial.printf("orders: count=%u\n", g_orderCount);
      for (uint8_t i=0;i<g_orderCount;i++) {
        Serial.printf("  [%u] H%u mask=0x%02X text=\"%s\"\n",
          i, g_orders[i].house_id, g_orders[i].mask, g_orders[i].text);
      }
      return;
    }

    if (rest == "push") { ordersPushAll(); return; }

    // orders add <house_id> <mask> "text..."
    if (rest.startsWith("add ")) {
      rest = rest.substring(4);
      int sp = rest.indexOf(' ');
      if (sp < 0) { Serial.println("usage: orders add <house_id> <mask> \"text\""); return; }
      int house = rest.substring(0, sp).toInt();

      String rest2 = rest.substring(sp+1); rest2.trim();
      sp = rest2.indexOf(' ');
      if (sp < 0) { Serial.println("usage: orders add <house_id> <mask> \"text\""); return; }

      String maskStr = rest2.substring(0, sp);
      String textStr = rest2.substring(sp+1); textStr.trim();
      if (textStr.length() && textStr[0]=='\"' && textStr[textStr.length()-1]=='\"') {
        textStr = textStr.substring(1, textStr.length()-1);
      }

      long mask = (maskStr.startsWith("0x")||maskStr.startsWith("0X")) ? strtol(maskStr.c_str(), nullptr, 16)
                                                                      : maskStr.toInt();
      if (house < 1 || house > 6 || mask < 0 || mask > 31) {
        Serial.println("usage: orders add <house_id 1..6> <mask 0..31|0xNN> \"text\"");
        return;
      }
      if (!ordersAddLocal((uint8_t)house, (uint8_t)mask, textStr.c_str())) {
        Serial.println("orders: list full (max 6)");
        return;
      }
      Serial.println("orders: added");
      return;
    }

    Serial.println("orders commands: reset | show | add <house> <mask> \"text\" | push");
    return;
  }

}
