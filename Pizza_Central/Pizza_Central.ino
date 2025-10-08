// Role: CENTRAL (Feather S3)
// Roster + clean DELIVER_SCAN → DELIVER_RESULT (auto-OK for now).
// CLI: list | hello-req | panel <id> "text" [style] [speed] [bright] | sound <id> [clip] [vol]
//      update <ROLE> all|id=<n> [url] | claim <MAC> <id> [force]

#define PIZZA_ROLE CENTRAL
#define PIZZA_HOUSE_ID 0
#define PZ_HOUSES 6

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"

static uint16_t g_seq = 1;
static Preferences s_prefsBoxes;

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

// Topping map 
static const uint8_t kTopBits[5]  = {0x01, 0x02, 0x04, 0x08, 0x10};
static const char*   kTopNames[5] = {"pepperoni","mushrooms","peppers","pineapple","ham"};

// Orders pool + L1 generator
static uint8_t g_housePool[7]; // indices 1..6 -> assigned numbers 1..99 (0 = not set)
static bool g_autoNextL1 = true;

// Enums for physical facts
enum HHouseColor : uint8_t { HC_RED=0, HC_YELLOW=1, HC_BLUE=2 };
enum HDoorColor  : uint8_t { DC_BROWN=0, DC_WHITE=1,  DC_GREY=2 };
enum HHandleShape: uint8_t { HS_ROUND=0, HS_BAR=1 };
enum HHandleColor: uint8_t { HCOL_SILVER=0, HCOL_GOLD=1 };

// Index by house id [1..6]
static uint8_t g_fact_houseColor[7] = {0, HC_RED,    HC_BLUE,   HC_YELLOW, HC_RED,    HC_YELLOW, HC_BLUE};
static uint8_t g_fact_doorColor [7] = {0, DC_GREY,   DC_WHITE,  DC_BROWN,  DC_BROWN,  DC_WHITE,  DC_GREY};
static uint8_t g_fact_handleShape[7]= {0, HS_ROUND,  HS_ROUND,  HS_BAR,    HS_ROUND,  HS_ROUND,  HS_ROUND};
static uint8_t g_fact_handleColor[7]= {0, HCOL_SILVER,HCOL_SILVER,HCOL_SILVER,HCOL_GOLD,HCOL_GOLD,HCOL_GOLD};

// "Beside" (same-side neighbors), 0 if none
static uint8_t g_adj_left [3] = {1,2,3}; // left wall, increasing
static uint8_t g_adj_right[3] = {6,5,4}; // right wall, decreasing
// Precomputed immediate neighbors (beside)
static uint8_t g_besideA[7] = {0, 2, 1, 2, 5, 6, 5}; // first neighbor
static uint8_t g_besideB[7] = {0, 0, 3, 0, 0, 4, 0}; // second neighbor (if any)

// "Across"
static uint8_t g_across[7] = {0, 6, 5, 4, 3, 2, 1};

// Box whitelist
struct BoxUID { uint8_t len; uint8_t uid[10]; bool set; };
static BoxUID g_box[6];
static int8_t g_learnSlot = 0;  // 1..6 means “capture next scan into this slot”

///// Game Manager (state + config) /////
enum GamePhase : uint8_t { GP_IDLE=0, GP_RUNNING=1, GP_OVER=2 };
struct GameState {
  GamePhase phase;
  uint8_t   level;         // 1..5
  uint16_t  targetOK;      // OK deliveries to "complete" a level (optional)
  bool      autoAdvance;   // auto level-up when targetOK reached
  uint16_t  okTotal;       // across all levels
  uint16_t  okLevel;       // within current level
  uint32_t  score;         // +100 * level per OK
  uint32_t  durationMs;    // game length
  uint32_t  startedAt;     // millis()
  uint32_t  endsAt;        // startedAt + durationMs
  uint16_t  genLevel;      // NEW: how many orders generated this level
} g_game = { GP_IDLE, 1, /*targetOK*/6, /*autoAdv*/false,
             0,0,0, /*duration*/180000, 0,0,
             /*genLevel*/0 };

static uint32_t g_gameTickDueAt = 0;

// Level 1 config
static const uint8_t L1_SEED_ORDERS = 3;
static const uint8_t L1_GEN_CAP     = 6;   // stop generating after the 6th order

// --- Orders window DRIP scheduler (Central) ---
static uint32_t ORD_DRIP_GAP_RESET_MS = 15;  // gap after RESET
static uint32_t ORD_DRIP_GAP_ITEM_MS  = 12;  // gap between ITEM sends
static uint32_t ORD_DRIP_START_DELAY_MS = 60; // optional delay after verdict/snapshot

struct OrdersDrip {
  bool     active;
  uint8_t  window;     // how many items to send (0..3)
  uint8_t  idx;        // current item index 0..window-1
  uint8_t  stage;      // 0=send RESET, 1=send item[idx] pass1, 2=pass2, 3=advance idx
  uint32_t dueAt;      // next action time
};
static OrdersDrip s_ordersDrip = {false,0,0,0,0};

