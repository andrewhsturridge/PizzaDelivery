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

// Window token catalog (minimal but expandable)
struct WinTok {
  const char* token; // lower-case name used in clue text
  uint8_t fx;
  uint8_t h,s,v;
  uint8_t speed;
};

static const WinTok kWinSolids[] = {
  {"red",    WIN_FX_SOLID, 0,   255, 130, 0},
  {"blue",   WIN_FX_SOLID, 170, 255, 130, 0},
  {"green",  WIN_FX_SOLID, 85,  255, 130, 0},
  {"yellow", WIN_FX_SOLID, 42,  255, 130, 0},
  {"purple", WIN_FX_SOLID, 200, 255, 130, 0},
  {"cyan",   WIN_FX_SOLID, 120, 255, 130, 0},
  {"orange", WIN_FX_SOLID, 18,  255, 130, 0},
};

static const WinTok kWinPatterns[] = {
  // W2
  {"disco",    WIN_FX_PARTY,   0, 200, 120, 220},
  {"rainbow",  WIN_FX_RAINBOW, 0, 255, 120, 8},
  // W3 direction-like variants using RAINBOW with speed sign encoded in uint8
  // NOTE: HouseNode can treat speed >127 as negative steps for CCW.
  {"chase cw",  WIN_FX_RAINBOW, 0, 255, 120, 10},
  {"chase ccw", WIN_FX_RAINBOW, 0, 255, 120, 246},
};

struct SoundTok {
  const char* token; // lower-case name used in clue text
  uint8_t clip;
};

// Minimal starter set. Clip IDs must exist on house nodes for sound to play.
static const SoundTok kSoundS1[] = {
  {"cat",      1},
  {"dog",      2},
  {"doorbell", 3},
  {"alarm",    4},
  {"music",    5},
  {"baby",     6},
  {"angry",    7},
  {"beeps",    8},
};

