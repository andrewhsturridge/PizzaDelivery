// =============================================================
// Pizza Delivery ESP-NOW Central
//
// With standardized serial protocol for the main Player Management System (PMS)
//
// Standard protocol lines always start with:
//   !PMS
//
// PMS protocol (v1) ***DO NOT REMOVE***:
//
//   PMS -> Central:
//     !PMS PING
//     !PMS START level=1        (level 1..5; starts run at that level; resets score/lives)
//     !PMS STOP
//
//   Central -> PMS:
//     !PMS PONG v=1 game=pizza role=server
//     !PMS STATUS v=1 state=playing level=.. score=.. lives=.. tleft_ms=.. last_reason=..
//       (STATUS is NOT emitted while idle)
//     !PMS EVENT v=1 name=game_start level=..
//     !PMS EVENT v=1 name=game_end reason=timeup|no_lives|stopped score=.. lives=..
//     !PMS EVENT v=1 name=score delta=.. total=.. bonus=0
//     !PMS EVENT v=1 name=life delta=-1 lives=..
//
// Build toggles (compile-time):
//   - PMS_STD_ENABLED: enable/disable PMS protocol support
//   - PMS_DEBUG_SERIAL: when 0, suppress non-!PMS debug/legacy Serial prints (clean PMS output)
//
// =============================================================

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
#include "PizzaNetCfg.h"
#include "PizzaGameEngine.h"


// =============================================================
// PMS / DEBUG SERIAL TOGGLES
// =============================================================

// 1 = Enable PMS standard protocol parsing/output.
// 0 = Legacy-only (no !PMS parsing/output).
#ifndef PMS_STD_ENABLED
#define PMS_STD_ENABLED 1
#endif

// 1 = Keep legacy/debug Serial prints (CLI help, verbose logs).
// 0 = Suppress all non-!PMS Serial prints (clean PMS serial output).
#ifndef PMS_DEBUG_SERIAL
#define PMS_DEBUG_SERIAL 0
#endif

#ifndef PMS_STATUS_PERIOD_MS
#define PMS_STATUS_PERIOD_MS 250
#endif

#if PMS_STD_ENABLED
// End reason hint used for !PMS game_end reporting (set by STOP/TIMEUP/NO_LIVES code paths)
static const char* s_pmsEndReasonHint = nullptr; // "stopped"|"timeup"|"no_lives"
#endif

#if PMS_DEBUG_SERIAL
  // Variadic macros allow DBG_PRINTLN() with no args
  #define DBG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...)  Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)    do { } while (0)
  #define DBG_PRINTLN(...)  do { } while (0)
  #define DBG_PRINTF(...)   do { } while (0)
#endif

// Best-effort suppression of logging macros (if present) to keep serial output clean.
#if !PMS_DEBUG_SERIAL
  #ifdef PZ_LOGI
    #undef PZ_LOGI
    #define PZ_LOGI(...) do { } while (0)
  #endif
  #ifdef PZ_LOGW
    #undef PZ_LOGW
    #define PZ_LOGW(...) do { } while (0)
  #endif
  #ifdef PZ_LOGE
    #undef PZ_LOGE
    #define PZ_LOGE(...) do { } while (0)
  #endif
#endif

static uint16_t g_seq = 1;
static Preferences s_prefsBoxes;

static char g_net_ssid[32] = WIFI_DEFAULT_SSID;
static char g_net_pass[64] = WIFI_DEFAULT_PASS;
static char g_net_base[128]= OTA_BASE_URL_DEFAULT;

/*** Block A: GLOBAL STATE + HELPERS ***/
// Legacy per-house validator masks (kept for backward compatibility / debugging).
// NOTE: The new engine does NOT use this; deliveries are rejected when game is idle.
static const uint8_t MASK_NONE = 0xFF;
static uint8_t g_orderMask[256];

// Small ring buffer of recent pizza tags we’ve seen (uid -> mask)
struct TagEntry { uint8_t len; uint8_t uid[10]; uint8_t mask; uint32_t ts; };
static TagEntry g_tags[32];
static uint8_t  g_tagCount = 0;

/*** CENTRAL: Orders storage ***/
static PzOrderItemSetPayload g_orders[PZ_ORDERS_MAX];
static uint8_t g_orderCount = 0;
static uint16_t g_seq_orders = 1;

// --- New mapping-first game engine (levels 1..5, always-on identities, per-order timers) ---
static PizzaGameEngine g_engine;

// For status printing only (must match PizzaGameEngine::levelCfg)
static uint16_t quotaForLevel(uint8_t lvl) {
  switch (lvl) {
    default:
    case 1: return 4;
    case 2: return 4;
    case 3: return 5;
    case 4: return 6;
    case 5: return 6;
  }
}


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
static uint8_t g_fact_houseColor[7] = {0, HC_BLUE,    HC_RED,   HC_YELLOW, HC_BLUE,    HC_YELLOW, HC_RED};
static uint8_t g_fact_doorColor [7] = {0, DC_WHITE,   DC_GREY,  DC_BROWN,  DC_GREY,  DC_WHITE,  DC_BROWN};
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

  // NEW: lives
  uint8_t   livesMax;      // configured max lives (default 5)
  uint8_t   livesLeft;     // current lives in this run

  uint16_t  okTotal;       // across all levels
  uint16_t  okLevel;       // within current level
  uint32_t  score;         // +100 * level per OK
  uint32_t  durationMs;    // game length
  uint32_t  startedAt;     // millis()
  uint32_t  endsAt;        // startedAt + durationMs
  uint16_t  genLevel;      // how many orders generated this level
} g_game = {
  /*phase*/GP_IDLE,
  /*level*/1,
  /*targetOK*/6,
  /*autoAdv*/false,
  /*livesMax*/5,       // default lives
  /*livesLeft*/5,      // initialized to livesMax at start
  /*okTotal*/0, /*okLevel*/0, /*score*/0,
  /*duration*/360000, /*startedAt*/0, /*endsAt*/0,
  /*genLevel*/0
};

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

// --- HOUSE MAPPING resend burst ---
// Level transitions require pushing new house identities (window/panel/sound).
// ESPNOW is lossy, so we resend the mapping a few times to ensure every house updates.
// HouseNode/HousePanel treat identical mapping as a no-op (dedupe), so resends are safe.

// --- GAME_STATE broadcaster (Central -> ALL) ---
// Player-interactive stations (HOUSE_NODE / ORDERS_NODE / PIZZA_NODE) should hard-disable inputs
// unless phase == Running.
static uint32_t g_gameStateDueAt = 0;
static uint8_t  g_gameStateBurstLeft = 0;