///// forward declarations /////
static void ordersPushWindow(uint8_t maxDisplay = 3);
static void ordersPushWindowDripStart(uint8_t maxDisplay, uint16_t startDelayMs = 0);

// Helpers
static void gameResetOrdersAndMasks(){
  ordersResetLocal();
  for (int i=0;i<256;i++) g_orderMask[i] = MASK_NONE;
}

static void gamePrintStatus(){
  uint32_t now = millis();
  int32_t remaining = (g_game.phase==GP_RUNNING)? (int32_t)(g_game.endsAt - now) : 0;
  Serial.printf("game: phase=%s level=%u score=%lu okLevel=%u/%u okTotal=%u timeLeft=%ldms\n",
    (g_game.phase==GP_IDLE?"IDLE":(g_game.phase==GP_RUNNING?"RUNNING":"OVER")),
    g_game.level, (unsigned long)g_game.score, g_game.okLevel, g_game.targetOK, g_game.okTotal, (long)remaining);
}

static void gameStart(uint16_t minutes, uint8_t level){
  if (minutes==0) minutes = 3;
  if (level==0)   level = 1;
  g_game.phase = GP_RUNNING;
  g_game.level = level;
  g_game.okLevel = 0;
  g_game.okTotal = 0;
  g_game.score   = 0;
  g_game.durationMs = (uint32_t)minutes * 60UL * 1000UL;
  g_game.startedAt  = millis();
  g_game.endsAt     = g_game.startedAt + g_game.durationMs;
  g_game.genLevel   = 0;

  // Fresh numbers + panels
  ordersAssignNumbers(esp_random());
  housesNumbersShow();

  // Fresh list + masks
  ordersResetLocal();

  // Seed orders: L1 -> 3, otherwise 1
  uint8_t seeds = (g_game.level==1 ? L1_SEED_ORDERS : 1);
  for (uint8_t i=0; i<seeds; ++i) {
    if (ordersGenLevel1()) ++g_game.genLevel;
  }

  // Arm validators for ALL seeded orders and push once
  armAllOrdersFromLocal();
  ordersPushWindowDripStart(3 /*max*/, 0 /*start delay*/);

  // Keep L1 auto-next on; capped by genLevel vs L1_GEN_CAP
  g_autoNextL1 = true;

  g_gameTickDueAt = millis() + 200;
  Serial.println("game: START");
  gamePrintStatus();
}

static void gameStop(){
  if (g_game.phase == GP_IDLE) return;
  g_game.phase = GP_OVER;
  // Clear list; OrderStation will show "NO ORDERS"
  gameResetOrdersAndMasks();
  ordersPushAll();
  Serial.println("game: STOP");
  gamePrintStatus();
}

static void gameNextLevel(){
  if (g_game.phase != GP_RUNNING) return;
  g_game.level++;
  g_game.okLevel = 0;
  // reset list and seed one order for the new level (for now: still L1 generator)
  gameResetOrdersAndMasks();
  if (ordersGenLevel1()) {
    const auto &it = g_orders[g_orderCount-1];
    g_orderMask[it.house_id] = it.mask;
  }
  ordersPushAll();
  Serial.printf("game: NEXT -> level=%u\n", g_game.level);
  gamePrintStatus();
}

static void gameOnOk(uint8_t /*houseId*/){
  if (g_game.phase != GP_RUNNING) return;
  g_game.okTotal++;
  g_game.okLevel++;
  g_game.score += (uint32_t)(100 * g_game.level);
  // Auto-advance if enabled and target reached
  if (g_game.autoAdvance && g_game.okLevel >= g_game.targetOK) {
    gameNextLevel();
  }
}

static void gameTick(){
  uint32_t now = millis();
  if ((int32_t)(now - g_gameTickDueAt) < 0) return;
  g_gameTickDueAt = now + 200; // 5 Hz is plenty

  if (g_game.phase == GP_RUNNING && (int32_t)(now - g_game.endsAt) >= 0) {
    Serial.println("game: TIME UP");
    gameStop();
  }
}

// Start a scheduled push of the current window (up to maxDisplay)
static void ordersPushWindowDripStart(uint8_t maxDisplay, uint16_t startDelayMs) {
  uint8_t win = (g_orderCount < maxDisplay) ? g_orderCount : maxDisplay;

  s_ordersDrip.active = true;
  s_ordersDrip.window = win;
  s_ordersDrip.idx    = 0;
  s_ordersDrip.stage  = 0;                       // send RESET first
  s_ordersDrip.dueAt  = millis() + startDelayMs; // small pause after verdicts
  Serial.printf("[central] DRIP start: window=%u delay=%ums\n", win, (unsigned)startDelayMs);
}

