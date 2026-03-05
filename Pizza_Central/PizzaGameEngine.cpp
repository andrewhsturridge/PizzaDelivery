// Central-side game engine (mapping-first, always-on identities, quota progression)

#include "PizzaGameEngine.h"

#include <string.h>
#include <stdio.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_system.h> // esp_random
#endif

// -----------------------------
// Utilities
// -----------------------------
static inline uint32_t msFromS(uint32_t s) { return s * 1000UL; }

static const uint8_t kTopBits[5]  = {0x01, 0x02, 0x04, 0x08, 0x10};
static const char*   kTopNames[5] = {"pepperoni","mushrooms","peppers","pineapple","ham"};

// Window token catalog (expanded; keep tokens DISTINCT)
struct WinTok {
  const char* token; // lower-case name used in clue text
  uint8_t fx;
  uint8_t h,s,v;
  uint8_t speed;
};

// W1: solid colors (>=6 unique options)
static const WinTok kWinSolids[] = {
  {"red",    WIN_FX_SOLID, 0,   255, 130, 0},
  {"blue",   WIN_FX_SOLID, 170, 255, 130, 0},
  {"green",  WIN_FX_SOLID, 85,  255, 130, 0},
  {"yellow", WIN_FX_SOLID, 42,  255, 130, 0},
  {"purple", WIN_FX_SOLID, 200, 255, 130, 0},
  {"cyan",   WIN_FX_SOLID, 120, 255, 130, 0},
  {"orange", WIN_FX_SOLID, 18,  255, 130, 0},
  {"pink",   WIN_FX_SOLID, 235, 200, 130, 0},
  {"white",  WIN_FX_SOLID, 0,   0,   200, 0},
};

// W2: visually-distinct patterns (non-directional)
static const WinTok kWinW2[] = {
  {"disco",   WIN_FX_PARTY,      0, 200, 120, 220},
  {"police",  WIN_FX_POLICE,     0,   0, 160, 200},
  {"flicker", WIN_FX_FLICKER,   18, 255, 160, 190},
  {"sparkle", WIN_FX_SPARKLE,    0,   0, 150, 180},
  {"pulse",   WIN_FX_PULSE,    200, 255, 180, 170},
  {"strobe",  WIN_FX_STROBE,     0,   0, 200, 210},
  {"split",   WIN_FX_SPLIT_SWAP, 30, 255, 160, 120},
};

// W3: motion/direction patterns
static const WinTok kWinW3[] = {
  {"chase clockwise",        WIN_FX_CHASE_CW,   0, 255, 180, 170},
  {"chase counter-clockwise",WIN_FX_CHASE_CCW,  0, 255, 180, 170},
  {"bounce",                WIN_FX_BOUNCE,    120, 255, 180, 150},
  {"wedge clockwise",        WIN_FX_WEDGE_CW,   85, 255, 180, 160},
  {"wedge counter-clockwise",WIN_FX_WEDGE_CCW,  85, 255, 180, 160},
};

// -----------------------------
// Sound token catalog
// NOTE: Clip IDs must exist as /clips/XYZ.wav on the houses.
// - S2 beep patterns (100..105) are generated on the houses at boot.
// -----------------------------
enum SoundCat : uint8_t { SC_ANIMAL=0, SC_ALERT=1, SC_PERSON=2, SC_INSTR=3 };

struct SoundTok {
  const char* token; // lower-case name used in clue text
  uint8_t clip;
  uint8_t cat;
};

static const SoundTok kSoundS1[] = {
  // Animals
  {"cat",        1,  SC_ANIMAL},
  {"dog",        2,  SC_ANIMAL},
  {"bird",       14, SC_ANIMAL},
  {"frog",       15, SC_ANIMAL},

  // Alerts / Household
  {"doorbell",   3,  SC_ALERT},
  {"fire alarm", 4,  SC_ALERT},
  {"buzzer",     9,  SC_ALERT},
  {"phone ring", 10, SC_ALERT},

  // People
  {"baby",       6,  SC_PERSON},
  {"laugh",      13, SC_PERSON},
  {"sneeze",     16, SC_PERSON},
  {"angry",      7,  SC_PERSON},

  // Instruments
  {"drum",       17, SC_INSTR},
  {"guitar",     11, SC_INSTR},
  {"piano",      18, SC_INSTR},
  {"trumpet",    12, SC_INSTR},
};

// S2: generated beep-pattern identities (HouseNode generates these: /clips/100..105.wav)
static const SoundTok kSoundS2[] = {
  {"double beep", 100, SC_ALERT},
  {"triple beep", 101, SC_ALERT},
  {"long beep",   102, SC_ALERT},
  {"short-long",  103, SC_ALERT},
  {"long-short",  104, SC_ALERT},
  {"five fast",   105, SC_ALERT},
};


// -----------------------------
// Panel token pools
// - T1: numbers and easy names
// - T2: equations, harder famous last names, and icons
// NOTE: Avoid tokens that overlap with other domains (colors, etc.).
// -----------------------------

// T1 names (optional later; we keep Level 1/2 as numeric for now)
static const char* kPanelNamesT1[] = {
  "TESLA",
  "JOBS",
  "GATES",
  "MUSK",
  "BEZOS",
  "JORDAN",
  "SWIFT",
  "DISNEY",
  "MOZART",
  "PICASSO",
};

// T2 names: famous but a bit harder/longer
static const char* kPanelNamesT2[] = {
  "EINSTEIN",
  "NEWTON",
  "CURIE",
  "TURING",
  "DARWIN",
  "SHAKESPEARE",
  "VANGOGH",
  "BEETHOVEN",
  "SPIELBERG",
  "HITCHCOCK",
  "FREUD",
  "GALILEI",
  "BECKHAM",
  "CHAPLIN",
};

// T2 icons: rendered by HousePanel when text begins with '@'
static const char* kPanelIconsT2[] = {
  "@STAR",
  "@HEART",
  "@CROWN",
  "@KEY",
  "@BOLT",
  "@SKULL",
  "@SMILE",
  "@FROWN",
  "@UP",
  "@DOWN",
  "@CHECK",
  "@X",
};

static const char* doorColorName(uint8_t dc) {
  switch (dc) {
    case PizzaGameEngine::DC_BROWN: return "brown";
    case PizzaGameEngine::DC_WHITE: return "white";
    case PizzaGameEngine::DC_GREY:  return "grey";
    default: return "unknown";
  }
}

static const char* handleColorName(uint8_t hc) {
  switch (hc) {
    case PizzaGameEngine::HCOL_SILVER: return "silver";
    case PizzaGameEngine::HCOL_GOLD:   return "gold";
    default: return "unknown";
  }
}

