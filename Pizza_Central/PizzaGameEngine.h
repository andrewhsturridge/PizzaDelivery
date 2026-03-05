#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "PizzaProtocol.h"

// -----------------------------
// PizzaGameEngine (Central-side)
// -----------------------------
// Owns:
// - Level ladder + mapping presets
// - Always-on house identities (panel/window/sound)
// - Active orders (quota-based progression)
// - Per-order timers + expiry -> life loss
// - Clue generation (single-domain; composites only when needed; relations optional for L5)
//
// Designed so Pizza_Central.ino can remain mostly as-is:
// - Central provides an IO callback to send HOUSE_DIGITAL_SET
// - Engine writes ORDER_ITEM_SET payloads into Central's g_orders[] buffer
// - Central keeps its existing "orders drip" broadcaster

class PizzaGameEngine {
public:
  static constexpr uint8_t kHouseCount = 6;      // house IDs 1..6
  static constexpr uint8_t kMaxOrders  = PZ_ORDERS_MAX;

  // ----- Core concepts -----
  enum class Phase : uint8_t { Idle=0, Running=1, Over=2 };

  enum class Domain : uint8_t { Panel=0, Window=1, Sound=2, Physical=3 };

  enum class PizzaTier : uint8_t {
    Single = 0,       // 1 topping
    Multi2 = 1,       // 2 toppings
    Multi3 = 2,       // 3 toppings
    Constraints = 3   // constraints-only (require + forbid masks)
  };

  enum class RelOp : uint8_t { LeftOf=0, RightOf=1, Opposite=2 };

  // ----- Physical facts (public so .cpp helpers can reference tokens) -----
  enum PHouseColor : uint8_t { HC_RED=0, HC_YELLOW=1, HC_BLUE=2 };
  enum PDoorColor  : uint8_t { DC_BROWN=0, DC_WHITE=1, DC_GREY=2 };
  enum PHandleShape: uint8_t { HS_ROUND=0, HS_BAR=1 };
  enum PHandleColor: uint8_t { HCOL_SILVER=0, HCOL_GOLD=1 };

  struct PhysicalFacts {
    uint8_t houseColor=0, doorColor=0, handleShape=0, handleColor=0;
  };

  // ----- IO callbacks (Central provides these) -----
  using SendHouseDigitalFn =
    void (*)(uint8_t houseId, uint8_t flags,
             // window
             uint8_t fx, uint8_t h, uint8_t s, uint8_t v, uint8_t fxSpeed,
             // panel
             uint8_t panelMode, const char* panelText, uint8_t panelStyle, uint8_t panelSpeed, uint8_t panelBright,
             // speaker
             uint8_t clip, uint8_t vol, bool loop, bool stopNow);

  struct IO {
    SendHouseDigitalFn sendHouseDigital = nullptr;  // REQUIRED
    uint32_t (*rand32)() = nullptr;                 // optional (defaults to esp_random)
  };

  // ----- Order constraints (engine-owned) -----
  struct OrderRules {
    uint8_t requireMask = 0;   // toppings that MUST be ON
    uint8_t forbidMask  = 0;   // toppings that MUST be OFF
    bool    exactMask   = true;// if true: actualMask must equal requireMask (and forbidMask ignored)
  };

  // ----- Active order state (engine internal) -----
  struct OrderState {
    bool     used = false;
    uint16_t orderId = 0;          // stable within a run
    uint8_t  targetHouse = 0;      // 1..6

    // Toppings / validation
    OrderRules rules{};

    // Timer
    uint32_t createdAtMs = 0;
    uint32_t expiresAtMs = 0;

    // Display / clue signature
    char clueText[PZ_ORDER_TEXT_MAX] = {0};   // what players see on Orders Panel
    char clueSig[64] = {0};                  // uniqueness signature (not shown)
  };

  // ----- House identity state (engine internal) -----
  struct HouseIdentity {
    // Panel
    uint8_t panelMode  = PANEL_MODE_NUMBER;
    char    panelText[24] = {0};
    uint8_t panelStyle = 1;
    uint8_t panelSpeed = 1;
    uint8_t panelBright= 180;

    // Window
    uint8_t winFx = WIN_FX_OFF;
    uint8_t winH  = 0, winS = 0, winV = 0;
    uint8_t winSpeed = 0;

    // Speaker (background identity)
    uint8_t spkClip = 0;
    uint8_t spkVol  = 80;      // low beacon volume
    bool    spkLoop = true;    // Central interprets as "beacon enabled"

    // Human tokens for clue generator
    // (These must match the “concepts” players see/hear.)
    char panelToken[24]  = {0}; // e.g., "12" or "3+4"
    char windowToken[24] = {0}; // e.g., "red", "disco"
    char soundToken[24]  = {0}; // e.g., "cat", "double beep"
  };

  // ----- Public lifecycle -----
  PizzaGameEngine() = default;

  void begin(const IO& io);

  // Bind an external orders payload buffer (Central’s g_orders[] and g_orderCount).
  // Engine will write 0..capacity items into buf and update *countPtr.
  void bindOrdersBuffer(PzOrderItemSetPayload* buf, uint8_t capacity, uint8_t* countPtr);

  // Configure run-level settings
  void setLivesMax(uint8_t livesMax);