static void sendGameStateNow() {
  GameStatePayload p{};
  // Compute phase from Central + Engine (engine is source of truth while running).
  if (g_game.phase == GP_RUNNING && g_engine.phase() == PizzaGameEngine::Phase::Running) {
    p.phase = 1;
  } else if (g_game.phase == GP_OVER || g_engine.phase() == PizzaGameEngine::Phase::Over) {
    p.phase = 2;
  } else {
    p.phase = 0;
  }
  p.level = g_engine.level();
  p.rsv0 = 0; p.rsv1 = 0;

  uint8_t buf[64];
  size_t n = PizzaProtocol::pack(GAME_STATE, CENTRAL, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  if (n) PizzaNow::sendBroadcast(buf, n);
}

static void requestGameStateBurst(uint8_t count = 4) {
  g_gameStateBurstLeft = count;
  g_gameStateDueAt = millis();
}

static void gameStateTick() {
  const uint32_t now = millis();

  if (g_gameStateBurstLeft) {
    if ((int32_t)(now - g_gameStateDueAt) < 0) return;
    sendGameStateNow();
    g_gameStateBurstLeft--;
    g_gameStateDueAt = now + (g_gameStateBurstLeft ? 40 : 1000);
    return;
  }

  if ((int32_t)(now - g_gameStateDueAt) < 0) return;
  // Periodic keepalive in case any station missed the edge.
  sendGameStateNow();
  g_gameStateDueAt = now + 1000;
}

///// forward declarations /////
static void ordersPushWindow(uint8_t maxDisplay = 3);
static void ordersPushWindowDripStart(uint8_t maxDisplay, uint16_t startDelayMs = 0);

// Helpers
// Print status (now includes lives)
// Print status (engine-backed)
static void gamePrintStatus(){
  uint32_t now = millis();

  // phase + timer
  const bool running = (g_game.phase == GP_RUNNING) && (g_engine.phase() == PizzaGameEngine::Phase::Running);
  int32_t remaining = running ? (int32_t)(g_game.endsAt - now) : 0;
  if (remaining < 0) remaining = 0;

  uint8_t lvl = g_engine.level();
  uint16_t q  = quotaForLevel(lvl);

  DBG_PRINTF("game: phase=%s level=%u lives=%u/%u okLevel=%u/%u okTotal=%u timeLeft=%ldms\n",
    (running ? "RUNNING" : (g_game.phase==GP_IDLE ? "IDLE" : "OVER")),
    (unsigned)lvl,
    (unsigned)g_engine.livesLeft(), (unsigned)g_engine.livesMax(),
    (unsigned)g_engine.successInLevel(), (unsigned)q,
    (unsigned)g_engine.successTotal(),
    (long)remaining
  );
}

struct GameOverFx {
  bool     active;
  uint8_t  step;    // 0..N
  uint32_t nextAt;
} s_goFx = {false, 0, 0};

static void gameStart(uint16_t minutes, uint8_t level){
  // Level is owned by PizzaGameEngine; CLI can start at different levels for testing.
  // Minutes is the global run cap (players only reach full duration when mastered).
  if (minutes == 0) minutes = (uint16_t)(g_game.durationMs / 60000UL);
  if (minutes == 0) minutes = 6;

  const uint32_t now = millis();

  g_game.phase      = GP_RUNNING;
  g_game.durationMs = (uint32_t)minutes * 60UL * 1000UL;
  g_game.startedAt  = now;
  g_game.endsAt     = now + g_game.durationMs;

  // Reset ingredient cache so old pizzas don't carry into a new run
  ingredientsResetAll();

  // Clear any previous order window on the stations/panels
  ordersResetLocal();

  // Lives for this run (per-game lives)
  if (g_game.livesMax == 0) g_game.livesMax = 5;
  g_engine.setLivesMax(g_game.livesMax);

  // Stop any pending "game over" animation from a previous run
  s_goFx.active = false;

  // Start engine (build house identities + spawn initial orders)
  if (level < 1) level = 1;
  if (level > 5) level = 5;
  g_engine.startGameAtLevel((uint8_t)level, now);

  // Resend mapping a couple of times right after game start (covers occasional packet loss)
  requestGameStateBurst(6);

  // Broadcast first order window (OrdersStation will start countdown from remain_s)
  ordersPushWindowDripStart(/*maxDisplay*/3, /*delay*/0);

  // Let all player stations know the run is live (and repeat a few times for reliability).
  requestGameStateBurst(6);

  g_gameTickDueAt = now + 100;

  DBG_PRINTLN("game: START");
  gamePrintStatus();
}

static void housesAllOff(){
  // Turn off ALL house outputs when no game is running.
  // (We still allow per-delivery success/fail pulses handled locally by HouseNode.)
  for (uint8_t h=1; h<=6; ++h){
    sendHouseDigital(h,
      /*flags*/0x01 | 0x02 | 0x04,           // window + panel + speaker
      /*win*/WIN_FX_OFF, 0,0,0,0,
      /*panel*/PANEL_MODE_TEXT, "", 1,1,0, // blank + brightness 0
      /*spk*/0, 0, false, /*stopNow*/true
    );
  }
}

// Simple non-blocking game-over animation: all windows blink red a few times,
// then everything turns off and we return to IDLE.

static void gameOverFxStart() {
  s_goFx.active = true;
  s_goFx.step   = 0;
  s_goFx.nextAt = millis();
}

static void gameOverFxTick() {
  if (!s_goFx.active) return;
  const uint32_t now = millis();
  if ((int32_t)(now - s_goFx.nextAt) < 0) return;

  // 6 steps = 3 flashes (red/off)
  const bool on = ((s_goFx.step % 2) == 0);
  for (uint8_t h = 1; h <= 6; ++h) {
    sendHouseDigital(h,
      /*flags*/0x01 | 0x02 | 0x04,
      /*win*/on ? WIN_FX_SOLID : WIN_FX_OFF,
      /*h*/0, /*s*/255, /*v*/120, /*spd*/0,
      /*panel*/PANEL_MODE_TEXT, "", 1, 1, 0,
      /*spk*/0, 0, false, /*stopNow*/true
    );
  }

  s_goFx.step++;
  if (s_goFx.step >= 6) {
    // Finalize: full off and back to idle
    housesAllOff();
    s_goFx.active = false;
    g_game.phase = GP_IDLE;
    requestGameStateBurst(6);
    DBG_PRINTLN("game: OVER -> IDLE");
    gamePrintStatus();
  } else {
    s_goFx.nextAt = now + 160;
  }
}

static void gameStop(){
  if (g_game.phase == GP_IDLE) return;

  const uint32_t now = millis();

  g_game.phase = GP_OVER;

  // Stop engine + clear active orders
  g_engine.stopGame(now);

  // Immediately disable player inputs everywhere (houses/pizza/order stations).
  requestGameStateBurst(6);

  // Clear list on OrdersStation
  ordersPushWindowDripStart(/*maxDisplay*/3, /*delay*/0);

  // Start game over window animation (panels stay off)
  gameOverFxStart();

  // Also clear remembered toppings at game end
  ingredientsResetAll();

  DBG_PRINTLN("game: STOP");
  gamePrintStatus();
}

static void gameNextLevel(){
  // NOTE: With PizzaGameEngine, levels advance automatically based on per-level quotas.
  // Keeping this CLI hook as a debug affordance; for now it just prints state.
  DBG_PRINTLN("game next: (engine) levels auto-advance; use deliveries to progress");
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
  const uint32_t now = millis();
  if ((int32_t)(now - g_gameTickDueAt) < 0) return;
  g_gameTickDueAt = now + 100; // 10 Hz

  if (g_game.phase != GP_RUNNING) return;

  // Global run cap
  if ((int32_t)(now - g_game.endsAt) >= 0) {
    DBG_PRINTLN("game: TIME UP");
    #if PMS_STD_ENABLED
    s_pmsEndReasonHint = "timeup";
    #endif
    gameStop();
    return;
  }

  // Engine tick (handles per-order expiry + refills + auto level-up)
  const uint8_t levelBefore = g_engine.level();
  const bool changed = g_engine.tick(now);
  const uint8_t levelAfter  = g_engine.level();

  // If the engine advanced to a new level, resend mapping a few times.
  // (If a house misses this, it will still show the previous level identity and orders will look "wrong".)
  if (levelAfter != levelBefore) {
    DBG_PRINTF("[central] LEVEL CHANGE %u -> %u\n", (unsigned)levelBefore, (unsigned)levelAfter);
    requestGameStateBurst(6);
  }

  // If orders changed (expiry/refill/level-up), push window to OrdersStation
  if (changed) {
    ordersPushWindowDripStart(/*maxDisplay*/3, /*delay*/0);
  }

  // Engine might end the game (out of lives, completed final level)
  if (g_engine.phase() != PizzaGameEngine::Phase::Running) {
    DBG_PRINTLN("game: ENGINE END");
    #if PMS_STD_ENABLED
    s_pmsEndReasonHint = (g_engine.livesLeft() == 0) ? "no_lives" : "timeup";
    #endif
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
  DBG_PRINTF("[central] DRIP start: window=%u delay=%ums\n", win, (unsigned)startDelayMs);
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
    DBG_PRINTLN("[central] DRIP done");
    return;
  }

  // Update remaining seconds right before sending (v2 timers)
  g_engine.prepareOrderBroadcast(millis());

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

  DBG_PRINTF("orders: pool ->");
  for (int h=1; h<=6; ++h) DBG_PRINTF(" H%d=#%u", h, g_housePool[h]);
  DBG_PRINTLN();
}

static void ordersShowPool() {
  if (!g_housePool[1]) { DBG_PRINTLN("orders: pool is empty (run `orders pool reset`)"); return; }
  DBG_PRINTF("orders: pool ->"); for (int h=1; h<=6; ++h) DBG_PRINTF(" H%d=#%u", h, g_housePool[h]); DBG_PRINTLN();
}

static void housesNumbersShow() {
  if (!g_housePool[1]) { DBG_PRINTLN("houses: pool empty; run `orders pool reset` first"); return; }
  for (uint8_t h=1; h<=6; ++h) {
    char digits[8]; snprintf(digits, sizeof(digits), "%u", (unsigned)g_housePool[h]);
    // Panel only (flags=0x02). NOTE: 5 window fields -> 0,0,0,0,0
    sendHouseDigital(h, /*flags*/0x02,
                     /*win*/0,0,0,0,0,
                     /*panel*/PANEL_MODE_NUMBER, digits, 1,1,180,
                     /*spk*/0,0,false,false);
  }
  DBG_PRINTLN("houses: numbers pushed to panels");
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
  if (ok) DBG_PRINTF("orders: gen1 -> H%u mask=0x%02X text=\"%s\"\n", targetHouse, mask, clue);
  return ok;
}

static void ordersResetLocal() {
  g_orderCount = 0;
  for (uint8_t i=0;i<PZ_ORDERS_MAX;i++) {
    g_orders[i].index = i;
    g_orders[i].house_id = 0;
    g_orders[i].mask = 0;
    g_orders[i]._pad0 = 0;
    g_orders[i].order_id = 0;
    g_orders[i].remain_s = 0;
    memset(g_orders[i].text, 0, sizeof(g_orders[i].text));
  }
}

static bool ordersAddLocal(uint8_t house_id, uint8_t mask, const char* text) {
  if (g_orderCount >= PZ_ORDERS_MAX) return false;
  PzOrderItemSetPayload &it = g_orders[g_orderCount];
  it.index = g_orderCount;
  it.house_id = house_id;
  it.mask = mask;
  it._pad0 = 0;
  it.order_id = 0;
  it.remain_s = 0;
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

// Clear ALL remembered pizza ingredients and broadcast "empty" snapshots
static void ingredientsResetAll() {
  // Tell everyone each known UID is now empty
  for (uint8_t i = 0; i < g_tagCount; ++i) {
    PizzaIngrSnapshotPayload sp{};
    sp.uid_len = g_tags[i].len;
    memcpy(sp.uid, g_tags[i].uid, g_tags[i].len);
    sp.ok   = 1;
    sp.mask = 0;
    sendIngrSnapshot(sp);
  }
  // Wipe our cache
  g_tagCount = 0;
  DBG_PRINTLN("[central] ingredients: reset all (cache cleared and snapshots broadcast)");
}

static void printMask(uint8_t m) {
  if (m == MASK_NONE) { DBG_PRINT(F("(none)")); return; }
  DBG_PRINT(F("0x")); if (m < 16) DBG_PRINT('0'); DBG_PRINT(m, HEX);
  DBG_PRINT(F(" ["));
  DBG_PRINT((m &  1) ? 'P' : '.'); // Pepperoni
  DBG_PRINT((m &  2) ? 'M' : '.'); // Mushrooms
  DBG_PRINT((m &  4) ? 'p' : '.'); // Peppers
  DBG_PRINT((m &  8) ? 'i' : '.'); // Pineapple
  DBG_PRINT((m & 16) ? 'H' : '.'); // Ham
  DBG_PRINT(']');
}

// Handy printer
static void factsShow(){
  auto hc=[](uint8_t c){ return (c==HC_RED?"red":(c==HC_YELLOW?"yellow":"blue")); };
  auto dc=[](uint8_t c){ return (c==DC_BROWN?"brown":(c==DC_WHITE?"white":"grey")); };
  auto hs=[](uint8_t s){ return (s==HS_ROUND?"round":"bar"); };
  auto hk=[](uint8_t c){ return (c==HCOL_SILVER?"silver":"gold"); };
  for (uint8_t h=1; h<=6; ++h){
    DBG_PRINTF("H%u: house=%s door=%s handle=%s/%s beside={%u%s%s} across=%u\n",
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
  DBG_PRINTLN("boxes:");
  for (int i=0;i<6;i++){
    DBG_PRINTF("  %d: ", i+1);
    if (!g_box[i].set) { DBG_PRINTLN("(empty)"); continue; }
    for (uint8_t k=0;k<g_box[i].len;k++){ if (g_box[i].uid[k]<16) DBG_PRINT('0'); DBG_PRINT(g_box[i].uid[k], HEX); if (k+1<g_box[i].len) DBG_PRINT(':'); }
    DBG_PRINTLN();
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
  DBG_PRINTLN("boxes: saved to NVS");
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
    DBG_PRINTLN("boxes: loaded from NVS");
  } else {
    // Nothing stored yet → leave empty
    for (int i=0;i<6;i++){ g_box[i].set=false; g_box[i].len=0; }
    DBG_PRINTLN("boxes: NVS empty");
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
  DBG_PRINTLN(F("\nROLE          ID   FW        MAC                LAST_SEEN(ms)\n--------------------------------------------------------------"));
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
    DBG_PRINTF("%-12s  %2u   %-8s  %-17s  %9u  %7us\n",
      rn, g_devices[i].house_id, g_devices[i].fw, macbuf,
      (unsigned)last, (unsigned)(age/1000));
  }
  DBG_PRINTLN();
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

static void sendNetCfgSet(const char* ssid, const char* pass, const char* base) {
  NetCfgSetPayload p{};
  if (ssid) strlcpy(p.ssid, ssid, sizeof(p.ssid));
  if (pass) strlcpy(p.pass, pass, sizeof(p.pass));
  if (base) strlcpy(p.base, base, sizeof(p.base));
  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(NET_CFG_SET, CENTRAL, 0, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  DBG_PRINTF("[Central] NET_CFG_SET -> ssid=\"%s\" base=\"%s\"\n", p.ssid, p.base);
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
  DBG_PRINTF("[Central] ORDER_LIST_RESET count=%u\n", count);
}

static void ordersSendItem(const PzOrderItemSetPayload& item) {
  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(ORDER_ITEM_SET, CENTRAL, 0, g_seq_orders++, &item, sizeof(item), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  DBG_PRINTF("[Central] ORDER_ITEM_SET idx=%u H%u mask=0x%02X\n", item.index, item.house_id, item.mask);
}

/*** CENTRAL: push local list to the Orders Node(s) ***/
static void ordersPushAll() {
  // 1) RESET
  ordersSendReset(g_orderCount);
  delay(8); // small gap helps ESPNOW

  // 2) ITEMS
  g_engine.prepareOrderBroadcast(millis()); // v2 timers

  for (uint8_t i = 0; i < g_orderCount; ++i) {
    ordersSendItem(g_orders[i]);
    delay(6); // 4–10 ms is enough in practice
  }
}

static void ordersPushWindow(uint8_t maxDisplay) {
  uint8_t window = (g_orderCount < maxDisplay) ? g_orderCount : maxDisplay;

  ordersSendReset(window);
  delay(8); // small gap helps on ESPNOW

  g_engine.prepareOrderBroadcast(millis()); // v2 timers

  for (uint8_t i = 0; i < window; ++i) {
    PzOrderItemSetPayload it = g_orders[i]; // take first 'window' items
    it.index = i;                            // <— REBASE indices to 0..window-1
    ordersSendItem(it);
    delay(6);
  }
  DBG_PRINTF("[Central] Pushed window=%u of total=%u\n", window, g_orderCount);
}

/*** CENTRAL: send ORDER_SHOW_TEXT directly to the panel ***/
static void ordersSendShowText(const char* s) {
  PzOrderShowTextPayload p{};
  if (s && *s) strlcpy(p.text, s, sizeof(p.text));
  else         strcpy(p.text, "IDLE");

  uint8_t buf[256];
  size_t n = PizzaProtocol::pack(ORDER_SHOW_TEXT, CENTRAL, 0, g_seq_orders++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  DBG_PRINTF("[Central] ORDER_SHOW_TEXT len=%u\n", (unsigned)strlen(p.text));
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
  DBG_PRINTLN(F("[central] ordersApplyToValidator: validator masks updated"));
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
      DBG_PRINTLN("[Central] ING_UPDATE ignored: UID not whitelisted");
      return;
    }
    DBG_PRINTF("[Central] ING_UPDATE from id=%u mask=0x%02X\n", hdr.house_id, u->mask);
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
      DBG_PRINTF("boxes: learned slot %d -> ", g_learnSlot);
      for (uint8_t k=0;k<s->uid_len;k++){ if (s->uid[k]<16) DBG_PRINT('0'); DBG_PRINT(s->uid[k], HEX); if (k+1<s->uid_len) DBG_PRINT(':'); }
      DBG_PRINTLN();
      g_learnSlot = 0;
      boxesSave();
      // continue to normal validation
    }

    const uint32_t nowMs = millis();

    const bool running = (g_game.phase == GP_RUNNING) && (g_engine.phase() == PizzaGameEngine::Phase::Running);
    // When no game is running, houses/stations should be "inactive": always reject deliveries.
    if (!running) {
      sendDeliverResult(s->house_id, /*ok*/false, /*reason*/DR_NO_ORDER);
      return;
    }

    uint8_t reason = DR_OK;
    uint8_t tagMask = 0;

    // 1) must be whitelisted
    if (!isWhitelisted(s->uid, s->uid_len)) {
      reason = DR_UNKNOWN_PIZZA;
    } else {
      // 2) must have ingredient mask cached
      int tidx = tagFind(s->uid, s->uid_len);
      if (tidx < 0) {
        reason = DR_UNKNOWN_PIZZA;
      } else {
        tagMask = g_tags[tidx].mask;
      }
    }

    // 3) must match an active order (engine)
    if (reason == DR_OK) {
      int8_t oidx = g_engine.findActiveOrderIndexByHouse(s->house_id);
      if (oidx < 0) {
        reason = DR_NO_ORDER;
      } else if (!g_engine.validateToppingsForOrderIndex((uint8_t)oidx, tagMask)) {
        reason = DR_WRONG_PIZZA;
      }
    }

    // Tell the house the verdict
    sendDeliverResult(s->house_id, (reason == DR_OK), reason);

    // Apply outcome
    if (reason == DR_OK) {
      // Success: clear tag ingredients + advance engine
      ingrClearTag(s->uid, s->uid_len);

      if (g_game.phase == GP_RUNNING && g_engine.phase() == PizzaGameEngine::Phase::Running) {
        bool changed = g_engine.onDeliveryResult(s->house_id, /*ok*/true, reason, nowMs);
        if (changed) {
          ordersPushWindowDripStart(/*maxDisplay*/3, ORD_DRIP_START_DELAY_MS);
        }
        // If that delivery completed the game (final level), stop cleanly
        if (g_engine.phase() != PizzaGameEngine::Phase::Running) {
          #if PMS_STD_ENABLED
          s_pmsEndReasonHint = (g_engine.livesLeft() == 0) ? "no_lives" : "timeup";
          #endif
          gameStop();
        }
      }

    } else {
      // Wrong delivery costs a life only when a run is active
      bool changed = g_engine.onDeliveryResult(s->house_id, /*ok*/false, reason, nowMs);
      if (changed) {
        ordersPushWindowDripStart(/*maxDisplay*/3, ORD_DRIP_START_DELAY_MS);
      }
      if (g_engine.phase() != PizzaGameEngine::Phase::Running) {
        #if PMS_STD_ENABLED
        s_pmsEndReasonHint = (g_engine.livesLeft() == 0) ? "no_lives" : "timeup";
        #endif
        gameStop();
      }
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
    DBG_PRINTF("[Central] ASSET_RESULT H%u ok=%u count=%u code=%u\n",
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

// -------------------- PMS STANDARD SERIAL (v1) --------------------
#if PMS_STD_ENABLED

// PMS state tracking for delta/events
static uint32_t s_pmsLastTickMs = 0;
static bool     s_pmsBaselineValid = false;
static bool     s_pmsWasRunning = false;
static uint8_t  s_pmsLastLevel = 1;
static uint16_t s_pmsLastScore = 0;
static uint8_t  s_pmsLastLives = 0;

// NOTE: Pizza currently has no explicit "arming" phase; we report state=playing while running.
static const char* pmsStateStr(bool running) {
  return running ? "playing" : "idle";
}

static const char* pmsLastReasonStr(bool stateChanged, int32_t scoreDelta, int32_t livesDelta) {
  if (livesDelta < 0) return "life";
  if (scoreDelta > 0) return "score";
  if (stateChanged)   return "state";
  return "none";
}

static void pmsPrintPong() {
  Serial.println(F("!PMS PONG v=1 game=pizza role=server"));
}

static void pmsPrintEventGameStart(uint8_t level) {
  Serial.print(F("!PMS EVENT v=1 name=game_start level="));
  Serial.println(level);
}

static void pmsPrintEventGameEnd(const char* reason, uint16_t score, uint8_t lives) {
  Serial.print(F("!PMS EVENT v=1 name=game_end reason="));
  Serial.print(reason);
  Serial.print(F(" score="));
  Serial.print(score);
  Serial.print(F(" lives="));
  Serial.println(lives);
}

static void pmsPrintEventScore(int32_t delta, uint16_t total) {
  Serial.print(F("!PMS EVENT v=1 name=score delta="));
  Serial.print(delta);
  Serial.print(F(" total="));
  Serial.print(total);
  Serial.println(F(" bonus=0"));
}

static void pmsPrintEventLife(int32_t delta, uint8_t lives) {
  Serial.print(F("!PMS EVENT v=1 name=life delta="));
  Serial.print(delta);
  Serial.print(F(" lives="));
  Serial.println(lives);
}

static void pmsPrintStatus(const char* state, uint8_t level, uint16_t score, uint8_t lives, uint32_t tleftMs, const char* lastReason) {
  Serial.print(F("!PMS STATUS v=1 state="));
  Serial.print(state);
  Serial.print(F(" level="));
  Serial.print(level);
  Serial.print(F(" score="));
  Serial.print(score);
  Serial.print(F(" lives="));
  Serial.print(lives);
  Serial.print(F(" tleft_ms="));
  Serial.print(tleftMs);
  Serial.print(F(" last_reason="));
  Serial.println(lastReason);
}

// Simple key=value int parser from a command line (e.g. "START level=2")
static int32_t pmsParseKeyInt(const String& s, const char* key, int32_t defVal) {
  String pat = String(key) + "=";
  int idx = s.indexOf(pat);
  if (idx < 0) return defVal;
  idx += pat.length();
  int end = idx;
  while (end < (int)s.length()) {
    char c = s.charAt(end);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
    end++;
  }
  String v = s.substring(idx, end);
  v.trim();
  if (!v.length()) return defVal;
  return v.toInt();
}

// PMS command handler:
//   !PMS PING
//   !PMS START level=1
//   !PMS STOP
static bool handlePmsLine(const String& rawLine) {
  String line = rawLine;
  line.trim();
  if (!line.startsWith("!PMS")) return false;

  // Strip prefix
  String rest = line.substring(4);
  rest.trim();
  if (!rest.length()) return true;

  // KIND token
  int sp = rest.indexOf(' ');
  String kind = (sp >= 0) ? rest.substring(0, sp) : rest;
  String args = (sp >= 0) ? rest.substring(sp + 1) : "";
  kind.toUpperCase();
  args.trim();

  if (kind == "PING") {
    pmsPrintPong();
    return true;
  }

  if (kind == "START") {
    int lvl = (int)pmsParseKeyInt(args, "level", 1);
    if (lvl < 1) lvl = 1;
    if (lvl > 5) lvl = 5;

    // Starting a run clears any previous stop reason hint
    s_pmsEndReasonHint = nullptr;

    // Start with default minutes (0 -> use configured default)
    gameStart(0, (uint8_t)lvl);
    return true;
  }

  if (kind == "STOP") {
    // Hint end reason for PMS (used when we detect the transition)
    s_pmsEndReasonHint = "stopped";
    gameStop();
    return true;
  }

  // Unknown PMS command: ignore silently
  return true;
}

// Emit STATUS + derived EVENTS at 250ms cadence while running.
// STATUS is NOT emitted while idle.
static void pmsTick() {
  const uint32_t now = millis();
  if ((uint32_t)(now - s_pmsLastTickMs) < (uint32_t)PMS_STATUS_PERIOD_MS) return;
  s_pmsLastTickMs = now;

  const bool running = (g_game.phase == GP_RUNNING) && (g_engine.phase() == PizzaGameEngine::Phase::Running);

  // Snapshot the engine values. Note: these remain valid even right after a run ends,
  // and we must use them to report the FINAL lives/score (including the last life loss).
  const uint8_t  engLevel    = g_engine.level();
  const uint16_t engScore    = (uint16_t)g_engine.successTotal();
  const uint8_t  engLives    = g_engine.livesLeft();
  const uint8_t  engLivesMax = (uint8_t)g_engine.livesMax();

  // If the run ended between PMS ticks, `running` is already false.
  // We still want to emit the final life/score deltas AND game_end with the true final values.
  const bool treatAsFinalSnapshot = (s_pmsBaselineValid && s_pmsWasRunning && !running);

  const uint8_t  curLevel = (running || treatAsFinalSnapshot) ? engLevel : 1;
  const uint16_t curScore = (running || treatAsFinalSnapshot) ? engScore : 0;
  const uint8_t  curLives = (running || treatAsFinalSnapshot) ? engLives : engLivesMax;

  uint32_t tleftMs = 0;
  if (running) {
    int32_t rem = (int32_t)(g_game.endsAt - now);
    if (rem < 0) rem = 0;
    tleftMs = (uint32_t)rem;
  }

  if (!s_pmsBaselineValid) {
    s_pmsBaselineValid = true;
    s_pmsWasRunning = running;
    s_pmsLastLevel = curLevel;
    s_pmsLastScore = curScore;
    s_pmsLastLives = curLives;

    // Do not emit events on first tick.
    if (running) {
      pmsPrintStatus(pmsStateStr(true), curLevel, curScore, curLives, tleftMs, "none");
    }
    return;
  }

  // Detect transitions
  bool stateChanged = (running != s_pmsWasRunning);
  int32_t scoreDelta = (int32_t)curScore - (int32_t)s_pmsLastScore;
  int32_t livesDelta = (int32_t)curLives - (int32_t)s_pmsLastLives;

  // Transition -> events
  if (stateChanged) {
    if (!s_pmsWasRunning && running) {
      pmsPrintEventGameStart(curLevel);
    } else if (s_pmsWasRunning && !running) {
      // IMPORTANT: the last life loss often happens on the same tick as the game ends.
      // Emit final deltas BEFORE game_end so the PMS doesn't "miss" the last life.
      if (scoreDelta > 0) {
        pmsPrintEventScore(scoreDelta, curScore);
      }
      if (livesDelta < 0) {
        pmsPrintEventLife(livesDelta, curLives);
      }

      const char* reason = s_pmsEndReasonHint;
      if (!reason) {
        // Best-effort fallback
        if (curLives == 0) reason = "no_lives";
        else if ((int32_t)(now - g_game.endsAt) >= 0) reason = "timeup";
        else reason = "stopped";
      }
      pmsPrintEventGameEnd(reason, curScore, curLives);
      s_pmsEndReasonHint = nullptr;
    }
  }

  // Ongoing run -> score/life events
  if (running && s_pmsWasRunning) {
    if (scoreDelta > 0) {
      pmsPrintEventScore(scoreDelta, curScore);
    }
    if (livesDelta < 0) {
      pmsPrintEventLife(livesDelta, curLives);
    }
  }

  // STATUS only while running
  if (running) {
    const char* lastReason = pmsLastReasonStr(stateChanged, scoreDelta, livesDelta);
    pmsPrintStatus(pmsStateStr(true), curLevel, curScore, curLives, tleftMs, lastReason);
  }

  // Update baseline
  s_pmsWasRunning = running;
  if (running) {
    s_pmsLastLevel = curLevel;
    s_pmsLastScore = curScore;
    s_pmsLastLives = curLives;
  } else {
    // reset stored values so next run doesn't emit spurious deltas
    s_pmsLastLevel = 1;
    s_pmsLastScore = 0;
    s_pmsLastLives = engLivesMax;
  }
}

#else
static void pmsTick() { }
static bool handlePmsLine(const String&) { return false; }
#endif

/*** Central CLI: help printer ***/
static void printHelp() {
  DBG_PRINTLN(F("=== Central CLI Help ==="));
  DBG_PRINTLN();

  // General
  DBG_PRINTLN(F("General"));
  DBG_PRINTLN(F("  help | ?                       Show this help"));
  DBG_PRINTLN(F("  list                           Print device roster"));
  DBG_PRINTLN(F("  hello-req                      Ask nodes to send HELLO"));
  DBG_PRINTLN();

  // Boxes (pizza box UID management)
  DBG_PRINTLN(F("Boxes (pizza box UID management)"));
  DBG_PRINTLN(F("  boxes show                     Show current 1..6 box UID slots"));
  DBG_PRINTLN(F("  boxes clear [n]                Clear all or slot n (1..6)"));
  DBG_PRINTLN(F("  boxes set <n> <UIDhex>         Set slot n with UID (colons ok)"));
  DBG_PRINTLN(F("  boxes learn <n>                Learn next scanned tag into slot n"));
  DBG_PRINTLN(F("  boxes reload                   Reload from NVS and show"));
  DBG_PRINTLN();

  // Facts
  DBG_PRINTLN(F("Facts"));
  DBG_PRINTLN(F("  facts show                     Print internal constants/config"));
  DBG_PRINTLN();

  // Panels
  DBG_PRINTLN(F("Panels"));
  DBG_PRINTLN(F("  panel <id> \"text\" [style] [speed] [bright]"));
  DBG_PRINTLN(F("      style: 0=scroll, 1=static; speed: 1..5; bright: 0..255"));
  DBG_PRINTLN();

  // Sound
  DBG_PRINTLN(F("Sound"));
  DBG_PRINTLN(F("  sound <id> [clip] [vol]        Play clip on house speaker (vol 0..255)"));
  DBG_PRINTLN();

  // Scenes & Assets
  DBG_PRINTLN(F("Scenes & Assets"));
  DBG_PRINTLN(F("  scene <id> party|stop|number <digits>"));
  DBG_PRINTLN(F("  clips sync <id|all> <base_url> <count>"));
  DBG_PRINTLN();

  // OTA / Updates
  DBG_PRINTLN(F("OTA / Updates"));
  DBG_PRINTLN(F("  update <ROLE> all|id=<n> [url]"));
  DBG_PRINTLN(F("      ROLE: HOUSE_PANEL | HOUSE_NODE | ORDERS_PANEL | ORDERS_NODE | PIZZA_NODE | CENTRAL"));
  DBG_PRINTLN();

  // Claiming
  DBG_PRINTLN(F("Claiming"));
  DBG_PRINTLN(F("  claim <MAC> <id> [force]       ex: claim AA:BB:CC:DD:EE:FF 3"));
  DBG_PRINTLN();

  // Game
  DBG_PRINTLN(F("Game"));
  DBG_PRINTLN(F("  game start [minutes] [level]   Start a timed run (level defaults to 1; use for testing)"));
  DBG_PRINTLN(F("  game stop                      End game and clear orders"));
  DBG_PRINTLN(F("  game status                    Print current game state"));
  DBG_PRINTLN(F("  game next                      Debug: print status (levels auto-advance)"));
  DBG_PRINTLN(F("  (levels auto-advance via engine quotas; no target/auto knobs in CLI yet)"));
  // (engine handles auto-advance)

  DBG_PRINTLN(F("  game minutes <n>               Set default run duration (minutes)"));
  DBG_PRINTLN();

  // Delivery Validator (per-house mask)
  DBG_PRINTLN(F("Delivery Validator (per-house mask)"));
  DBG_PRINTLN(F("  order <id> <mask>              Set validator mask (0..31 or 0xNN)"));
  DBG_PRINTLN(F("  order show [id]                Show validator(s)"));
  DBG_PRINTLN(F("  order clear <id>               Clear validator"));
  DBG_PRINTLN(F("  tags                           List recent tag->mask cache"));
  DBG_PRINTLN();

  // Operator Orders (Orders Node/Panel list)
  DBG_PRINTLN(F("Operator Orders (Orders Node/Panel list)"));
  DBG_PRINTLN(F("  orders reset                   Clear local list (max 6 items)"));
  DBG_PRINTLN(F("  orders add <house> <mask> \"text\""));
  DBG_PRINTLN(F("  orders show                    Print local list"));
  DBG_PRINTLN(F("  orders push                    Broadcast list to Orders Node (+apply)"));
  DBG_PRINTLN(F("  orders apply                   Apply current list to validators"));
  DBG_PRINTLN(F("  orders pool reset              Generate new 6-number house pool"));
  DBG_PRINTLN(F("  orders pool show               Show current house numbers"));
  DBG_PRINTLN(F("  orders gen1                    Auto-add one L1 order (with clue)"));
  DBG_PRINTLN(F("  orders showidx <n>             Display item n on Orders Panel"));
  DBG_PRINTLN(F("  orders show \"text\"            Display ad-hoc text on Orders Panel"));
  DBG_PRINTLN();

  // Network
  DBG_PRINTLN(F("Network"));
  DBG_PRINTLN(F("  net show                      Show current SSID/PASS/BASE (central-side)"));
  DBG_PRINTLN(F("  net set \"ssid\" \"pass\" \"base\"  Update central-side values"));
  DBG_PRINTLN(F("  net push                      Broadcast SSID/PASS/BASE to all nodes"));
  DBG_PRINTLN();
}

void setup() {
  Serial.begin(115200); delay(100);
  PZ_LOGI("Central boot fw=%s mac=%s", PizzaIdentity::fw(), PizzaIdentity::macStr().c_str());

  // Radio
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);

  stateInit();
  boxesLoad();

  // Engine init (binds ORDER_ITEM_SET payload buffer)
  PizzaGameEngine::IO io{};
  io.sendHouseDigital = sendHouseDigital;
  io.rand32 = []()->uint32_t { return esp_random(); };
  g_engine.begin(io);
  g_engine.bindOrdersBuffer(g_orders, PZ_ORDERS_MAX, &g_orderCount);

  // Idle baseline: houses dark + panels off, stations show no orders.
  ordersResetLocal();
  housesAllOff();
  ordersPushWindowDripStart(/*maxDisplay*/3, /*delay*/0);
  requestGameStateBurst(6);

  // Help
  DBG_PRINTLN(F("Type 'help' for commands."));
}

void loop() {
  PizzaNow::loop();

  // Non-blocking game-over window animation (if active)
  gameOverFxTick();

  gameTick();
  ordersPushWindowDripTick();
  gameStateTick();

  pmsTick();

  String line = readLine();
  if (!line.length()) return;
  line.trim();

#if PMS_STD_ENABLED
  if (line.startsWith("!PMS")) { handlePmsLine(line); return; }
#endif


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

    // Load runtime base URL (editable via net set / net push)
    NetCfg::Value net{};
    NetCfg::load(net);
    String base = String(net.base);

    const char* rel = nullptr;
    if (role==HOUSE_PANEL)  rel = OTA_REL_HOUSE_PANEL;
    else if (role==HOUSE_NODE)  rel = OTA_REL_HOUSE_NODE;
    else if (role==ORDERS_PANEL)rel = OTA_REL_ORDERS_PANEL;
    else if (role==ORDERS_NODE) rel = OTA_REL_ORDERS_NODE;
    else if (role==PIZZA_NODE)  rel = OTA_REL_PIZZA_NODE;
    else if (role==CENTRAL)     rel = OTA_REL_CENTRAL;

    String urlOverride;
    int sp=target.indexOf(' ');
    if (!all) {
      int eq=target.indexOf('='); if (eq>0) id=parseInt(target.substring(eq+1, sp>0?sp:target.length()),1);
      if (sp>0) urlOverride = target.substring(sp+1);
    } else {
      if (sp>0) urlOverride = target.substring(sp+1);
    }
    urlOverride.trim();

    String defUrl = base + rel;                    // <- runtime base + role-specific .bin
    String url = urlOverride.length()? urlOverride : defUrl;
    sendOtaStart(role, all, (uint8_t)id, url.c_str());
    return;
  }

  if (line.startsWith("claim ")) {
    // claim AA:BB:CC:DD:EE:FF 3 [force]
    int s1=line.indexOf(' '); if (s1<0) return;
    String rest=line.substring(s1+1); rest.trim();
    int s2=rest.indexOf(' ');
    if (s2<0){ DBG_PRINTLN(F("usage: claim <MAC> <id> [force]")); return; }
    String macStr=rest.substring(0,s2);
    String rest2=rest.substring(s2+1); rest2.trim();
    int s3=rest2.indexOf(' ');
    String idStr = (s3>=0)? rest2.substring(0,s3) : rest2;
    int id = idStr.toInt();
    bool force = (s3>=0) ? (rest2.substring(s3+1)=="force") : false;

    uint8_t mac[6];
    if (!parseMac(macStr, mac) || id<=0) { DBG_PRINTLN(F("usage: claim <MAC> <id> [force]")); return; }
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
        if (id < 0 || id > 255) { DBG_PRINTLN(F("usage: order show [id]")); return; }
        DBG_PRINT(F("order[")); DBG_PRINT(id); DBG_PRINT(F("]=")); printMask(g_orderMask[id]); DBG_PRINTLN();
      } else {
        for (int i = 0; i < 256; ++i) if (g_orderMask[i] != MASK_NONE) {
          DBG_PRINT(F("order[")); DBG_PRINT(i); DBG_PRINT(F("]=")); printMask(g_orderMask[i]); DBG_PRINTLN();
        }
      }
      return;
    }

    if (rest.startsWith("clear")) {
      rest = rest.substring(5); rest.trim();
      int id = rest.toInt();
      if (id < 0 || id > 255) { DBG_PRINTLN(F("usage: order clear <id>")); return; }
      g_orderMask[id] = MASK_NONE;
      DBG_PRINTF("order[%d] cleared\n", id);
      return;
    }

    // order <id> <mask>
    int space = rest.indexOf(' ');
    if (space < 0) { DBG_PRINTLN(F("usage: order <id> <mask>  (mask: 0..31 or 0xNN)")); return; }
    int id = rest.substring(0, space).toInt();
    String mstr = rest.substring(space + 1); mstr.trim();
    long mask;
    if (mstr.startsWith("0x") || mstr.startsWith("0X")) mask = strtol(mstr.c_str(), nullptr, 16);
    else mask = mstr.toInt();
    if (id < 0 || id > 255 || mask < 0 || mask > 31) { DBG_PRINTLN(F("usage: order <id> <mask>")); return; }
    g_orderMask[id] = (uint8_t)mask;
    DBG_PRINT(F("order[")); DBG_PRINT(id); DBG_PRINT(F("]=")); printMask(g_orderMask[id]); DBG_PRINTLN();
    return;
  }

  if (line == "tags") {
    DBG_PRINTF("tags (%u)\n", g_tagCount);
    for (uint8_t i = 0; i < g_tagCount; ++i) {
      DBG_PRINTF("#%u ", i);
      for (uint8_t k = 0; k < g_tags[i].len; ++k) {
        if (g_tags[i].uid[k] < 16) DBG_PRINT('0');
        DBG_PRINT(g_tags[i].uid[k], HEX);
        if (k + 1 < g_tags[i].len) DBG_PRINT(':');
      }
      DBG_PRINT(F(" → ")); printMask(g_tags[i].mask); DBG_PRINTLN();
    }
    return;
  }

  /*** CENTRAL CLI: orders ... ***/
  if (line.startsWith("orders ")) {
    String rest = line.substring(7); rest.trim();

    // New: number pool for houses (6 consecutive numbers 1..99)
    if (rest == "pool reset") {
      ordersAssignNumbers(esp_random());
      DBG_PRINTLN("orders: pool reset");
      housesNumbersShow();              // <— push digits to panels automatically
      return;
    }
    if (rest == "pool show")  { ordersShowPool(); return; }

    // Existing: reset/show/push
    if (rest == "reset") { ordersResetLocal(); DBG_PRINTLN("orders: reset"); return; }

    if (rest == "show") {
      DBG_PRINTF("orders: count=%u\n", g_orderCount);
      for (uint8_t i=0;i<g_orderCount;i++) {
        DBG_PRINTF("  [%u] H%u mask=0x%02X text=\"%s\"\n",
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
      if (!ordersGenLevel1()) { DBG_PRINTLN("orders: list full (max 6)"); return; }

      // Arm the per-house validator for this order
      const auto &it = g_orders[g_orderCount-1];
      g_orderMask[it.house_id] = it.mask;

      // Push list to Orders Node/Panel; Order Station UI handles display
      ordersPushAll();

      // Optional: ensure panels are showing the numbers (safe to resend)
      housesNumbersShow();

      DBG_PRINTF("orders: armed H%u with mask=0x%02X; clue in list\n", it.house_id, it.mask);
      return;
    }

    // Existing: orders add <house_id> <mask> "text..."
    if (rest.startsWith("add ")) {
      rest = rest.substring(4);
      int sp = rest.indexOf(' ');
      if (sp < 0) { DBG_PRINTLN("usage: orders add <house_id> <mask> \"text\""); return; }
      int house = rest.substring(0, sp).toInt();

      String rest2 = rest.substring(sp+1); rest2.trim();
      sp = rest2.indexOf(' ');
      if (sp < 0) { DBG_PRINTLN("usage: orders add <house_id> <mask> \"text\""); return; }

      String maskStr = rest2.substring(0, sp);
      String textStr = rest2.substring(sp+1); textStr.trim();
      if (textStr.length() && textStr[0]=='\"' && textStr[textStr.length()-1]=='\"') {
        textStr = textStr.substring(1, textStr.length()-1);
      }

      long mask = (maskStr.startsWith("0x")||maskStr.startsWith("0X")) ? strtol(maskStr.c_str(), nullptr, 16)
                                                                      : maskStr.toInt();
      if (house < 1 || house > 6 || mask < 0 || mask > 31) {
        DBG_PRINTLN("usage: orders add <house_id 1..6> <mask 0..31|0xNN> \"text\"");
        return;
      }
      if (!ordersAddLocal((uint8_t)house, (uint8_t)mask, textStr.c_str())) {
        DBG_PRINTLN("orders: list full (max 6)");
        return;
      }
      DBG_PRINTLN("orders: added");
      return;
    }

    // Existing: orders showidx <n>  -> send text of item n to panel
    if (rest.startsWith("showidx ")) {
      int n = rest.substring(8).toInt();
      if (n < 0 || n >= g_orderCount) { DBG_PRINTLN("usage: orders showidx <0..count-1>"); return; }
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
      DBG_PRINTF("orders auto-next L1 = %s\n", g_autoNextL1 ? "ON" : "OFF");
      return;
    }

    DBG_PRINTLN("orders commands: reset | show | push | add <house> <mask> \"text\" | showidx <n> | show \"text\" | pool reset | pool show | gen1");
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
    if (sp1<0 || sp2<0) { DBG_PRINTLN(F("usage: clips sync <id> <base_url> <count>")); return; }

    int id = rest.substring(0,sp1).toInt();
    String base = rest.substring(sp1+1, sp2);
    int count = rest.substring(sp2+1).toInt(); if (count < 1) count = 1; if (count > 30) count = 30;

    AssetSyncPayload ap{}; ap.house_id = (uint8_t)id;
    strlcpy(ap.base_url, base.c_str(), sizeof(ap.base_url)); ap.count = (uint8_t)count;

    uint8_t out[192]; size_t n = PizzaProtocol::pack(ASSET_SYNC, CENTRAL, 0, g_seq++, &ap, sizeof(ap), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);
    DBG_PRINTF("ASSET_SYNC -> id=%d url=%s count=%d\n", id, ap.base_url, ap.count);
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
    DBG_PRINTLN("boxes: cleared");
    boxesSave();
    return;
  }

  // boxes set <n> <uidHex>
  if (line.startsWith("boxes set ")){
    String rest = line.substring(10); rest.trim();
    int sp = rest.indexOf(' ');
    if (sp<0){ DBG_PRINTLN("usage: boxes set <1..6> <UIDhex>"); return; }
    int slot = rest.substring(0,sp).toInt();
    String hex = rest.substring(sp+1); hex.trim();
    if (slot<1||slot>6){ DBG_PRINTLN("usage: boxes set <1..6> <UIDhex>"); return; }
    uint8_t buf[10], len=0;
    if (!parseUidHex(hex, buf, len)){ DBG_PRINTLN("boxes set: bad UID"); return; }
    g_box[slot-1].set=true; g_box[slot-1].len=len; memcpy(g_box[slot-1].uid, buf, len);
    DBG_PRINTF("boxes: set slot %d\n", slot);
    boxesSave();
    return;
  }

  // boxes learn <n>   -> next scanned tag will populate slot n
  if (line.startsWith("boxes learn ")){
    int slot = line.substring(12).toInt();
    if (slot<1||slot>6){ DBG_PRINTLN("usage: boxes learn <1..6>"); return; }
    g_learnSlot = slot;
    DBG_PRINTF("boxes: learning slot %d (scan a pizza box tag once)\n", slot);
    return;
  }

  if (line == "boxes reload"){ boxesLoad(); boxesShow(); return; }
  if (line == "facts show"){ factsShow(); return; }

  // game start [minutes] [level]
  if (line.startsWith("game start")){
    String rest = line.substring(10); rest.trim();
    int minutes = (int)(g_game.durationMs / 60000UL);
    if (minutes <= 0) minutes = 6;
    int lvl = 1;
    if (rest.length()){
      int sp = rest.indexOf(' ');
      if (sp<0) { minutes = rest.toInt(); }
      else { minutes = rest.substring(0,sp).toInt(); lvl = rest.substring(sp+1).toInt(); }
    }
    if (minutes <= 0) minutes = 6;
    if (lvl <= 0)     lvl = 1;
    gameStart((uint16_t)minutes, (uint8_t)lvl);
    return;
  }
  #if PMS_STD_ENABLED
  if (line == "game stop"){ s_pmsEndReasonHint = "stopped"; gameStop(); return; }
#else
  if (line == "game stop"){ gameStop(); return; }
#endif
  if (line == "game status"){ gamePrintStatus(); return; }
  // game next  -> advance to next level (manual)
  if (line == "game next"){ gameNextLevel(); return; }
  // game target <n>  -> how many OKs to consider a level "complete"
  if (line.startsWith("game target ")){ DBG_PRINTLN("game target: handled by engine quotas (no-op)"); gamePrintStatus(); return; }
  // game auto on|off -> auto advance level when target reached
  if (line.startsWith("game auto ")){ DBG_PRINTLN("game auto: engine always auto-advances (no-op)"); gamePrintStatus(); return; }
  // game minutes <n> -> change duration for next start
  if (line.startsWith("game minutes ")){ int m=line.substring(13).toInt(); if (m>0) g_game.durationMs=(uint32_t)m*60UL*1000UL; return; }
  // net show
  if (line == "net show") {
    DBG_PRINTF("net: ssid=\"%s\"\n", g_net_ssid);
    DBG_PRINTF("net: pass=\"%s\"\n", g_net_pass);
    DBG_PRINTF("net: base=\"%s\"\n", g_net_base);
    return;
  }

  // net set "<ssid>" "<pass>" "<base_url>"
  if (line.startsWith("net set ")) {
    String rest = line.substring(8);
    rest.trim();

    // Expect 3 quoted strings: "ssid" "pass" "base"
    auto usage = [](){ DBG_PRINTLN(F("usage: net set \"<ssid>\" \"<pass>\" \"<base_url>\"")); };

    String s1, s2, s3;

    // 1) SSID
    int q1 = rest.indexOf('"'); if (q1 < 0) { usage(); return; }
    int q2 = rest.indexOf('"', q1 + 1); if (q2 < 0) { usage(); return; }
    s1 = rest.substring(q1 + 1, q2);
    rest = rest.substring(q2 + 1); rest.trim();

    // 2) PASS
    q1 = rest.indexOf('"'); if (q1 < 0) { usage(); return; }
    q2 = rest.indexOf('"', q1 + 1); if (q2 < 0) { usage(); return; }
    s2 = rest.substring(q1 + 1, q2);
    rest = rest.substring(q2 + 1); rest.trim();

    // 3) BASE URL
    q1 = rest.indexOf('"'); if (q1 < 0) { usage(); return; }
    q2 = rest.indexOf('"', q1 + 1); if (q2 < 0) { usage(); return; }
    s3 = rest.substring(q1 + 1, q2);

    if (!s1.length() || !s2.length() || !s3.length()) { usage(); return; }

    strlcpy(g_net_ssid, s1.c_str(), sizeof(g_net_ssid));
    strlcpy(g_net_pass, s2.c_str(), sizeof(g_net_pass));
    strlcpy(g_net_base, s3.c_str(), sizeof(g_net_base));
    DBG_PRINTLN(F("net: updated local values (use `net push` to broadcast)"));
    return;
  }

  // net push
  if (line == "net push") {
    sendNetCfgSet(g_net_ssid, g_net_pass, g_net_base);
    return;
  }


}