static void ordersPushWindowDripTick() {
  if (!s_ordersDrip.active) return;
  if ((int32_t)(millis() - s_ordersDrip.dueAt) < 0) return;

  if (s_ordersDrip.stage == 0) {
    // RESET
    ordersSendReset(s_ordersDrip.window);
    s_ordersDrip.stage = 1;
    s_ordersDrip.dueAt = millis() + ORD_DRIP_GAP_RESET_MS;
    return;
  }

  // Done?
  if (s_ordersDrip.idx >= s_ordersDrip.window) {
    s_ordersDrip.active = false;
    Serial.println("[central] DRIP done");
    return;
  }

  // Prepare the current item (rebase index 0..window-1)
  PzOrderItemSetPayload it = g_orders[s_ordersDrip.idx];
  it.index = s_ordersDrip.idx;

  if (s_ordersDrip.stage == 1) {
    // First send of this item
    ordersSendItem(it);
    s_ordersDrip.stage = 2;
    s_ordersDrip.dueAt = millis() + ORD_DRIP_GAP_ITEM_MS;
    return;
  }

  if (s_ordersDrip.stage == 2) {
    // Second send (covers occasional drop)
    ordersSendItem(it);
    s_ordersDrip.stage = 3;
    s_ordersDrip.dueAt = millis() + ORD_DRIP_GAP_ITEM_MS;
    return;
  }

  // stage 3: advance to next item
  s_ordersDrip.idx++;
  s_ordersDrip.stage = 1;
  // immediate next item send allowed (no extra delay), or add a small one:
  // s_ordersDrip.dueAt = millis() + ORD_DRIP_GAP_ITEM_MS;
}

static void ordersAssignNumbers(uint32_t seed) {
  randomSeed(seed);
  // Pick a base so base..base+5 ⊆ [1..99]
  uint8_t base = 1 + (random(0, 94)); // 1..94

  // Disperse in this exact order: H1, H6, H2, H5, H3, H4
  const uint8_t houseOrder[6] = {1, 6, 2, 5, 3, 4};
  for (int i = 0; i < 6; ++i) {
    uint8_t h = houseOrder[i];
    g_housePool[h] = base + i;
  }

  Serial.printf("orders: pool ->");
  for (int h=1; h<=6; ++h) Serial.printf(" H%d=#%u", h, g_housePool[h]);
  Serial.println();
}

static void ordersShowPool() {
  if (!g_housePool[1]) { Serial.println("orders: pool is empty (run `orders pool reset`)"); return; }
  Serial.printf("orders: pool ->"); for (int h=1; h<=6; ++h) Serial.printf(" H%d=#%u", h, g_housePool[h]); Serial.println();
}

static void housesNumbersShow() {
  if (!g_housePool[1]) { Serial.println("houses: pool empty; run `orders pool reset` first"); return; }
  for (uint8_t h=1; h<=6; ++h) {
    char digits[8]; snprintf(digits, sizeof(digits), "%u", (unsigned)g_housePool[h]);
    // Panel only (flags=0x02). NOTE: 5 window fields -> 0,0,0,0,0
    sendHouseDigital(h, /*flags*/0x02,
                     /*win*/0,0,0,0,0,
                     /*panel*/PANEL_MODE_NUMBER, digits, 1,1,180,
                     /*spk*/0,0,false,false);
  }
  Serial.println("houses: numbers pushed to panels");
}

// Add 1 Level-1 order to your local list (returns false if list is full)
static bool ordersGenLevel1() {
  if (!g_housePool[1]) ordersAssignNumbers(esp_random()); // lazy-init
  uint8_t targetHouse = 1 + (esp_random() % 6);  // 1..6
  uint8_t topIdx      = (esp_random() % 5);      // 0..4
  uint8_t mask        = kTopBits[topIdx];

  char clue[96];
  snprintf(clue, sizeof(clue), "%s pizza for house #%u",
           kTopNames[topIdx], g_housePool[targetHouse]);

  bool ok = ordersAddLocal(targetHouse, mask, clue);
  if (ok) Serial.printf("orders: gen1 -> H%u mask=0x%02X text=\"%s\"\n", targetHouse, mask, clue);
  return ok;
}

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

// Set per-house validator masks from current local order list
static void armAllOrdersFromLocal(){
  for (int i=0; i<256; ++i) g_orderMask[i] = MASK_NONE;
  for (uint8_t i=0; i<g_orderCount; ++i) {
    const auto &it = g_orders[i];
    g_orderMask[it.house_id] = it.mask;   // last one for a house wins if duplicates
  }
}

static void ordersRemoveByHouse(uint8_t h){
  for (uint8_t i=0; i<g_orderCount; ) {
    if (g_orders[i].house_id == h) {
      for (uint8_t j=i+1; j<g_orderCount; ++j) { g_orders[j-1] = g_orders[j]; g_orders[j-1].index = j-1; }
      --g_orderCount;
    } else {
      ++i;
    }
  }
}