static const SoundTok kSoundS2[] = {
  {"double beep", 10},
  {"triple beep", 11},
  {"beep-long",   12},
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
  m_phase = Phase::Running;
  m_level = 1;
  m_livesLeft = m_livesMax;
  m_successInLevel = 0;
  m_successTotal   = 0;
  m_nextOrderId    = 1;
  m_spawnedThisLevel = 0;
  m_ordersDirty = true;
  startLevel(1, nowMs);
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
      c.timeout_s = 25; c.timeout_firstWave_s = 30; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Single;
      c.compositesEnabled = false;
      c.relationsEnabled  = false;
      break;
    case 2:
      c.N_success = 4; c.maxActive = 2;
      c.timeout_s = 20; c.timeout_firstWave_s = 25; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Single;
      c.compositesEnabled = true;
      c.relationsEnabled  = false;
      break;
    case 3:
      c.N_success = 5; c.maxActive = 3;
      c.timeout_s = 18; c.timeout_firstWave_s = 22; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Multi2;
      c.compositesEnabled = true;
      c.relationsEnabled  = false;
      break;
    case 4:
      c.N_success = 6; c.maxActive = 3;
      c.timeout_s = 16; c.timeout_firstWave_s = 20; c.firstWaveCount = 2;
      c.pizzaTier = PizzaTier::Multi3;
      c.compositesEnabled = true;
      c.relationsEnabled  = false;
      break;
    case 5:
      c.N_success = 6; c.maxActive = 3;
      c.timeout_s = 15; c.timeout_firstWave_s = 20; c.firstWaveCount = 2;
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

  // Panel T1 unique numbers baseline
  char nums[7][8] = {{0}};
  makeUniqueNumbers(nums, m_io.rand32);

  auto applyPanelT1 = [&](uint8_t h, const char* digits) {
    m_id[h].panelMode = PANEL_MODE_NUMBER;
    strlcpy(m_id[h].panelText, digits, sizeof(m_id[h].panelText));
    strlcpy(m_id[h].panelToken, digits, sizeof(m_id[h].panelToken));
  };
  auto applyPanelT2 = [&](uint8_t h, const char* expr) {
    m_id[h].panelMode = PANEL_MODE_TEXT;
    strlcpy(m_id[h].panelText, expr, sizeof(m_id[h].panelText));
    strlcpy(m_id[h].panelToken, expr, sizeof(m_id[h].panelToken));
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

  // Select 6 distinct window solids indices
  uint8_t winIdx[6];
  {
    uint8_t poolN = (uint8_t)(sizeof(kWinSolids)/sizeof(kWinSolids[0]));
    uint8_t pool[16];
    for (uint8_t i=0;i<poolN;i++) pool[i]=i;
    shuffleU8(pool, poolN, m_io.rand32);
    for (uint8_t i=0;i<6;i++) winIdx[i]=pool[i];
  }

  // Select 6 distinct sound indices
  uint8_t sndIdx[6];
  {
    uint8_t poolN = (uint8_t)(sizeof(kSoundS1)/sizeof(kSoundS1[0]));
    uint8_t pool[16];
    for (uint8_t i=0;i<poolN;i++) pool[i]=i;
    shuffleU8(pool, poolN, m_io.rand32);
    for (uint8_t i=0;i<6;i++) sndIdx[i]=pool[i];
  }

  if (lvl == 1) {
    // Panel: T1 all unique
    for (uint8_t h=1; h<=6; ++h) applyPanelT1(h, nums[h]);
    // Window: W1 all unique solids
    for (uint8_t h=1; h<=6; ++h) applyWinTok(h, kWinSolids[winIdx[h-1]]);
    // Sound: S1 all unique
    for (uint8_t h=1; h<=6; ++h) applySoundTok(h, kSoundS1[sndIdx[h-1]], 70);
    return;
  }

  if (lvl == 2) {
    // Panel: T1 all unique
    for (uint8_t h=1; h<=6; ++h) applyPanelT1(h, nums[h]);

    // Default: windows unique + sounds unique
    for (uint8_t h=1; h<=6; ++h) {
      applyWinTok(h, kWinSolids[winIdx[h-1]]);
      applySoundTok(h, kSoundS1[sndIdx[h-1]], 70);
    }

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
      // Window 2/2/1/1
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
    // Panel: 3 T1 numbers, 3 T2 equations
    uint8_t hh[6] = {1,2,3,4,5,6};
    shuffleU8(hh, 6, m_io.rand32);
    bool isEq[7] = {0};
    for (uint8_t i=0;i<3;i++) isEq[hh[i]] = true;

    char eqs[3][12];
    makeUniqueEquations(eqs, 3, m_io.rand32);
    uint8_t ei = 0;

    for (uint8_t h=1; h<=6; ++h) {
      if (isEq[h]) applyPanelT2(h, eqs[ei++]);
      else applyPanelT1(h, nums[h]);
    }

    // Window: 1 disco, rest solids
    uint8_t discoHouse = (uint8_t)(1 + (rng() % 6));
    for (uint8_t h=1; h<=6; ++h) {
      if (h == discoHouse) applyWinTok(h, kWinPatterns[0]);
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
    // Panel: T2 all equations
    char eqs[6][12];
    makeUniqueEquations(eqs, 6, m_io.rand32);
    for (uint8_t h=1; h<=6; ++h) applyPanelT2(h, eqs[h-1]);

    // Windows: 3 disco, 3 rainbow
    uint8_t wh[6] = {1,2,3,4,5,6};
    shuffleU8(wh, 6, m_io.rand32);
    for (uint8_t i=0;i<3;i++) applyWinTok(wh[i], kWinPatterns[0]);
    for (uint8_t i=3;i<6;i++) applyWinTok(wh[i], kWinPatterns[1]);

    // Sound: S2 2/2 + S1 1/1
    uint8_t sh[6] = {1,2,3,4,5,6};
    shuffleU8(sh, 6, m_io.rand32);
    applySoundTok(sh[0], kSoundS2[0], 70);
    applySoundTok(sh[1], kSoundS2[0], 70);
    applySoundTok(sh[2], kSoundS2[1], 70);
    applySoundTok(sh[3], kSoundS2[1], 70);
    applySoundTok(sh[4], kSoundS1[sndIdx[0]], 70);
    applySoundTok(sh[5], kSoundS1[sndIdx[1]], 70);

    // Bucket overlay: either brightness buckets on disco, OR volume buckets on 3 dogs
    bool bucketVolume = ((rng() % 2) == 1);
    if (!bucketVolume) {
      // dim/normal/bright disco buckets on the 3 disco houses
      // Find the disco houses: those with token "disco"
      uint8_t disco[3];
      uint8_t dn=0;
      for (uint8_t h=1; h<=6 && dn<3; ++h) {
        if (strcmp(m_id[h].windowToken, "disco") == 0) disco[dn++] = h;
      }
      if (dn==3) {
        m_id[disco[0]].winV = 60;  strlcpy(m_id[disco[0]].windowToken, "dim disco", sizeof(m_id[disco[0]].windowToken));
        m_id[disco[1]].winV = 120; strlcpy(m_id[disco[1]].windowToken, "disco", sizeof(m_id[disco[1]].windowToken));
        m_id[disco[2]].winV = 200; strlcpy(m_id[disco[2]].windowToken, "bright disco", sizeof(m_id[disco[2]].windowToken));
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
    char eqs[6][12];
    makeUniqueEquations(eqs, 6, m_io.rand32);
    for (uint8_t h=1; h<=6; ++h) applyPanelT2(h, eqs[h-1]);

    // Windows: W3 2/2/2 (chase cw, chase ccw, disco)
    uint8_t wh[6] = {1,2,3,4,5,6};
    shuffleU8(wh, 6, m_io.rand32);
    applyWinTok(wh[0], kWinPatterns[2]);
    applyWinTok(wh[1], kWinPatterns[2]);
    applyWinTok(wh[2], kWinPatterns[3]);
    applyWinTok(wh[3], kWinPatterns[3]);
    applyWinTok(wh[4], kWinPatterns[0]);
    applyWinTok(wh[5], kWinPatterns[0]);

    // Sound: S2 2/2/2
    uint8_t sh[6] = {1,2,3,4,5,6};
    shuffleU8(sh, 6, m_io.rand32);
    applySoundTok(sh[0], kSoundS2[0], 70);
    applySoundTok(sh[1], kSoundS2[0], 70);
    applySoundTok(sh[2], kSoundS2[1], 70);
    applySoundTok(sh[3], kSoundS2[1], 70);
    applySoundTok(sh[4], kSoundS2[2], 70);
    applySoundTok(sh[5], kSoundS2[2], 70);
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
    Domain d = (attempt==0) ? pref : (Domain)(rng() % 4);

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
    if (isDigitsOnly(sel.tok)) snprintf(outText, outTextSz, "house #%s", sel.tok);
    else snprintf(outText, outTextSz, "the house labeled %s", sel.tok);
  }
  else if (d == Domain::Window) {
    snprintf(outText, outTextSz, "the %s windows house", sel.tok);
  }
  else if (d == Domain::Sound) {
    snprintf(outText, outTextSz, "the %s house", sel.tok);
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

  // Relations: ring for left/right; opposite is +3 in ring
  for (uint8_t h=1; h<=6; ++h) {
    uint8_t left  = (h==1)?6:(h-1);
    uint8_t right = (h==6)?1:(h+1);
    uint8_t opp   = (uint8_t)(((h + 2) % 6) + 1);
    m_leftOf[h]  = left;
    m_rightOf[h] = right;
    m_opposite[h]= opp;
  }
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