  // Start/stop a run
  void startGame(uint32_t nowMs);
  // Start a run at a specific level (1..5). Useful for dev/testing via CLI.
  // NOTE: also rebuilds the mapping for that level and seeds its first wave.
  void startGameAtLevel(uint8_t startLvl, uint32_t nowMs);
  void stopGame(uint32_t nowMs);

  // Tick timers + expiry + refill logic
  // Returns true if orders changed and caller should broadcast.
  bool tick(uint32_t nowMs);

  // Central calls this when it has decided a delivery result.
  // ok=true: clear the order for that house, award progress, refill.
  // ok=false: lose a life, order remains active.
  bool onDeliveryResult(uint8_t houseId, bool ok, uint8_t reason, uint32_t nowMs);

  // Before broadcasting orders, call this to fill order_id + remain_s in the payload buffer.
  void prepareOrderBroadcast(uint32_t nowMs);

  // ----- Query -----
  Phase   phase() const { return m_phase; }
  uint8_t level() const { return m_level; }
  uint8_t livesLeft() const { return m_livesLeft; }
  uint8_t livesMax() const { return m_livesMax; }
  uint16_t successInLevel() const { return m_successInLevel; }
  uint16_t successTotal() const { return m_successTotal; }

  // Find active order for a house; returns -1 if none.
  int8_t  findActiveOrderIndexByHouse(uint8_t houseId) const;

  // Validate toppings against the order rules (engine-side).
  bool validateToppingsForOrderIndex(uint8_t orderIdx, uint8_t actualMask) const;

  // For legacy per-house mask use.
  bool getExpectedMaskForHouse(uint8_t houseId, uint8_t& outMask) const;

  bool consumeOrdersDirty();

  // Debug
  void debugPrintMapping() const;
  void debugPrintOrders(uint32_t nowMs) const;

  // Re-send the CURRENT house identities (panel/window/sound) without changing the mapping.
  // Useful when a house panel/node reboots mid-run or to cover ESPNOW packet loss.
  void resendMapping();
  void resendHouse(uint8_t houseId);

private:
  // ----- Internal: randomness -----
  uint32_t rng() const;

  // ----- Internal: level configs -----
  struct LevelCfg {
    uint8_t  N_success = 4;
    uint8_t  maxActive = 2;

    uint16_t timeout_s = 25;
    uint16_t timeout_firstWave_s = 30;
    uint8_t  firstWaveCount = 2;

    PizzaTier pizzaTier = PizzaTier::Single;

    bool compositesEnabled = false;      // level 2+
    bool relationsEnabled  = false;      // level 5
  };

  LevelCfg levelCfg(uint8_t level) const;

  // ----- Internal: mapping generation -----
  void startLevel(uint8_t newLevel, uint32_t nowMs);
  void buildMappingForLevel(uint8_t lvl);
  void pushMappingToHouses();

  // ----- Internal: identities per house -----
  HouseIdentity m_id[kHouseCount + 1]; // index by houseId 1..6

  // ----- Internal: physical facts + relations graph -----
  PhysicalFacts m_phys[kHouseCount + 1];

  uint8_t m_leftOf[kHouseCount + 1]  = {0};
  uint8_t m_rightOf[kHouseCount + 1] = {0};
  uint8_t m_opposite[kHouseCount + 1]= {0};

  void initStaticFacts();

  // ----- Internal: order list + payload binding -----
  OrderState m_orders[kMaxOrders];
  uint8_t    m_orderCount = 0;

  PzOrderItemSetPayload* m_outBuf = nullptr;
  uint8_t*   m_outCountPtr = nullptr;
  uint8_t    m_outCap = 0;

  // ----- Internal: run state -----
  IO       m_io{};
  Phase    m_phase = Phase::Idle;
  uint8_t  m_level = 1;

  uint8_t  m_livesMax  = 5;
  uint8_t  m_livesLeft = 5;

  uint16_t m_successInLevel = 0;
  uint16_t m_successTotal   = 0;

  uint16_t m_nextOrderId = 1;
  uint8_t  m_spawnedThisLevel = 0;

  bool     m_ordersDirty = false;

  // ----- Internal: order management -----
  void clearOrders();
  uint8_t desiredActiveOrdersForLevel(const LevelCfg& cfg) const;
  bool refillOrdersIfNeeded(const LevelCfg& cfg, uint32_t nowMs);
  bool spawnOneOrder(const LevelCfg& cfg, uint32_t nowMs);
  void syncPayloadFromInternal(uint32_t nowMs);

  // ----- Internal: clue generation -----
  Domain pickPreferredDomain() const;

  bool buildUniqueSingle(Domain d, uint8_t& outHouse, char* outText, size_t outTextSz,
                         char* outSig, size_t outSigSz) const;

  bool buildUniqueCompositeAnchored(Domain anchor, uint8_t& outHouse, char* outText, size_t outTextSz,
                                    char* outSig, size_t outSigSz) const;

  bool maybeWrapRelation(const LevelCfg& cfg, uint8_t& ioTargetHouse,
                         char* ioText, size_t ioTextSz,
                         char* ioSig, size_t ioSigSz) const;

  bool houseAlreadyTargeted(uint8_t houseId) const;
  bool sigAlreadyUsed(const char* sig) const;

  // ----- Internal: pizza rule generation -----
  OrderRules makePizzaRules(const LevelCfg& cfg) const;
};