static bool isDigitsOnly(const char* s) {
  if (!s || !*s) return false;
  for (const char* p=s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

static void shuffleU8(uint8_t* a, uint8_t n, uint32_t (*rng)()) {
  for (uint8_t i=0;i<n;i++) {
    uint8_t j = (uint8_t)(i + (rng() % (n - i)));
    uint8_t t=a[i]; a[i]=a[j]; a[j]=t;
  }
}

static void makeUniqueNumbers(char outByHouse[7][8], uint32_t (*rng)()) {
  uint8_t base = (uint8_t)(1 + (rng() % 94)); // 1..94
  for (uint8_t h=1; h<=6; ++h) {
    snprintf(outByHouse[h], 8, "%u", (unsigned)(base + (h-1)));
  }
}

static void makeUniqueEquations(char out[][12], uint8_t count, uint32_t (*rng)()) {
  // generate unique "a+b" strings with a,b in 1..9
  char used[32][12] = {{0}};
  uint8_t usedN = 0;

  auto already = [&](const char* s)->bool{
    for (uint8_t i=0;i<usedN;i++) if (strncmp(used[i], s, 12) == 0) return true;
    return false;
  };

  uint8_t safety=0;
  while (usedN < count && safety++ < 200) {
    uint8_t a = (uint8_t)(1 + (rng() % 9));
    uint8_t b = (uint8_t)(1 + (rng() % 9));
    char s[12];
    snprintf(s, sizeof(s), "%u+%u", (unsigned)a, (unsigned)b);
    if (already(s)) continue;
    strlcpy(used[usedN++], s, 12);
  }

  // If somehow we couldn't generate enough, fill with fallback deterministic equations
  uint8_t fb = 1;
  while (usedN < count) {
    char s[12];
    snprintf(s, sizeof(s), "%u+%u", (unsigned)fb, (unsigned)(fb+1));
    if (!already(s)) strlcpy(used[usedN++], s, 12);
    fb++;
  }

  // Shuffle then copy
  uint8_t idx[32];
  for (uint8_t i=0;i<usedN;i++) idx[i]=i;
  shuffleU8(idx, usedN, rng);
  for (uint8_t i=0;i<count;i++) {
    strlcpy(out[i], used[idx[i]], 12);
  }
}

// -----------------------------
// PizzaGameEngine implementation
// -----------------------------

void PizzaGameEngine::begin(const IO& io) {
  m_io = io;
  if (!m_io.rand32) {
    m_io.rand32 = []() -> uint32_t {
      #if defined(ARDUINO_ARCH_ESP32)
        return esp_random();
      #else
        return (uint32_t)random(0x7fffffff);
      #endif
    };
  }
  initStaticFacts();
}

void PizzaGameEngine::bindOrdersBuffer(PzOrderItemSetPayload* buf, uint8_t capacity, uint8_t* countPtr) {
  m_outBuf = buf;
  m_outCap = capacity;
  m_outCountPtr = countPtr;
  if (m_outCountPtr) *m_outCountPtr = 0;
  syncPayloadFromInternal(millis());
}

void PizzaGameEngine::setLivesMax(uint8_t livesMax) {
  m_livesMax = (livesMax == 0) ? 5 : livesMax;
  if (m_phase != Phase::Running) {
    m_livesLeft = m_livesMax;
  } else {
    if (m_livesLeft > m_livesMax) m_livesLeft = m_livesMax;
  }
}

void PizzaGameEngine::startGame(uint32_t nowMs) {
  startGameAtLevel(1, nowMs);
}

void PizzaGameEngine::startGameAtLevel(uint8_t startLvl, uint32_t nowMs) {
  if (startLvl < 1) startLvl = 1;
  if (startLvl > 5) startLvl = 5;

  m_phase = Phase::Running;
  m_level = startLvl;
  m_livesLeft = m_livesMax;
  m_successInLevel = 0;
  m_successTotal   = 0;
  m_nextOrderId    = 1;
  m_spawnedThisLevel = 0;
  m_ordersDirty = true;

  startLevel(startLvl, nowMs);
}

void PizzaGameEngine::stopGame(uint32_t /*nowMs*/) {
  m_phase = Phase::Over;
  clearOrders();
  m_ordersDirty = true;
  syncPayloadFromInternal(millis());
}

uint32_t PizzaGameEngine::rng() const {
  return m_io.rand32 ? m_io.rand32() : 0x12345678u;
}

PizzaGameEngine::LevelCfg PizzaGameEngine::levelCfg(uint8_t level) const {
  LevelCfg c{};
  // Locked ladder v1 (matches Central status helper)
  switch (level) {
    default:
    case 1:
      c.N_success = 4; c.maxActive = 2;
      // Longer per-order timers (more forgiving): v6 tuning
      c.timeout_s = 45; c.timeout_firstWave_s = 55; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Single;
      c.compositesEnabled = false;
      c.relationsEnabled  = false;
      break;
    case 2:
      c.N_success = 4; c.maxActive = 2;
      c.timeout_s = 40; c.timeout_firstWave_s = 50; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Single;
      c.compositesEnabled = true;
      c.relationsEnabled  = false;
      break;
    case 3:
      c.N_success = 5; c.maxActive = 3;
      c.timeout_s = 35; c.timeout_firstWave_s = 45; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Multi2;
      c.compositesEnabled = true;
      c.relationsEnabled  = false;
      break;
    case 4:
      c.N_success = 6; c.maxActive = 3;
      c.timeout_s = 30; c.timeout_firstWave_s = 40; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Multi3;
      c.compositesEnabled = true;
      c.relationsEnabled  = false;
      break;
    case 5:
      c.N_success = 6; c.maxActive = 3;
      c.timeout_s = 28; c.timeout_firstWave_s = 38; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Constraints;
      c.compositesEnabled = true;
      c.relationsEnabled  = true;
      break;
  }
  return c;
}

void PizzaGameEngine::startLevel(uint8_t newLevel, uint32_t nowMs) {
  if (newLevel < 1) newLevel = 1;
  if (newLevel > 5) {
    stopGame(nowMs);
    return;
  }

  m_level = newLevel;
  m_successInLevel = 0;
  m_spawnedThisLevel = 0;

  clearOrders();
  buildMappingForLevel(m_level);
  pushMappingToHouses();

  const LevelCfg cfg = levelCfg(m_level);
  refillOrdersIfNeeded(cfg, nowMs);
  syncPayloadFromInternal(nowMs);
  m_ordersDirty = true;
}

void PizzaGameEngine::clearOrders() {
  m_orderCount = 0;
  for (uint8_t i=0;i<kMaxOrders;i++) m_orders[i] = OrderState{};
  if (m_outCountPtr) *m_outCountPtr = 0;
}

uint8_t PizzaGameEngine::desiredActiveOrdersForLevel(const LevelCfg& cfg) const {
  if (m_successInLevel >= cfg.N_success) return 0;
  uint16_t remaining = (uint16_t)(cfg.N_success - m_successInLevel);
  return (remaining < cfg.maxActive) ? (uint8_t)remaining : cfg.maxActive;
}

bool PizzaGameEngine::tick(uint32_t nowMs) {
  if (m_phase != Phase::Running) return false;

  bool changed = false;
  const LevelCfg cfg = levelCfg(m_level);

  // Expiry scan
  for (uint8_t i=0; i<m_orderCount; ) {
    if (m_orders[i].used && m_orders[i].expiresAtMs && (int32_t)(nowMs - m_orders[i].expiresAtMs) >= 0) {
      if (m_livesLeft > 0) m_livesLeft--;
      for (uint8_t j=i+1; j<m_orderCount; ++j) m_orders[j-1] = m_orders[j];
      m_orderCount--;
      changed = true;
      m_ordersDirty = true;
      continue;
    }
    ++i;
  }

  if (m_livesLeft == 0) {
    stopGame(nowMs);
    return true;
  }

  if (refillOrdersIfNeeded(cfg, nowMs)) changed = true;

  // Level completion: quota satisfied and no active orders
  if (m_successInLevel >= cfg.N_success && m_orderCount == 0) {
    if (m_level >= 5) {
      stopGame(nowMs);
      return true;
    }
    startLevel((uint8_t)(m_level + 1), nowMs);
    return true;
  }

  if (changed) syncPayloadFromInternal(nowMs);
  return changed;
}

bool PizzaGameEngine::onDeliveryResult(uint8_t houseId, bool ok, uint8_t /*reason*/, uint32_t nowMs) {
  if (m_phase != Phase::Running) return false;

  bool changed = false;
  const LevelCfg cfg = levelCfg(m_level);

  if (!ok) {
    if (m_livesLeft > 0) m_livesLeft--;
    if (m_livesLeft == 0) {
      stopGame(nowMs);
      return true;
    }
    // Wrong delivery does not clear
    return false;
  }

  // Success: remove any order targeting this house
  for (uint8_t i=0; i<m_orderCount; ) {
    if (m_orders[i].used && m_orders[i].targetHouse == houseId) {
      for (uint8_t j=i+1; j<m_orderCount; ++j) m_orders[j-1] = m_orders[j];
      m_orderCount--;
      changed = true;
      m_ordersDirty = true;
      continue;
    }
    ++i;
  }

  m_successInLevel++;
  m_successTotal++;

  if (refillOrdersIfNeeded(cfg, nowMs)) changed = true;

  if (m_successInLevel >= cfg.N_success && m_orderCount == 0) {
    if (m_level >= 5) {
      stopGame(nowMs);
      return true;
    }
    startLevel((uint8_t)(m_level + 1), nowMs);
    return true;
  }

  if (changed) syncPayloadFromInternal(nowMs);
  return changed;
}

void PizzaGameEngine::prepareOrderBroadcast(uint32_t nowMs) {
  if (!m_outBuf || !m_outCountPtr) return;
  uint8_t cnt = *m_outCountPtr;
  if (cnt > m_outCap) cnt = m_outCap;

  for (uint8_t i=0;i<cnt;i++) {
    if (i >= m_orderCount) {
      m_outBuf[i].order_id = 0;
      m_outBuf[i].remain_s = 0;
      continue;
    }

    const OrderState& st = m_orders[i];
    m_outBuf[i].order_id = st.orderId;

    if (st.expiresAtMs == 0) {
      m_outBuf[i].remain_s = 0;
    } else if ((int32_t)(st.expiresAtMs - nowMs) <= 0) {
      m_outBuf[i].remain_s = 0;
    } else {
      uint32_t remMs = st.expiresAtMs - nowMs;
      uint32_t remS  = (remMs + 999) / 1000;
      if (remS > 65535) remS = 65535;
      m_outBuf[i].remain_s = (uint16_t)remS;
    }
  }
}

int8_t PizzaGameEngine::findActiveOrderIndexByHouse(uint8_t houseId) const {
  for (uint8_t i=0;i<m_orderCount;i++) {
    if (m_orders[i].used && m_orders[i].targetHouse == houseId) return (int8_t)i;
  }
  return -1;
}

bool PizzaGameEngine::validateToppingsForOrderIndex(uint8_t orderIdx, uint8_t actualMask) const {
  if (orderIdx >= m_orderCount) return false;
  const OrderState& o = m_orders[orderIdx];
  if (!o.used) return false;
  if (o.rules.exactMask) {
    return actualMask == o.rules.requireMask;
  }
  if ((actualMask & o.rules.requireMask) != o.rules.requireMask) return false;
  if ((actualMask & o.rules.forbidMask) != 0) return false;
  return true;
}

bool PizzaGameEngine::getExpectedMaskForHouse(uint8_t houseId, uint8_t& outMask) const {
  int8_t idx = findActiveOrderIndexByHouse(houseId);
  if (idx < 0) return false;
  const OrderState& o = m_orders[(uint8_t)idx];
  outMask = o.rules.requireMask;
  return true;
}

bool PizzaGameEngine::consumeOrdersDirty() {
  bool d = m_ordersDirty;
  m_ordersDirty = false;
  return d;
}

// -----------------------------
// Mapping generation
// -----------------------------

static uint8_t countTok(const PizzaGameEngine::HouseIdentity id[], PizzaGameEngine::Domain d, const char* tok) {
  if (!tok || !*tok) return 0;
  uint8_t c=0;
  for (uint8_t h=1; h<=PizzaGameEngine::kHouseCount; ++h) {
    const char* t2 = "";
    switch (d) {
      case PizzaGameEngine::Domain::Panel:  t2 = id[h].panelToken;  break;
      case PizzaGameEngine::Domain::Window: t2 = id[h].windowToken; break;
      case PizzaGameEngine::Domain::Sound:  t2 = id[h].soundToken;  break;
      default: break;
    }
    if (t2 && strcmp(t2, tok)==0) c++;
  }
  return c;
}

void PizzaGameEngine::buildMappingForLevel(uint8_t lvl) {
  // Reset identities
  for (uint8_t h=1; h<=kHouseCount; ++h) {
    m_id[h] = HouseIdentity{};
    m_id[h].winFx = WIN_FX_OFF;
    m_id[h].spkClip = 0;
    m_id[h].spkVol  = 70;
    m_id[h].spkLoop = true;
    m_id[h].panelBright = 180;
    m_id[h].panelStyle  = 1;
    m_id[h].panelSpeed  = 1;
    m_id[h].winS = 255;
    m_id[h].winV = 130;
  }

  // ----- Panel baseline numbers (same as Level 1 original) -----
  char nums[7][8] = {{0}};
  makeUniqueNumbers(nums, m_io.rand32);

  auto applyPanelNumber = [&](uint8_t h, const char* digits) {
    m_id[h].panelMode = PANEL_MODE_NUMBER;
    strlcpy(m_id[h].panelText, digits, sizeof(m_id[h].panelText));
    strlcpy(m_id[h].panelToken, digits, sizeof(m_id[h].panelToken));
  };
  auto applyPanelText = [&](uint8_t h, const char* textTok) {
    m_id[h].panelMode = PANEL_MODE_TEXT;
    strlcpy(m_id[h].panelText, textTok, sizeof(m_id[h].panelText));
    strlcpy(m_id[h].panelToken, textTok, sizeof(m_id[h].panelToken));
  };
  auto applyWinTok = [&](uint8_t h, const WinTok& t) {
    m_id[h].winFx = t.fx;
    m_id[h].winH  = t.h;
    m_id[h].winS  = t.s;
    m_id[h].winV  = t.v;
    m_id[h].winSpeed = t.speed;
    strlcpy(m_id[h].windowToken, t.token, sizeof(m_id[h].windowToken));
  };
  auto applySoundTok = [&](uint8_t h, const SoundTok& s, uint8_t vol) {
    m_id[h].spkClip = s.clip;
    m_id[h].spkVol  = vol;
    m_id[h].spkLoop = true; // means "beacon enabled" on HouseNode
    strlcpy(m_id[h].soundToken, s.token, sizeof(m_id[h].soundToken));
  };

  // Pick 6 distinct window solid indices
  uint8_t winIdx[6];
  {
    uint8_t poolN = (uint8_t)(sizeof(kWinSolids)/sizeof(kWinSolids[0]));
    uint8_t pool[32];
    for (uint8_t i=0;i<poolN;i++) pool[i]=i;
    shuffleU8(pool, poolN, m_io.rand32);
    for (uint8_t i=0;i<6;i++) winIdx[i]=pool[i];
  }

  // Pick 6 distinct sound S1 indices
  uint8_t sndIdx[6];
  {
    uint8_t poolN = (uint8_t)(sizeof(kSoundS1)/sizeof(kSoundS1[0]));
    uint8_t pool[32];
    for (uint8_t i=0;i<poolN;i++) pool[i]=i;
    shuffleU8(pool, poolN, m_io.rand32);
    for (uint8_t i=0;i<6;i++) sndIdx[i]=pool[i];
  }

  // -----------------------------
  // Level recipes (Mapping-first)
  // -----------------------------

  if (lvl == 1) {
    // Panel: T1 numbers all unique
    for (uint8_t h=1; h<=6; ++h) applyPanelNumber(h, nums[h]);

    // Windows: W1 all unique solids
    for (uint8_t h=1; h<=6; ++h) applyWinTok(h, kWinSolids[winIdx[h-1]]);

    // Level 1: NO sound identities (quiet intro round).
    // Keep speakers silent so round 1 does not require audio mapping.
    for (uint8_t h=1; h<=6; ++h) {
      m_id[h].spkClip = 0;
      m_id[h].spkVol  = 0;
      m_id[h].spkLoop = false;
      m_id[h].soundToken[0] = '\0';
    }
    return;
  }

  if (lvl == 2) {
    // Panel: T1 numbers all unique
    for (uint8_t h=1; h<=6; ++h) applyPanelNumber(h, nums[h]);

    // Start with windows unique + sounds unique
    for (uint8_t h=1; h<=6; ++h) {
      applyWinTok(h, kWinSolids[winIdx[h-1]]);
      applySoundTok(h, kSoundS1[sndIdx[h-1]], 70);
    }

    // Add a collision in ONE digital domain (2/2/1/1) to force composites sometimes
    bool soundCollision = ((rng() % 2) == 0);
    uint8_t hh[6] = {1,2,3,4,5,6};
    shuffleU8(hh, 6, m_io.rand32);

    if (soundCollision) {
      // Sound 2/2/1/1
      applySoundTok(hh[0], kSoundS1[sndIdx[0]], 70);
      applySoundTok(hh[1], kSoundS1[sndIdx[0]], 70);
      applySoundTok(hh[2], kSoundS1[sndIdx[1]], 70);
      applySoundTok(hh[3], kSoundS1[sndIdx[1]], 70);
      applySoundTok(hh[4], kSoundS1[sndIdx[2]], 70);
      applySoundTok(hh[5], kSoundS1[sndIdx[3]], 70);
    } else {
      // Windows 2/2/1/1
      applyWinTok(hh[0], kWinSolids[winIdx[0]]);
      applyWinTok(hh[1], kWinSolids[winIdx[0]]);
      applyWinTok(hh[2], kWinSolids[winIdx[1]]);
      applyWinTok(hh[3], kWinSolids[winIdx[1]]);
      applyWinTok(hh[4], kWinSolids[winIdx[2]]);
      applyWinTok(hh[5], kWinSolids[winIdx[3]]);
    }
    return;
  }

  if (lvl == 3) {
    // Panel: 3 T1 numbers, 3 T2 (1 equation, 1 name, 1 icon)
    uint8_t hh[6] = {1,2,3,4,5,6};
    shuffleU8(hh, 6, m_io.rand32);
    bool isT2[7] = {0};
    for (uint8_t i=0;i<3;i++) isT2[hh[i]] = true;

    // Prepare T2 items
    char eqs[2][12];
    makeUniqueEquations(eqs, 2, m_io.rand32);

    uint8_t nameIdx[2];
    {
      uint8_t poolN = (uint8_t)(sizeof(kPanelNamesT2)/sizeof(kPanelNamesT2[0]));
      uint8_t pool[32];
      for (uint8_t i=0;i<poolN;i++) pool[i]=i;
      shuffleU8(pool, poolN, m_io.rand32);
      nameIdx[0]=pool[0]; nameIdx[1]=pool[1];
    }

    uint8_t iconIdx[2];
    {
      uint8_t poolN = (uint8_t)(sizeof(kPanelIconsT2)/sizeof(kPanelIconsT2[0]));
      uint8_t pool[32];
      for (uint8_t i=0;i<poolN;i++) pool[i]=i;
      shuffleU8(pool, poolN, m_io.rand32);
      iconIdx[0]=pool[0]; iconIdx[1]=pool[1];
    }

    // Assign the 3 T2 houses: shuffle which category goes where
    uint8_t t2H[3];
    uint8_t t2n=0;
    for (uint8_t i=0;i<6;i++) if (isT2[hh[i]]) t2H[t2n++] = hh[i];
    if (t2n != 3) { t2H[0]=hh[0]; t2H[1]=hh[1]; t2H[2]=hh[2]; }

    // permute categories eq/name/icon
    uint8_t cat[3] = {0,1,2};
    shuffleU8(cat, 3, m_io.rand32);

    for (uint8_t h=1; h<=6; ++h) {
      if (!isT2[h]) {
        applyPanelNumber(h, nums[h]);
        continue;
      }
      // Find index in t2H list
      uint8_t slot=0;
      for (uint8_t i=0;i<3;i++) if (t2H[i]==h) { slot=i; break; }
      uint8_t which = cat[slot];
      if (which == 0) {
        applyPanelText(h, eqs[slot%2]);
      } else if (which == 1) {
        applyPanelText(h, kPanelNamesT2[nameIdx[slot%2]]);
      } else {
        applyPanelText(h, kPanelIconsT2[iconIdx[slot%2]]);
      }
    }

    // Windows: 1 W2 pattern, rest W1 solids
    uint8_t discoHouse = (uint8_t)(1 + (rng() % 6));
    const WinTok& p = kWinW2[rng() % (sizeof(kWinW2)/sizeof(kWinW2[0]))];
    for (uint8_t h=1; h<=6; ++h) {
      if (h == discoHouse) applyWinTok(h, p);
      else applyWinTok(h, kWinSolids[winIdx[h-1]]);
    }

    // Sound: 2/1/1/1/1 (one duplicated)
    uint8_t sh[6] = {1,2,3,4,5,6};
    shuffleU8(sh, 6, m_io.rand32);
    applySoundTok(sh[0], kSoundS1[sndIdx[0]], 70);
    applySoundTok(sh[1], kSoundS1[sndIdx[0]], 70);
    applySoundTok(sh[2], kSoundS1[sndIdx[1]], 70);
    applySoundTok(sh[3], kSoundS1[sndIdx[2]], 70);
    applySoundTok(sh[4], kSoundS1[sndIdx[3]], 70);
    applySoundTok(sh[5], kSoundS1[sndIdx[4]], 70);
    return;
  }

  if (lvl == 4) {
    // Panel: T2 all (2 equations, 2 names, 2 icons)
    char eqs[2][12];
    makeUniqueEquations(eqs, 2, m_io.rand32);

    uint8_t namePool[2];
    {
      uint8_t N = (uint8_t)(sizeof(kPanelNamesT2)/sizeof(kPanelNamesT2[0]));
      uint8_t tmp[32]; for (uint8_t i=0;i<N;i++) tmp[i]=i;
      shuffleU8(tmp, N, m_io.rand32);
      namePool[0]=tmp[0]; namePool[1]=tmp[1];
    }

    uint8_t iconPool[2];
    {
      uint8_t N = (uint8_t)(sizeof(kPanelIconsT2)/sizeof(kPanelIconsT2[0]));
      uint8_t tmp[32]; for (uint8_t i=0;i<N;i++) tmp[i]=i;
      shuffleU8(tmp, N, m_io.rand32);
      iconPool[0]=tmp[0]; iconPool[1]=tmp[1];
    }

    // Assign in shuffled house order to keep it feeling random
    uint8_t hh[6] = {1,2,3,4,5,6};
    shuffleU8(hh, 6, m_io.rand32);
    applyPanelText(hh[0], eqs[0]);
    applyPanelText(hh[1], eqs[1]);
    applyPanelText(hh[2], kPanelNamesT2[namePool[0]]);
    applyPanelText(hh[3], kPanelNamesT2[namePool[1]]);
    applyPanelText(hh[4], kPanelIconsT2[iconPool[0]]);
    applyPanelText(hh[5], kPanelIconsT2[iconPool[1]]);

    // Windows: W2 3/3 (two distinct patterns)
    uint8_t wpN = (uint8_t)(sizeof(kWinW2)/sizeof(kWinW2[0]));
    uint8_t wpi[16];
    for (uint8_t i=0;i<wpN;i++) wpi[i]=i;
    shuffleU8(wpi, wpN, m_io.rand32);
    const WinTok& a = kWinW2[wpi[0]];
    const WinTok& b = kWinW2[wpi[1]];

    uint8_t wh[6] = {1,2,3,4,5,6};
    shuffleU8(wh, 6, m_io.rand32);
    for (uint8_t i=0;i<3;i++) applyWinTok(wh[i], a);
    for (uint8_t i=3;i<6;i++) applyWinTok(wh[i], b);

    // Sound: S2 2/2 + S1 1/1
    uint8_t spN = (uint8_t)(sizeof(kSoundS2)/sizeof(kSoundS2[0]));
    uint8_t spi[16]; for (uint8_t i=0;i<spN;i++) spi[i]=i;
    shuffleU8(spi, spN, m_io.rand32);

    uint8_t sh[6] = {1,2,3,4,5,6};
    shuffleU8(sh, 6, m_io.rand32);
    applySoundTok(sh[0], kSoundS2[spi[0]], 70);
    applySoundTok(sh[1], kSoundS2[spi[0]], 70);
    applySoundTok(sh[2], kSoundS2[spi[1]], 70);
    applySoundTok(sh[3], kSoundS2[spi[1]], 70);
    applySoundTok(sh[4], kSoundS1[sndIdx[0]], 70);
    applySoundTok(sh[5], kSoundS1[sndIdx[1]], 70);

    // Bucket overlay: either brightness buckets on one W2 pattern, OR volume buckets on dogs
    bool bucketVolume = ((rng() % 2) == 1);
    if (!bucketVolume) {
      // Brightness buckets on the FIRST W2 pattern group (a)
      // Find 3 houses whose window token == a.token
      uint8_t grp[3]; uint8_t gn=0;
      for (uint8_t h=1; h<=6 && gn<3; ++h) {
        if (strcmp(m_id[h].windowToken, a.token) == 0) grp[gn++] = h;
      }
      if (gn==3) {
        m_id[grp[0]].winV = 60;  snprintf(m_id[grp[0]].windowToken, sizeof(m_id[grp[0]].windowToken), "dim %s", a.token);
        m_id[grp[1]].winV = 120; strlcpy(m_id[grp[1]].windowToken, a.token, sizeof(m_id[grp[1]].windowToken));
        m_id[grp[2]].winV = 200; snprintf(m_id[grp[2]].windowToken, sizeof(m_id[grp[2]].windowToken), "bright %s", a.token);
      }
    } else {
      // Volume buckets: force 3 houses to be dog at quiet/med/loud
      uint8_t dh[6] = {1,2,3,4,5,6};
      shuffleU8(dh, 6, m_io.rand32);
      applySoundTok(dh[0], kSoundS1[1], 35);
      strlcpy(m_id[dh[0]].soundToken, "quiet dog", sizeof(m_id[dh[0]].soundToken));
      applySoundTok(dh[1], kSoundS1[1], 70);
      strlcpy(m_id[dh[1]].soundToken, "dog", sizeof(m_id[dh[1]].soundToken));
      applySoundTok(dh[2], kSoundS1[1], 140);
      strlcpy(m_id[dh[2]].soundToken, "loud dog", sizeof(m_id[dh[2]].soundToken));
    }
    return;
  }

  // lvl 5
  {
    // Panel: T2 all (2/2/2 again)
    char eqs[2][12];
    makeUniqueEquations(eqs, 2, m_io.rand32);

    uint8_t namePool[2];
    {
      uint8_t N = (uint8_t)(sizeof(kPanelNamesT2)/sizeof(kPanelNamesT2[0]));
      uint8_t tmp[32]; for (uint8_t i=0;i<N;i++) tmp[i]=i;
      shuffleU8(tmp, N, m_io.rand32);
      namePool[0]=tmp[0]; namePool[1]=tmp[1];
    }

    uint8_t iconPool[2];
    {
      uint8_t N = (uint8_t)(sizeof(kPanelIconsT2)/sizeof(kPanelIconsT2[0]));
      uint8_t tmp[32]; for (uint8_t i=0;i<N;i++) tmp[i]=i;
      shuffleU8(tmp, N, m_io.rand32);
      iconPool[0]=tmp[0]; iconPool[1]=tmp[1];
    }

    uint8_t hh[6] = {1,2,3,4,5,6};
    shuffleU8(hh, 6, m_io.rand32);
    applyPanelText(hh[0], eqs[0]);
    applyPanelText(hh[1], eqs[1]);
    applyPanelText(hh[2], kPanelNamesT2[namePool[0]]);
    applyPanelText(hh[3], kPanelNamesT2[namePool[1]]);
    applyPanelText(hh[4], kPanelIconsT2[iconPool[0]]);
    applyPanelText(hh[5], kPanelIconsT2[iconPool[1]]);

    // Windows: 2/2/2 using two W3 motion patterns + one W2 pattern
    uint8_t w3N = (uint8_t)(sizeof(kWinW3)/sizeof(kWinW3[0]));
    uint8_t w3i[16]; for (uint8_t i=0;i<w3N;i++) w3i[i]=i;
    shuffleU8(w3i, w3N, m_io.rand32);
    const WinTok& m1 = kWinW3[w3i[0]];
    const WinTok& m2 = kWinW3[w3i[1]];

    const WinTok& p  = kWinW2[rng() % (sizeof(kWinW2)/sizeof(kWinW2[0]))];

    uint8_t wh[6] = {1,2,3,4,5,6};
    shuffleU8(wh, 6, m_io.rand32);
    applyWinTok(wh[0], m1);
    applyWinTok(wh[1], m1);
    applyWinTok(wh[2], m2);
    applyWinTok(wh[3], m2);
    applyWinTok(wh[4], p);
    applyWinTok(wh[5], p);

    // Sound: S2 2/2/2
    uint8_t spN = (uint8_t)(sizeof(kSoundS2)/sizeof(kSoundS2[0]));
    uint8_t spi[16]; for (uint8_t i=0;i<spN;i++) spi[i]=i;
    shuffleU8(spi, spN, m_io.rand32);

    uint8_t sh[6] = {1,2,3,4,5,6};
    shuffleU8(sh, 6, m_io.rand32);
    applySoundTok(sh[0], kSoundS2[spi[0]], 70);
    applySoundTok(sh[1], kSoundS2[spi[0]], 70);
    applySoundTok(sh[2], kSoundS2[spi[1]], 70);
    applySoundTok(sh[3], kSoundS2[spi[1]], 70);
    applySoundTok(sh[4], kSoundS2[spi[2]], 70);
    applySoundTok(sh[5], kSoundS2[spi[2]], 70);
  }
}

void PizzaGameEngine::pushMappingToHouses() {
  if (!m_io.sendHouseDigital) return;

  for (uint8_t h=1; h<=kHouseCount; ++h) {
    const HouseIdentity& id = m_id[h];

    // Always-on: window + panel + speaker
    uint8_t flags = 0x01 | 0x02 | 0x04;
    bool stopNow = (id.spkClip == 0);

    m_io.sendHouseDigital(
      h,
      flags,
      // window
      id.winFx, id.winH, id.winS, id.winV, id.winSpeed,
      // panel
      id.panelMode, id.panelText, id.panelStyle, id.panelSpeed, id.panelBright,
      // speaker
      id.spkClip, id.spkVol, id.spkLoop, stopNow
    );
  }
}

void PizzaGameEngine::resendMapping() {
  pushMappingToHouses();
}

void PizzaGameEngine::resendHouse(uint8_t houseId) {
  if (!m_io.sendHouseDigital) return;
  if (houseId < 1 || houseId > kHouseCount) return;

  const HouseIdentity& id = m_id[houseId];

  // Always-on: window + panel + speaker
  uint8_t flags = 0x01 | 0x02 | 0x04;
  bool stopNow = (id.spkClip == 0);

  m_io.sendHouseDigital(
    houseId,
    flags,
    // window
    id.winFx, id.winH, id.winS, id.winV, id.winSpeed,
    // panel
    id.panelMode, id.panelText, id.panelStyle, id.panelSpeed, id.panelBright,
    // speaker
    id.spkClip, id.spkVol, id.spkLoop, stopNow
  );
}


// -----------------------------
// Orders
// -----------------------------

bool PizzaGameEngine::refillOrdersIfNeeded(const LevelCfg& cfg, uint32_t nowMs) {
  uint8_t desired = desiredActiveOrdersForLevel(cfg);
  bool changed=false;
  uint8_t safety=0;
  while (m_orderCount < desired && safety++ < 24) {
    if (!spawnOneOrder(cfg, nowMs)) break;
    changed=true;
  }
  return changed;
}

PizzaGameEngine::Domain PizzaGameEngine::pickPreferredDomain() const {
  // Level 1 is intended as a quiet intro round.
  // - No sound identities are mapped to houses.
  // - Therefore we must not generate sound-domain clues.
  if (m_level <= 1) {
    switch (rng() % 3) {
      default:
      case 0: return Domain::Panel;
      case 1: return Domain::Window;
      case 2: return Domain::Physical;
    }
  }

  switch (rng() % 4) {
    default:
    case 0: return Domain::Panel;
    case 1: return Domain::Window;
    case 2: return Domain::Sound;
    case 3: return Domain::Physical;
  }
}

bool PizzaGameEngine::houseAlreadyTargeted(uint8_t houseId) const {
  for (uint8_t i=0;i<m_orderCount;i++) if (m_orders[i].used && m_orders[i].targetHouse == houseId) return true;
  return false;
}

bool PizzaGameEngine::sigAlreadyUsed(const char* sig) const {
  if (!sig || !*sig) return false;
  for (uint8_t i=0;i<m_orderCount;i++) {
    if (m_orders[i].used && strncmp(m_orders[i].clueSig, sig, sizeof(m_orders[i].clueSig)) == 0) return true;
  }
  return false;
}

static void sigSingle(char* out, size_t outSz, PizzaGameEngine::Domain d, const char* tok) {
  const char* p = (d==PizzaGameEngine::Domain::Panel?"P":(d==PizzaGameEngine::Domain::Window?"W":(d==PizzaGameEngine::Domain::Sound?"S":"PH")));
  snprintf(out, outSz, "%s:%s", p, tok?tok:"");
}

static void sigComposite(char* out, size_t outSz,
                         PizzaGameEngine::Domain a, const char* at,
                         PizzaGameEngine::Domain b, const char* bt) {
  const char* ap = (a==PizzaGameEngine::Domain::Panel?"P":(a==PizzaGameEngine::Domain::Window?"W":(a==PizzaGameEngine::Domain::Sound?"S":"PH")));
  const char* bp = (b==PizzaGameEngine::Domain::Panel?"P":(b==PizzaGameEngine::Domain::Window?"W":(b==PizzaGameEngine::Domain::Sound?"S":"PH")));
  snprintf(out, outSz, "%s:%s|%s:%s", ap, at?at:"", bp, bt?bt:"");
}

static void sigRelation(char* out, size_t outSz, PizzaGameEngine::RelOp op, const char* anchorSig) {
  const char* opS = (op==PizzaGameEngine::RelOp::LeftOf?"L":(op==PizzaGameEngine::RelOp::RightOf?"R":"O"));
  snprintf(out, outSz, "R%s(%s)", opS, anchorSig?anchorSig:"");
}

static void makePizzaDesc(char* out, size_t outSz, const PizzaGameEngine::OrderRules& r) {
  if (!out || outSz==0) return;
  out[0]=0;

  auto append=[&](const char* s){
    if (!s || !*s) return;
    if (!out[0]) strlcpy(out, s, outSz);
    else { strlcat(out, " and ", outSz); strlcat(out, s, outSz); }
  };

  for (uint8_t i=0;i<5;i++) if (r.requireMask & kTopBits[i]) append(kTopNames[i]);
  if (!out[0]) strlcpy(out, "plain", outSz);

  if (!r.exactMask) {
    strlcat(out, " (no ", outSz);
    bool first=true;
    for (uint8_t i=0;i<5;i++) if (r.forbidMask & kTopBits[i]) {
      if (!first) strlcat(out, ", ", outSz);
      strlcat(out, kTopNames[i], outSz);
      first=false;
    }
    strlcat(out, ")", outSz);
  }
}

PizzaGameEngine::OrderRules PizzaGameEngine::makePizzaRules(const LevelCfg& cfg) const {
  OrderRules r{};
  r.requireMask = 0;
  r.forbidMask  = 0;
  r.exactMask   = true;

  auto pickBit = [&]() -> uint8_t { return kTopBits[rng() % 5]; };

  if (cfg.pizzaTier == PizzaTier::Single) {
    r.requireMask = pickBit();
    r.exactMask = true;
    return r;
  }

  if (cfg.pizzaTier == PizzaTier::Multi2 || cfg.pizzaTier == PizzaTier::Multi3) {
    uint8_t need = (cfg.pizzaTier == PizzaTier::Multi2) ? 2 : 3;
    while (__builtin_popcount((unsigned)r.requireMask) < need) r.requireMask |= pickBit();
    r.exactMask = true;
    return r;
  }

  // Constraints
  while (__builtin_popcount((unsigned)r.requireMask) < 2) r.requireMask |= pickBit();
  uint8_t forbid = 0;
  uint8_t safety = 0;
  while (((forbid == 0) || (forbid & r.requireMask)) && safety++ < 30) {
    forbid = pickBit();
  }
  if (forbid == 0) {
    // last resort
    forbid = (uint8_t)(~r.requireMask) & 0x1F;
    forbid = (forbid & -forbid);
  }
  r.forbidMask = forbid;
  r.exactMask  = false;
  return r;
}

bool PizzaGameEngine::spawnOneOrder(const LevelCfg& cfg, uint32_t nowMs) {
  if (m_orderCount >= kMaxOrders) return false;

  uint8_t house = 0;
  char dest[96] = {0};
  char sig[64]  = {0};

  bool okClue = false;
  Domain pref = pickPreferredDomain();

  for (uint8_t attempt=0; attempt<14 && !okClue; ++attempt) {
    Domain d = pref;
    if (attempt != 0) {
      // Level 1 has no sound identities, so avoid wasting attempts on sound-domain clues.
      if (m_level <= 1) {
        switch (rng() % 3) {
          default:
          case 0: d = Domain::Panel;    break;
          case 1: d = Domain::Window;   break;
          case 2: d = Domain::Physical; break;
        }
      } else {
        d = (Domain)(rng() % 4);
      }
    }

    okClue = buildUniqueSingle(d, house, dest, sizeof(dest), sig, sizeof(sig));
    if (!okClue && cfg.compositesEnabled) {
      okClue = buildUniqueCompositeAnchored(d, house, dest, sizeof(dest), sig, sizeof(sig));
    }
    if (!okClue && cfg.relationsEnabled) {
      okClue = maybeWrapRelation(cfg, house, dest, sizeof(dest), sig, sizeof(sig));
    }
  }

  if (!okClue || house == 0) return false;
  if (houseAlreadyTargeted(house)) return false;
  if (sigAlreadyUsed(sig)) return false;

  OrderState st{};
  st.used = true;
  st.orderId = m_nextOrderId++;
  st.targetHouse = house;
  st.createdAtMs = nowMs;

  uint16_t tS = cfg.timeout_s;
  if (m_spawnedThisLevel < cfg.firstWaveCount) tS = cfg.timeout_firstWave_s;
  st.expiresAtMs = nowMs + msFromS(tS);

  st.rules = makePizzaRules(cfg);

  char pizzaDesc[64];
  makePizzaDesc(pizzaDesc, sizeof(pizzaDesc), st.rules);

  snprintf(st.clueText, sizeof(st.clueText), "%s pizza for %s", pizzaDesc, dest);
  strlcpy(st.clueSig, sig, sizeof(st.clueSig));

  m_orders[m_orderCount++] = st;
  m_spawnedThisLevel++;
  m_ordersDirty = true;
  syncPayloadFromInternal(nowMs);
  return true;
}

void PizzaGameEngine::syncPayloadFromInternal(uint32_t nowMs) {
  if (!m_outBuf || !m_outCountPtr) return;

  uint8_t cnt = m_orderCount;
  if (cnt > m_outCap) cnt = m_outCap;
  *m_outCountPtr = cnt;

  for (uint8_t i=0;i<cnt;i++) {
    PzOrderItemSetPayload& it = m_outBuf[i];
    const OrderState& st = m_orders[i];

    it.index = i;
    it.house_id = st.targetHouse;
    it.mask = st.rules.requireMask;
    memset(it.text, 0, sizeof(it.text));
    strlcpy(it.text, st.clueText, sizeof(it.text));

    // Keep protocol wire-format deterministic across toolchains.
    it._pad0 = 0;

    it.order_id = st.orderId;

    if (st.expiresAtMs && (int32_t)(st.expiresAtMs - nowMs) > 0) {
      uint32_t remMs = st.expiresAtMs - nowMs;
      uint32_t remS  = (remMs + 999) / 1000;
      it.remain_s = (remS > 65535) ? 65535 : (uint16_t)remS;
    } else {
      it.remain_s = 0;
    }
  }
}

// -----------------------------
// Clue generation
// -----------------------------

bool PizzaGameEngine::buildUniqueSingle(Domain d, uint8_t& outHouse, char* outText, size_t outTextSz,
                                        char* outSig, size_t outSigSz) const {
  outHouse = 0;
  if (!outText || outTextSz==0 || !outSig || outSigSz==0) return false;

  struct Cand { uint8_t h; char tok[24]; };
  Cand cands[6];
  uint8_t cn=0;

  for (uint8_t h=1; h<=kHouseCount; ++h) {
    if (houseAlreadyTargeted(h)) continue;

    if (d == Domain::Physical) {
      // P2: door + handle color
      char tok[24];
      snprintf(tok, sizeof(tok), "%s/%s", doorColorName(m_phys[h].doorColor), handleColorName(m_phys[h].handleColor));

      bool unique=true;
      for (uint8_t h2=1; h2<=kHouseCount; ++h2) {
        if (h2==h) continue;
        char tok2[24];
        snprintf(tok2, sizeof(tok2), "%s/%s", doorColorName(m_phys[h2].doorColor), handleColorName(m_phys[h2].handleColor));
        if (strcmp(tok, tok2)==0) { unique=false; break; }
      }
      if (!unique) continue;

      cands[cn].h = h;
      strlcpy(cands[cn].tok, tok, sizeof(cands[cn].tok));
      cn++;
      continue;
    }

    const char* tok = "";
    if (d == Domain::Panel) tok = m_id[h].panelToken;
    else if (d == Domain::Window) tok = m_id[h].windowToken;
    else if (d == Domain::Sound) tok = m_id[h].soundToken;

    if (!tok || !*tok) continue;

    if (countTok(m_id, d, tok) != 1) continue;

    cands[cn].h = h;
    strlcpy(cands[cn].tok, tok, sizeof(cands[cn].tok));
    cn++;
  }

  if (cn == 0) return false;

  const Cand& sel = cands[rng() % cn];
  outHouse = sel.h;

  sigSingle(outSig, outSigSz, d, sel.tok);
  if (sigAlreadyUsed(outSig)) return false;

  // Render G1 clue
  if (d == Domain::Panel) {
    if (sel.tok[0] == '@') {
      char name[24]={0};
      strlcpy(name, sel.tok+1, sizeof(name));
      // Uppercase in clue for emphasis
      for (char* p=name; *p; ++p) { if (*p>='a' && *p<='z') *p = (char)(*p - 'a' + 'A'); }
      snprintf(outText, outTextSz, "the house with the %s icon", name);
    }
    else if (isDigitsOnly(sel.tok)) {
      snprintf(outText, outTextSz, "house #%s", sel.tok);
    }
    else {
      snprintf(outText, outTextSz, "the house labeled %s", sel.tok);
    }
  }
  else if (d == Domain::Window) {
    snprintf(outText, outTextSz, "the house with %s windows", sel.tok);
  }
  else if (d == Domain::Sound) {
    snprintf(outText, outTextSz, "the house with %s sound", sel.tok);
  }
  else {
    // Physical token is "door/handle"
    const char* slash = strchr(sel.tok, '/');
    if (slash) {
      char dc[16]={0};
      char hc[16]={0};
      size_t a = (size_t)(slash - sel.tok);
      strncpy(dc, sel.tok, (a<sizeof(dc)-1)?a:(sizeof(dc)-1));
      strlcpy(hc, slash+1, sizeof(hc));
      snprintf(outText, outTextSz, "the house with a %s door and %s handle", dc, hc);
    } else {
      snprintf(outText, outTextSz, "the house with %s", sel.tok);
    }
  }

  return true;
}

bool PizzaGameEngine::buildUniqueCompositeAnchored(Domain anchor, uint8_t& outHouse, char* outText, size_t outTextSz,
                                                   char* outSig, size_t outSigSz) const {
  outHouse = 0;
  if (!outText || outTextSz==0 || !outSig || outSigSz==0) return false;

  Domain seconds[3];
  uint8_t sn=0;
  for (uint8_t d=0; d<4; ++d) {
    Domain dd = (Domain)d;
    if (dd == anchor) continue;
    seconds[sn++] = dd;
  }

  struct Cand { uint8_t h; Domain b; char at[24]; char bt[24]; };
  Cand buckets[3][16];
  uint8_t bn[3] = {0,0,0};

  for (uint8_t h=1; h<=kHouseCount; ++h) {
    if (houseAlreadyTargeted(h)) continue;

    // anchor token for this house
    char aTok[24]={0};
    if (anchor == Domain::Physical) {
      snprintf(aTok, sizeof(aTok), "%s/%s", doorColorName(m_phys[h].doorColor), handleColorName(m_phys[h].handleColor));
    } else if (anchor == Domain::Panel) {
      strlcpy(aTok, m_id[h].panelToken, sizeof(aTok));
    } else if (anchor == Domain::Window) {
      strlcpy(aTok, m_id[h].windowToken, sizeof(aTok));
    } else if (anchor == Domain::Sound) {
      strlcpy(aTok, m_id[h].soundToken, sizeof(aTok));
    }
    if (!aTok[0]) continue;

    for (uint8_t si=0; si<sn; ++si) {
      Domain b = seconds[si];
      char bTok[24]={0};

      if (b == Domain::Physical) {
        snprintf(bTok, sizeof(bTok), "%s/%s", doorColorName(m_phys[h].doorColor), handleColorName(m_phys[h].handleColor));
      } else if (b == Domain::Panel) {
        strlcpy(bTok, m_id[h].panelToken, sizeof(bTok));
      } else if (b == Domain::Window) {
        strlcpy(bTok, m_id[h].windowToken, sizeof(bTok));
      } else if (b == Domain::Sound) {
        strlcpy(bTok, m_id[h].soundToken, sizeof(bTok));
      }
      if (!bTok[0]) continue;

      // Count matches across houses
      uint8_t matches=0;
      for (uint8_t h2=1; h2<=kHouseCount; ++h2) {
        char a2[24]={0}, b2[24]={0};

        if (anchor == Domain::Physical) snprintf(a2, sizeof(a2), "%s/%s", doorColorName(m_phys[h2].doorColor), handleColorName(m_phys[h2].handleColor));
        else if (anchor == Domain::Panel) strlcpy(a2, m_id[h2].panelToken, sizeof(a2));
        else if (anchor == Domain::Window) strlcpy(a2, m_id[h2].windowToken, sizeof(a2));
        else if (anchor == Domain::Sound) strlcpy(a2, m_id[h2].soundToken, sizeof(a2));

        if (b == Domain::Physical) snprintf(b2, sizeof(b2), "%s/%s", doorColorName(m_phys[h2].doorColor), handleColorName(m_phys[h2].handleColor));
        else if (b == Domain::Panel) strlcpy(b2, m_id[h2].panelToken, sizeof(b2));
        else if (b == Domain::Window) strlcpy(b2, m_id[h2].windowToken, sizeof(b2));
        else if (b == Domain::Sound) strlcpy(b2, m_id[h2].soundToken, sizeof(b2));

        if (a2[0] && b2[0] && strcmp(aTok, a2)==0 && strcmp(bTok, b2)==0) matches++;
      }

      if (matches != 1) continue;

      if (bn[si] >= 16) continue;
      Cand& c = buckets[si][bn[si]++];
      c.h = h;
      c.b = b;
      strlcpy(c.at, aTok, sizeof(c.at));
      strlcpy(c.bt, bTok, sizeof(c.bt));
    }
  }

  // Choose second domain uniformly among non-empty
  uint8_t nonEmpty[3];
  uint8_t ne=0;
  for (uint8_t i=0;i<sn;i++) if (bn[i] > 0) nonEmpty[ne++] = i;
  if (ne == 0) return false;

  uint8_t group = nonEmpty[rng() % ne];
  const Cand& sel = buckets[group][rng() % bn[group]];

  outHouse = sel.h;
  sigComposite(outSig, outSigSz, anchor, sel.at, seconds[group], sel.bt);
  if (sigAlreadyUsed(outSig)) return false;

  auto domWord=[&](Domain d)->const char*{
    switch (d) {
      case Domain::Panel: return "panel";
      case Domain::Window: return "windows";
      case Domain::Sound: return "sound";
      case Domain::Physical: return "physical";
      default: return "";
    }
  };

  auto prettyTok=[&](Domain d, const char* tok, char* out, size_t outSz){
    if (!tok || !*tok) { strlcpy(out, "", outSz); return; }
    if (d == Domain::Panel) {
      // Panel token is either digits, text, or icon token like "@STAR"
      if (tok[0] == '@') {
        char name[24]={0};
        strlcpy(name, tok+1, sizeof(name));
        // Uppercase for readability
        for (char* p=name; *p; ++p) { if (*p>='a' && *p<='z') *p = (char)(*p - 'a' + 'A'); }
        snprintf(out, outSz, "the %s icon", name);
        return;
      }
      if (isDigitsOnly(tok)) {
        snprintf(out, outSz, "#%s", tok);
        return;
      }
      // normal text
      strlcpy(out, tok, outSz);
      return;
    }

    if (d == Domain::Window) {
      // Make composites read naturally: "windows are disco" etc.
      strlcpy(out, tok, outSz);
      return;
    }

    if (d == Domain::Sound) {
      strlcpy(out, tok, outSz);
      return;
    }

    if (d != Domain::Physical) { strlcpy(out, tok, outSz); return; }

    const char* slash = strchr(tok, '/');
    if (slash) {
      char dc[16]={0}, hc[16]={0};
      size_t a=(size_t)(slash - tok);
      strncpy(dc, tok, (a<sizeof(dc)-1)?a:(sizeof(dc)-1));
      strlcpy(hc, slash+1, sizeof(hc));
      snprintf(out, outSz, "a %s door and %s handle", dc, hc);
    } else {
      strlcpy(out, tok, outSz);
    }
  };

  char aPhrase[48]={0}, bPhrase[48]={0};
  prettyTok(anchor, sel.at, aPhrase, sizeof(aPhrase));
  prettyTok(sel.b,  sel.bt, bPhrase, sizeof(bPhrase));

  snprintf(outText, outTextSz,
           "the house whose %s is %s and whose %s are %s",
           domWord(anchor), aPhrase,
           domWord(sel.b), bPhrase);

  return true;
}

bool PizzaGameEngine::maybeWrapRelation(const LevelCfg& cfg, uint8_t& ioTargetHouse,
                                       char* ioText, size_t ioTextSz,
                                       char* ioSig, size_t ioSigSz) const {
  if (!cfg.relationsEnabled) return false;

  uint8_t anchorHouse=0;
  char anchorText[96]={0};
  char anchorSig[64]={0};

  // Prefer unique panel
  if (!buildUniqueSingle(Domain::Panel, anchorHouse, anchorText, sizeof(anchorText), anchorSig, sizeof(anchorSig))) {
    // Any domain
    for (uint8_t d=0; d<4 && !anchorHouse; ++d) {
      buildUniqueSingle((Domain)d, anchorHouse, anchorText, sizeof(anchorText), anchorSig, sizeof(anchorSig));
    }
  }
  if (!anchorHouse) return false;

  RelOp op = (RelOp)(rng() % 3);
  uint8_t target = 0;
  if (op == RelOp::LeftOf) target = m_leftOf[anchorHouse];
  else if (op == RelOp::RightOf) target = m_rightOf[anchorHouse];
  else target = m_opposite[anchorHouse];

  if (target == 0) return false;
  if (houseAlreadyTargeted(target)) return false;

  const char* opText = (op==RelOp::LeftOf?"left of":(op==RelOp::RightOf?"right of":"opposite"));
  snprintf(ioText, ioTextSz, "the house %s %s", opText, anchorText);
  sigRelation(ioSig, ioSigSz, op, anchorSig);
  if (sigAlreadyUsed(ioSig)) return false;

  ioTargetHouse = target;
  return true;
}

// -----------------------------
// Static facts & relations
// -----------------------------

void PizzaGameEngine::initStaticFacts() {
  // Match the physical facts used in the original Central
  static const uint8_t houseColor[7] = {0, HC_BLUE,    HC_RED,    HC_YELLOW, HC_BLUE,   HC_YELLOW, HC_RED};
  static const uint8_t doorColor [7] = {0, DC_WHITE,   DC_GREY,   DC_BROWN,  DC_GREY,   DC_WHITE,  DC_BROWN};
  static const uint8_t handleShape[7]= {0, HS_ROUND,   HS_ROUND,  HS_BAR,    HS_ROUND,  HS_ROUND,  HS_ROUND};
  static const uint8_t handleColor[7]= {0, HCOL_SILVER,HCOL_SILVER,HCOL_SILVER,HCOL_GOLD,HCOL_GOLD,HCOL_GOLD};

  for (uint8_t h=1; h<=6; ++h) {
    m_phys[h].houseColor = houseColor[h];
    m_phys[h].doorColor  = doorColor[h];
    m_phys[h].handleShape= handleShape[h];
    m_phys[h].handleColor= handleColor[h];
  }

  // Relations: match the physical room mapping
  // - Right side: 1,2,3 (in that order)
  // - Left side:  6,5,4 (in that order)
  // - Across: 3<->4, 2<->5, 1<->6
  //
  // We treat leftOf/rightOf as "immediately adjacent on the SAME side".
  // End houses have 0 for the missing neighbor.
  for (uint8_t h=0; h<=6; ++h) {
    m_leftOf[h] = 0;
    m_rightOf[h] = 0;
    m_opposite[h] = 0;
  }

  // Right side adjacency
  m_rightOf[1] = 2;
  m_leftOf[2]  = 1;  m_rightOf[2] = 3;
  m_leftOf[3]  = 2;

  // Left side adjacency
  m_rightOf[6] = 5;
  m_leftOf[5]  = 6;  m_rightOf[5] = 4;
  m_leftOf[4]  = 5;

  // Across
  m_opposite[1] = 6; m_opposite[6] = 1;
  m_opposite[2] = 5; m_opposite[5] = 2;
  m_opposite[3] = 4; m_opposite[4] = 3;
}

// -----------------------------
// Debug
// -----------------------------

void PizzaGameEngine::debugPrintMapping() const {
  Serial.printf("[Engine] Level %u mapping:\n", (unsigned)m_level);
  for (uint8_t h=1; h<=6; ++h) {
    Serial.printf("  H%u: panel='%s' window='%s' sound='%s' (clip=%u vol=%u)\n",
                  h,
                  m_id[h].panelToken,
                  m_id[h].windowToken,
                  m_id[h].soundToken,
                  (unsigned)m_id[h].spkClip,
                  (unsigned)m_id[h].spkVol);
  }
}

void PizzaGameEngine::debugPrintOrders(uint32_t nowMs) const {
  Serial.printf("[Engine] Orders (%u):\n", (unsigned)m_orderCount);
  for (uint8_t i=0;i<m_orderCount;i++) {
    const OrderState& o = m_orders[i];
    uint16_t rem = 0;
    if (o.expiresAtMs && (int32_t)(o.expiresAtMs - nowMs) > 0) {
      uint32_t remMs = o.expiresAtMs - nowMs;
      rem = (uint16_t)((remMs + 999) / 1000);
    }
    Serial.printf("  #%u id=%u H%u rem=%us mask=0x%02X text=\"%s\"\n",
                  (unsigned)i,
                  (unsigned)o.orderId,
                  (unsigned)o.targetHouse,
                  (unsigned)rem,
                  (unsigned)o.rules.requireMask,
                  o.clueText);
  }
}