// Called on DR_OK
static void onDeliveredOk(uint8_t houseId){
  // 1) Remove delivered order(s) and disarm that house
  ordersRemoveByHouse(houseId);
  g_orderMask[houseId] = MASK_NONE;

  // 2) Optionally auto-create next order (L1 capped at 6 total generated)
  if (g_autoNextL1) {
    bool added = false;
    if (g_game.level == 1) {
      if (g_game.genLevel < L1_GEN_CAP) {
        if (ordersGenLevel1()) { ++g_game.genLevel; added = true; }
      }
    } else {
      // future levels can choose their own generator; keep L1 for now
      if (ordersGenLevel1()) added = true;
    }
    (void)added;
  }

  // 3) Arm ALL current orders and push ONCE
  armAllOrdersFromLocal();
  ordersPushWindowDripStart(3, ORD_DRIP_START_DELAY_MS);  // e.g., 60ms pause after verdict/snapshot

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

static void ingrClearTag(const uint8_t* uid, uint8_t uidLen) {
  int idx = tagFind(uid, uidLen);
  if (idx >= 0) {
    g_tags[idx].mask = 0;                 // ingredients cleared
    g_tags[idx].ts   = millis();
  }
  PizzaIngrSnapshotPayload sp{};
  sp.uid_len = uidLen;
  memcpy(sp.uid, uid, uidLen);
  sp.ok   = 1;
  sp.mask = 0;                            // broadcast "now empty"
  sendIngrSnapshot(sp);
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

// Handy printer
static void factsShow(){
  auto hc=[](uint8_t c){ return (c==HC_RED?"red":(c==HC_YELLOW?"yellow":"blue")); };
  auto dc=[](uint8_t c){ return (c==DC_BROWN?"brown":(c==DC_WHITE?"white":"grey")); };
  auto hs=[](uint8_t s){ return (s==HS_ROUND?"round":"bar"); };
  auto hk=[](uint8_t c){ return (c==HCOL_SILVER?"silver":"gold"); };
  for (uint8_t h=1; h<=6; ++h){
    Serial.printf("H%u: house=%s door=%s handle=%s/%s beside={%u%s%s} across=%u\n",
      h, hc(g_fact_houseColor[h]), dc(g_fact_doorColor[h]),
      hs(g_fact_handleShape[h]), hk(g_fact_handleColor[h]),
      g_besideA[h], (g_besideB[h]? ",":""),
      (g_besideB[h]? String(g_besideB[h]).c_str():""), g_across[h]);
  }
}

// Box whitelist
static bool uidEq(const uint8_t* a, uint8_t alen, const uint8_t* b, uint8_t blen){
  if (alen != blen) return false;
  return memcmp(a, b, alen) == 0;
}

static bool isWhitelisted(const uint8_t* uid, uint8_t len){
  for (int i=0;i<6;i++) if (g_box[i].set && uidEq(uid, len, g_box[i].uid, g_box[i].len)) return true;
  return false;
}

static void boxesShow(){
  Serial.println("boxes:");
  for (int i=0;i<6;i++){
    Serial.printf("  %d: ", i+1);
    if (!g_box[i].set) { Serial.println("(empty)"); continue; }
    for (uint8_t k=0;k<g_box[i].len;k++){ if (g_box[i].uid[k]<16) Serial.print('0'); Serial.print(g_box[i].uid[k], HEX); if (k+1<g_box[i].len) Serial.print(':'); }
    Serial.println();
  }
}

static void boxesClear(int idx /*1..6 or 0 for all*/){
  if (idx==0){ for (int i=0;i<6;i++){ g_box[i].set=false; g_box[i].len=0; } }
  else if (idx>=1 && idx<=6){ g_box[idx-1].set=false; g_box[idx-1].len=0; }
}

static bool parseUidHex(const String& s, uint8_t out[10], uint8_t &len){
  // Accept "04:A1:B2:C3" or "04A1B2C3"
  String t = s; t.toUpperCase(); String clean;
  for (char c: t) if ((c>='0'&&c<='9')||(c>='A'&&c<='F')) clean += c;
  if (clean.length()<8 || clean.length()>20 || (clean.length()&1)) return false;
  len = (uint8_t)(clean.length()/2);
  for (uint8_t i=0;i<len;i++){ char b[3]={clean[2*i], clean[2*i+1],0}; out[i]=(uint8_t)strtol(b,nullptr,16); }
  return true;
}

// Packed form for NVS
struct BoxStore {
  uint8_t len;
  uint8_t uid[10];
  uint8_t set;
  uint8_t _pad;                   // align to 12 bytes each (optional)
};

static void boxesSave(){
  BoxStore a[6] = {};
  for (int i=0;i<6;i++){
    a[i].len = g_box[i].len;
    a[i].set = g_box[i].set ? 1 : 0;
    memset(a[i].uid, 0, sizeof(a[i].uid));
    if (g_box[i].len && g_box[i].len <= 10) {
      memcpy(a[i].uid, g_box[i].uid, g_box[i].len);
    }
  }
  s_prefsBoxes.begin("central", false);   // NVS namespace for Central
  s_prefsBoxes.putBytes("boxes", a, sizeof(a));
  s_prefsBoxes.end();
  Serial.println("boxes: saved to NVS");
}

static void boxesLoad(){
  BoxStore a[6] = {};
  s_prefsBoxes.begin("central", true);
  size_t got = s_prefsBoxes.getBytes("boxes", a, sizeof(a));
  s_prefsBoxes.end();

  if (got == sizeof(a)) {
    for (int i=0;i<6;i++){
      g_box[i].len = a[i].len;
      g_box[i].set = (a[i].set != 0);
      if (g_box[i].len > 10) g_box[i].len = 0;
      if (g_box[i].len) memcpy(g_box[i].uid, a[i].uid, g_box[i].len);
    }
    Serial.println("boxes: loaded from NVS");
  } else {
    // Nothing stored yet → leave empty
    for (int i=0;i<6;i++){ g_box[i].set=false; g_box[i].len=0; }
    Serial.println("boxes: NVS empty");
  }
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
    uint32_t last = g_devices[i].last_seen_ms;
    uint32_t age  = millis() - last;
    Serial.printf("%-12s  %2u   %-8s  %-17s  %9u  %7us\n",
      rn, g_devices[i].house_id, g_devices[i].fw, macbuf,
      (unsigned)last, (unsigned)(age/1000));
  }
  Serial.println();
}

static void rosterTouch(const uint8_t mac[6], uint8_t role, uint8_t house_id){
  int idx = allocSlotForMac(mac);
  if (idx < 0) return;
  g_devices[idx].last_seen_ms = millis();
  if (role)     g_devices[idx].role = (Role)role;
  if (house_id) g_devices[idx].house_id = house_id;
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
  // 1) RESET
  ordersSendReset(g_orderCount);
  delay(8); // small gap helps ESPNOW

  // 2) ITEMS, with a tiny gap; optionally retransmit each once
  for (uint8_t i = 0; i < g_orderCount; ++i) {
    ordersSendItem(g_orders[i]);
    delay(6); // 4–10 ms is enough in practice
  }
}

static void ordersPushWindow(uint8_t maxDisplay) {
  uint8_t window = (g_orderCount < maxDisplay) ? g_orderCount : maxDisplay;

  ordersSendReset(window);
  delay(8); // small gap helps on ESPNOW

  for (uint8_t i = 0; i < window; ++i) {
    PzOrderItemSetPayload it = g_orders[i]; // take first 'window' items
    it.index = i;                            // <— REBASE indices to 0..window-1
    ordersSendItem(it);
    delay(6);
  }
  Serial.printf("[Central] Pushed window=%u of total=%u\n", window, g_orderCount);
}

/*** CENTRAL: send ORDER_SHOW_TEXT directly to the panel ***/
static void ordersSendShowText(const char* s) {
  PzOrderShowTextPayload p{};
  if (s && *s) strlcpy(p.text, s, sizeof(p.text));
  else         strcpy(p.text, "NO ORDERS");

  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, CENTRAL, 0, g_seq_orders++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  Serial.printf("[Central] ORDER_SHOW_TEXT len=%u\n", (unsigned)strlen(p.text));
}

// === CENTRAL: apply the pushed orders to the delivery validator ===
// Mirrors g_orders[] into per-house g_orderMask[] so deliveries validate
static void ordersApplyToValidator() {
  // If your game only uses houses 1..6 in L1, clear that range:
  for (int id = 1; id <= 6; ++id) {
    g_orderMask[id] = MASK_NONE;      // assumes MASK_NONE exists in your codebase
  }
  // Now project each order's mask to its house id
  for (uint8_t i = 0; i < g_orderCount; ++i) {
    const auto &it = g_orders[i];     // it.house_id, it.mask, it.text
    if (it.house_id >= 1 && it.house_id <= 6) {
      g_orderMask[it.house_id] = it.mask;
    }
  }
  Serial.println(F("[central] ordersApplyToValidator: validator masks updated"));
}

static void sendHouseDigital(
  uint8_t houseId, uint8_t flags,                // flags: b0=window, b1=panel, b2=speaker
  // window
  uint8_t fx, uint8_t h, uint8_t s, uint8_t v, uint8_t fxSpeed,
  // panel
  uint8_t panelMode, const char* panelText, uint8_t panelStyle, uint8_t panelSpeed, uint8_t panelBright,
  // speaker
  uint8_t clip, uint8_t vol, bool loop, bool stopNow
){
  HouseDigitalSetPayload p{};
  p.house_id   = houseId;
  p.flags      = (uint8_t)(flags & 0x07);

  // Window payload (applied only if flags&0x01)
  p.win_fx     = fx;   p.win_h = h; p.win_s = s; p.win_v = v; p.win_speed = fxSpeed;

  // Panel payload (applied only if flags&0x02)
  p.panel_mode  = panelMode;
  if (panelText) { strlcpy(p.panel_text, panelText, sizeof(p.panel_text)); }
  p.panel_style = panelStyle; p.panel_speed = panelSpeed; p.panel_bright = panelBright;

  // Speaker payload (applied only if flags&0x04)
  p.spk_clip   = clip; p.spk_vol = vol; p.spk_flags = (loop?1:0) | (stopNow?2:0);

  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(HOUSE_DIGITAL_SET, CENTRAL, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HOUSE_DIGITAL_SET -> H%u flags=0x%02X", houseId, p.flags);
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
  rosterTouch(srcMac, hdr.role, hdr.house_id);

  /*** Block B: onRx – PIZZA_ING_UPDATE -> remember uid→mask ***/
  if (hdr.type == PIZZA_ING_UPDATE && len >= sizeof(PizzaIngrUpdatePayload)) {
    const PizzaIngrUpdatePayload* u = (const PizzaIngrUpdatePayload*)payload;
    if (!isWhitelisted(u->uid, u->uid_len)) {
      Serial.println("[Central] ING_UPDATE ignored: UID not whitelisted");
      return;
    }
    Serial.printf("[Central] ING_UPDATE from id=%u mask=0x%02X\n", hdr.house_id, u->mask);
    // Remember uid->mask for validation later
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

    // Optional learn mode (you kept this — good)
    if (g_learnSlot >= 1 && g_learnSlot <= 6) {
      g_box[g_learnSlot-1].len = s->uid_len;
      memcpy(g_box[g_learnSlot-1].uid, s->uid, s->uid_len);
      g_box[g_learnSlot-1].set = true;
      Serial.printf("boxes: learned slot %d -> ", g_learnSlot);
      for (uint8_t k=0;k<s->uid_len;k++){ if (s->uid[k]<16) Serial.print('0'); Serial.print(s->uid[k], HEX); if (k+1<s->uid_len) Serial.print(':'); }
      Serial.println();
      g_learnSlot = 0;
      boxesSave();
      // continue to normal validation
    }

    uint8_t reason = DR_OK;
    uint8_t order  = g_orderMask[s->house_id];

    // 1) must be whitelisted
    if (!isWhitelisted(s->uid, s->uid_len)) {
      reason = DR_UNKNOWN_PIZZA;
    } else {
      // 2) must have ingredient mask cached
      int idx = tagFind(s->uid, s->uid_len);
      if (idx < 0)                      reason = DR_UNKNOWN_PIZZA;
      else if (order == MASK_NONE)      reason = DR_NO_ORDER;
      else if (g_tags[idx].mask != order) reason = DR_WRONG_PIZZA;
    }

    // Tell the house the verdict
    sendDeliverResult(s->house_id, (reason == DR_OK), reason);

    // On success: clear that box’s ingredients and trigger auto-next flow
    if (reason == DR_OK) {
      ingrClearTag(s->uid, s->uid_len);     // <— clear toppings + broadcast snapshot
      gameOnOk(s->house_id);                // <— score/level counters
      onDeliveredOk(s->house_id);           // <— remove order, maybe create+arm next
    }
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

  if (hdr.type == ASSET_RESULT && len >= sizeof(AssetResultPayload)) {
    const AssetResultPayload* r = (const AssetResultPayload*)payload;
    Serial.printf("[Central] ASSET_RESULT H%u ok=%u count=%u code=%u\n",
                  r->house_id, r->ok, r->count_done, r->code);
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

/*** Central CLI: help printer ***/
static void printHelp() {
  Serial.println(F("=== Central CLI Help ==="));
  Serial.println(F(""));
  Serial.println(F("General"));
  Serial.println(F("  help | ?                       Show this help"));
  Serial.println(F("  list                           Print device roster"));
  Serial.println(F("  hello-req                      Ask nodes to send HELLO"));
  Serial.println(F(""));
  Serial.println(F("Panels"));
  Serial.println(F("  panel <id> \"text\" [style] [speed] [bright]"));
  Serial.println(F("      style: 0=scroll, 1=static (see PizzaPanel defs)"));
  Serial.println(F("      speed: 1..5, bright: 0..255"));
  Serial.println(F(""));
  Serial.println(F("Sound"));
  Serial.println(F("  sound <id> [clip] [vol]        vol: 0..255"));
  Serial.println(F(""));
  Serial.println(F("OTA / Updates"));
  Serial.println(F("  update <ROLE> all|id=<n> [url]"));
  Serial.println(F("      ROLE: HOUSE_PANEL | HOUSE_NODE | ORDERS_PANEL | ORDERS_NODE | PIZZA_NODE | CENTRAL"));
  Serial.println(F(""));
  Serial.println(F("Claiming"));
  Serial.println(F("  claim <MAC> <id> [force]       ex: claim AA:BB:CC:DD:EE:FF 3"));
  Serial.println(F(""));
  Serial.println(F("Delivery Validator (per-house mask)"));
  Serial.println(F("  order <id> <mask>              set target mask for house (0..31 or 0xNN)"));
  Serial.println(F("  order show [id]                show orders (all or one)"));
  Serial.println(F("  order clear <id>               clear order for a house"));
  Serial.println(F("  tags                           list remembered uid->mask cache"));
  Serial.println(F(""));
  Serial.println(F("Operator Orders (text list for Orders Node/Panel)"));
  Serial.println(F("  orders reset                   clear local list (max 6 items)"));
  Serial.println(F("  orders add <house> <mask> \"text\""));
  Serial.println(F("  orders show                    print local list"));
  Serial.println(F("  orders push                    broadcast list to Orders Node"));
  Serial.println(F("  orders showidx <n>             display item n on Orders Panel"));
  Serial.println(F("  orders show \"text\"            display ad-hoc text on Orders Panel"));
  Serial.println();
}

void setup() {
  Serial.begin(115200); delay(100);
  PZ_LOGI("Central boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // Radio
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);

  stateInit();
  boxesLoad();

  // Help
  Serial.println(F("Type 'help' for commands."));
}

void loop() {
  PizzaNow::loop();

  gameTick();
  ordersPushWindowDripTick();

  String line = readLine();
  if (!line.length()) return;
  line.trim();

  /*** Central CLI: help entrypoint ***/
  if (line == "help" || line == "?") { printHelp(); return; }
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

    // New: number pool for houses (6 consecutive numbers 1..99)
    if (rest == "pool reset") {
      ordersAssignNumbers(esp_random());
      Serial.println("orders: pool reset");
      housesNumbersShow();              // <— push digits to panels automatically
      return;
    }
    if (rest == "pool show")  { ordersShowPool(); return; }

    // Existing: reset/show/push
    if (rest == "reset") { ordersResetLocal(); Serial.println("orders: reset"); return; }

    if (rest == "show") {
      Serial.printf("orders: count=%u\n", g_orderCount);
      for (uint8_t i=0;i<g_orderCount;i++) {
        Serial.printf("  [%u] H%u mask=0x%02X text=\"%s\"\n",
          i, g_orders[i].house_id, g_orders[i].mask, g_orders[i].text);
      }
      return;
    }

    if (rest == "push") {
      ordersPushAll();
      ordersApplyToValidator();
      return;
    }

    if (rest == "apply") {
      ordersApplyToValidator();
      return;
    }

    // New: auto-generate one Level-1 order and add to local list
    if (rest == "gen1") {
      if (!ordersGenLevel1()) { Serial.println("orders: list full (max 6)"); return; }

      // Arm the per-house validator for this order
      const auto &it = g_orders[g_orderCount-1];
      g_orderMask[it.house_id] = it.mask;

      // Push list to Orders Node/Panel; Order Station UI handles display
      ordersPushAll();

      // Optional: ensure panels are showing the numbers (safe to resend)
      housesNumbersShow();

      Serial.printf("orders: armed H%u with mask=0x%02X; clue in list\n", it.house_id, it.mask);
      return;
    }

    // Existing: orders add <house_id> <mask> "text..."
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

    // Existing: orders showidx <n>  -> send text of item n to panel
    if (rest.startsWith("showidx ")) {
      int n = rest.substring(8).toInt();
      if (n < 0 || n >= g_orderCount) { Serial.println("usage: orders showidx <0..count-1>"); return; }
      ordersSendShowText(g_orders[n].text);
      return;
    }

    // Existing: orders show "text..." -> send arbitrary text to panel
    if (rest.startsWith("show ")) {
      String txt = rest.substring(5); txt.trim();
      if (txt.length() && txt[0]=='"' && txt[txt.length()-1]=='"') {
        txt = txt.substring(1, txt.length()-1);
      }
      ordersSendShowText(txt.c_str());
      return;
    }

    if (rest.startsWith("auto ")) {
      String v = rest.substring(5); v.trim();
      g_autoNextL1 = (v == "on");
      Serial.printf("orders auto-next L1 = %s\n", g_autoNextL1 ? "ON" : "OFF");
      return;
    }

    Serial.println("orders commands: reset | show | push | add <house> <mask> \"text\" | showidx <n> | show \"text\" | pool reset | pool show | gen1");
    return;
  }

  // scene <id> party | stop | number <digits>
  if (line.startsWith("scene ")) {
    // scene <id> party | stop | number <digits>
    int s1 = line.indexOf(' '); if (s1<0) return;
    String rest = line.substring(s1+1); rest.trim();
    int s2 = rest.indexOf(' ');
    uint8_t id = (uint8_t)((s2>0)? rest.substring(0,s2).toInt() : 1);
    String mode = (s2>0)? rest.substring(s2+1) : String("party");

    if (mode == "party") {
      // Window + Speaker only (leave panel as-is)
      sendHouseDigital(id, /*flags*/0x01|0x04,
                      /*win*/WIN_FX_PARTY, 0,255,40, 140,
                      /*panel*/0, "", 0,0,0,
                      /*spk*/1, 200, /*loop*/true, /*stop*/false);
    } else if (mode == "stop") {
      // Turn window off + stop speaker (leave panel as-is)
      sendHouseDigital(id, /*flags*/0x01|0x04,
                      /*win*/WIN_FX_OFF, 0,0,0, 0,
                      /*panel*/0, "", 0,0,0,
                      /*spk*/0, 0, /*loop*/false, /*stop*/true);
    } else if (mode.startsWith("number ")) {
      String digits = mode.substring(7); digits.trim();
      // Only set panel (leave window/speaker untouched)
      sendHouseDigital(id, /*flags*/0x02,
                      /*win*/0, 0,0,0, 0,
                      /*panel*/PANEL_MODE_NUMBER, digits.c_str(), 1,1,180,
                      /*spk*/0,0,false,false);
    }
    return;
  }

  // clips sync <id|all> <base_url> <count>
  if (line.startsWith("clips sync ")) {
    // clips sync 3 http://10.0.0.5/pizza/h3 12
    String rest = line.substring(String("clips sync ").length()); rest.trim();
    int sp1 = rest.indexOf(' '), sp2 = rest.indexOf(' ', sp1+1);
    if (sp1<0 || sp2<0) { Serial.println(F("usage: clips sync <id> <base_url> <count>")); return; }

    int id = rest.substring(0,sp1).toInt();
    String base = rest.substring(sp1+1, sp2);
    int count = rest.substring(sp2+1).toInt(); if (count < 1) count = 1; if (count > 30) count = 30;

    AssetSyncPayload ap{}; ap.house_id = (uint8_t)id;
    strlcpy(ap.base_url, base.c_str(), sizeof(ap.base_url)); ap.count = (uint8_t)count;

    uint8_t out[192]; size_t n = PizzaProtocol::pack(ASSET_SYNC, CENTRAL, 0, g_seq++, &ap, sizeof(ap), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);
    Serial.printf("ASSET_SYNC -> id=%d url=%s count=%d\n", id, ap.base_url, ap.count);
    return;
  }

  // boxes show
  if (line == "boxes show"){ boxesShow(); return; }

  // boxes clear            -> clear all
  // boxes clear <n>        -> clear one slot (1..6)
  if (line.startsWith("boxes clear")){
    String rest = line.substring(11); rest.trim();
    int slot = rest.length()? rest.toInt() : 0;
    boxesClear(slot);
    Serial.println("boxes: cleared");
    boxesSave();
    return;
  }

  // boxes set <n> <uidHex>
  if (line.startsWith("boxes set ")){
    String rest = line.substring(10); rest.trim();
    int sp = rest.indexOf(' ');
    if (sp<0){ Serial.println("usage: boxes set <1..6> <UIDhex>"); return; }
    int slot = rest.substring(0,sp).toInt();
    String hex = rest.substring(sp+1); hex.trim();
    if (slot<1||slot>6){ Serial.println("usage: boxes set <1..6> <UIDhex>"); return; }
    uint8_t buf[10], len=0;
    if (!parseUidHex(hex, buf, len)){ Serial.println("boxes set: bad UID"); return; }
    g_box[slot-1].set=true; g_box[slot-1].len=len; memcpy(g_box[slot-1].uid, buf, len);
    Serial.printf("boxes: set slot %d\n", slot);
    boxesSave();
    return;
  }

  // boxes learn <n>   -> next scanned tag will populate slot n
  if (line.startsWith("boxes learn ")){
    int slot = line.substring(12).toInt();
    if (slot<1||slot>6){ Serial.println("usage: boxes learn <1..6>"); return; }
    g_learnSlot = slot;
    Serial.printf("boxes: learning slot %d (scan a pizza box tag once)\n", slot);
    return;
  }

  if (line == "boxes reload"){ boxesLoad(); boxesShow(); return; }
  if (line == "facts show"){ factsShow(); return; }

  // game start [minutes] [level]
  if (line.startsWith("game start")){
    String rest = line.substring(10); rest.trim();
    int minutes = 3, lvl = 1;
    if (rest.length()){
      int sp = rest.indexOf(' ');
      if (sp<0) { minutes = rest.toInt(); }
      else { minutes = rest.substring(0,sp).toInt(); lvl = rest.substring(sp+1).toInt(); }
    }
    if (minutes <= 0) minutes = 3;
    if (lvl <= 0)     lvl = 1;
    gameStart((uint16_t)minutes, (uint8_t)lvl);
    return;
  }
  if (line == "game stop"){ gameStop(); return; }
  if (line == "game status"){ gamePrintStatus(); return; }
  // game next  -> advance to next level (manual)
  if (line == "game next"){ gameNextLevel(); return; }
  // game target <n>  -> how many OKs to consider a level "complete"
  if (line.startsWith("game target ")){ int n=line.substring(12).toInt(); if (n>0 && n<99) g_game.targetOK=n; gamePrintStatus(); return; }
  // game auto on|off -> auto advance level when target reached
  if (line.startsWith("game auto ")){ String v=line.substring(10); v.trim(); g_game.autoAdvance=(v=="on"); gamePrintStatus(); return; }
  // game minutes <n> -> change duration for next start
  if (line.startsWith("game minutes ")){ int m=line.substring(13).toInt(); if (m>0) g_game.durationMs=(uint32_t)m*60UL*1000UL; return; }

}
