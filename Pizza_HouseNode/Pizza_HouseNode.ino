// Role: HOUSE_NODE (Feather S3)
// Scans RC522 and broadcasts DELIVER_SCAN on new UID.
// Blinks window LEDs and beeps on DELIVER_RESULT(ok).

#define PIZZA_ROLE HOUSE_NODE
#define PIZZA_ENABLE_RFID_MODULE
#define PIZZA_ENABLE_LEDS_MODULE
#define PIZZA_ENABLE_AUDIO_MODULE

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <FS.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <esp_wifi.h>

#include "PizzaProtocol.h"
#include "PizzaNow.h"
#include "PizzaIdentity.h"
#include "PizzaUtils.h"
#include "BuildConfig.h"
#include "PizzaRfid.h"
#include "PizzaAudio.h"
#include "PizzaOta.h"
#include "PizzaAudioFS.h"
#include "PizzaNetCfg.h"

static uint16_t g_seq = 1;
Preferences prefs;
static uint8_t g_houseId = 0;  // runtime house id from NVS
static uint32_t g_helloDueAt = 0;

// --- Pins (House mapping) ---
static const uint8_t  PIN_WS2812 = 38;
static const uint16_t LED_COUNT  = 90;
static const uint8_t  RC522_CS   = 5;
static const uint8_t  RC522_RST  = 11;
static const uint8_t  SPI_SCK    = 36, SPI_MISO = 37, SPI_MOSI = 35;

// Audio I2S pins on the House
constexpr int I2S_BCLK = 43;   // example
constexpr int I2S_LRCK = 44;   // example
constexpr int I2S_DOUT = 12;   // example

// --- OTA deferral ---
static volatile bool g_otaPending = false;
static char g_otaUrl[160] = {0};
static char g_otaVer[12]  = {0};

// --- clip sync deferral ---
static volatile bool g_assetPending = false;
static volatile bool g_assetBusy    = false;
static AssetSyncPayload g_assetReq; // saved request copied from onRx

Adafruit_NeoPixel strip(LED_COUNT, PIN_WS2812, NEO_GRB + NEO_KHZ800);

// --- Beep buffer (22050 Hz, ~200ms, 1kHz sine) ---
static int16_t beepBuf[4410/2]; // ~200 ms (~22050*0.2) -> 4410 samples; halve for size

// Smooth 16-bit mono WAV @ 22,050 Hz with fade in/out + tiny silence tail
static bool writeBeepWav(const char* path, float freq_hz, uint16_t ms, uint8_t amplitude /*0..255*/) {
  const uint32_t sr = 22050;
  uint32_t samples = (sr * ms) / 1000;
  if (samples < 10) samples = 10;

  // ~6 ms fade to kill clicks
  const uint32_t fadeMs = 6;
  uint32_t fade = (sr * fadeMs) / 1000;
  if (fade*2 >= samples) fade = samples/4;   // guard for very short clips

  // add a 3 ms silence tail to avoid pop at file end
  const uint32_t tailSilence = (sr * 3) / 1000;

  // amplitude with headroom (prevent inter-sample clipping)
  const float amp = (amplitude / 255.0f) * 28000.0f;  // ~-2.3 dBFS headroom

  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return false;

  // WAV header
  const uint32_t dataBytes = (samples + tailSilence) * 2;
  const uint32_t riffSize  = 36 + dataBytes;
  auto w16=[&](uint16_t v){ f.write((uint8_t*)&v,2); };
  auto w32=[&](uint32_t v){ f.write((uint8_t*)&v,4); };
  f.write((const uint8_t*)"RIFF",4); w32(riffSize);
  f.write((const uint8_t*)"WAVE",4);
  f.write((const uint8_t*)"fmt ",4); w32(16); w16(1); w16(1); w32(sr); w32(sr*2); w16(2); w16(16);
  f.write((const uint8_t*)"data",4); w32(dataBytes);

  // Samples with linear fade in/out (zero phase so start at 0)
  const float omega = 2.0f * 3.14159265f * freq_hz / sr;
  for (uint32_t i=0; i<samples; ++i) {
    float env = 1.0f;
    if (i < fade)            env = (float)i / (float)fade;                 // fade-in
    else if (i > samples-fade) env = (float)(samples-i) / (float)fade;     // fade-out
    float s = sinf(omega * i) * env;
    int16_t v = (int16_t)(amp * s);
    f.write((uint8_t*)&v, 2);
  }
  // silence tail
  int16_t z = 0;
  for (uint32_t i=0; i<tailSilence; ++i) f.write((uint8_t*)&z, 2);

  f.close();
  return true;
}

// Call this once in setup() after LittleFS.begin()
static void ensureBeepWavs() {
  if (!LittleFS.begin()) LittleFS.begin(true);
  LittleFS.mkdir("/clips");
  if (!LittleFS.exists("/clips/090.wav")) writeBeepWav("/clips/090.wav", 1000.0f, 200, 180); // OK beep
  if (!LittleFS.exists("/clips/091.wav")) writeBeepWav("/clips/091.wav",  400.0f, 200, 180); // ERR beep
}

// LED effect state (driven from loop)
enum Effect { EFFECT_NONE, EFFECT_OK_PULSE, EFFECT_ERR_PULSE, EFFECT_YELLOW_PING };
static Effect   g_fx      = EFFECT_NONE;
static uint32_t g_fxUntil = 0;   // millis() deadline for one-shot effects

/*** Delivery FSM (scan -> await result -> require removal) ***/
enum HState : uint8_t { HS_IDLE=0, HS_WAIT_RESULT, HS_WAIT_REMOVAL };
static HState  g_state = HS_IDLE;

static bool    g_haveResult = false;
static bool    g_lastOk = false;
static uint8_t g_lastReason = 0;

static uint8_t  g_lastUid[10];
static uint8_t  g_lastLen = 0;

static const uint32_t RESULT_TIMEOUT_MS = 1500;   // wait for Central
static const uint32_t REMOVAL_STABLE_MS = 250;    // require stable absence
static uint32_t g_stateDeadline = 0;
static uint32_t g_absentSince  = 0;

// window fx  state
static uint8_t  g_winFx   = WIN_FX_OFF;
static uint8_t  g_winH=0, g_winS=0, g_winV=0, g_winSpd=0;
static uint32_t g_nextFxAt = 0;     // scheduling for rainbow/party

static TaskHandle_t g_audioTask = nullptr;

// Forward declares
static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t srcMac[6]);

static void audioTask(void*){
  // High-ish priority, very lightweight loop
  for(;;){
    PizzaAudioFS::loop();
    vTaskDelay(1); // ~1ms yield keeps buffers happy
  }
}

static void fillBeep() {
  const float freq = 1000.0f, sr = 22050.0f;
  for (size_t i=0; i<sizeof(beepBuf)/sizeof(beepBuf[0]); ++i) {
    float t = (float)i / sr;
    float s = sinf(2.0f*PI*freq*t);
    beepBuf[i] = (int16_t)(s * 16000); // ~-12 dBFS
  }
}

// Play built-in OK/ERR beeps; auto-generate WAVs if missing.
static void playResultBeep(bool ok, uint8_t vol) {
  if (!LittleFS.begin()) LittleFS.begin(true);
  LittleFS.mkdir("/clips");
  // ensure files exist
  if (!LittleFS.exists("/clips/090.wav")) writeBeepWav("/clips/090.wav", 1000.0f, 200, 180);
  if (!LittleFS.exists("/clips/091.wav")) writeBeepWav("/clips/091.wav",  400.0f, 200, 180);

  PizzaAudioFS::setVolume(vol);
  uint8_t clip = ok ? 90 : 91;
  if (!PizzaAudioFS::playClip(clip, /*loop=*/false)) {
    Serial.printf("[House %u] playClip(%03u) failed\n", g_houseId, clip);
  }
}

// Utility: tiny HSV→RGB for Adafruit_NeoPixel (0..255 each)
static uint32_t hsv2rgb(uint8_t h, uint8_t s, uint8_t v){
  // simple wheel: map h(0..255) to 0..1530 steps
  uint16_t i = (uint16_t)h * 6; uint8_t f = (h * 6) & 0xFF;
  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - (uint16_t)s * f / 255) / 255;
  uint8_t t = (uint16_t)v * (255 - (uint16_t)s * (255 - f) / 255) / 255;
  uint8_t r,g,b;
  switch (i>>8){
    case 0: r=v; g=t; b=p; break; case 1: r=q; g=v; b=p; break;
    case 2: r=p; g=v; b=t; break; case 3: r=p; g=q; b=v; break;
    case 4: r=t; g=p; b=v; break; default: r=v; g=p; b=q; break;
  }
  return strip.Color(r,g,b);
}

// Apply window effect immediately (solid) or schedule (animated)
static void applyWindow(uint8_t fx, uint8_t h, uint8_t s, uint8_t v, uint8_t spd){
  g_winFx = fx; g_winH=h; g_winS=s; g_winV=v; g_winSpd=spd;
  if (fx == WIN_FX_OFF){
    strip.clear(); strip.show();
  } else if (fx == WIN_FX_SOLID){
    uint32_t c = hsv2rgb(h,s,v);
    for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, c);
    strip.show();
  } else {
    // animated modes tick in loop()
    g_nextFxAt = 0;
  }
}

// Asset Sync: to pivot radio for HTTP (Pizza_HouseNode.ino)
static void doAssetSync(const AssetSyncPayload* ap) {
  Serial.printf("[House %u] ASSET_SYNC start: url=\"%s\" count=%u\n",
                g_houseId, ap->base_url, ap->count);

  uint8_t okCount = 0;
  uint8_t code    = 0;   // 0=all ok, 2=some failed, 10=wifi fail

  if (!LittleFS.begin()) LittleFS.begin(true);
  LittleFS.mkdir("/clips");

  // 1) Pause ESPNOW exactly like OTA
  PizzaNow::deinit();
  delay(50);

  // 2) Join Wi-Fi using the OTA path (PizzaOta::beginWifi should use NetCfg::load)
  if (!PizzaOta::beginWifi(20000)) {
    Serial.printf("[House %u] ASSET_SYNC WiFi connect failed\n", g_houseId);
    code = 10;
  } else {
    okCount = 0;
    code    = 0; // assume success; mark 2 on any file failure

    // 3) Pull files 1..count
    for (uint8_t i = 1; i <= ap->count; ++i) {
      char url[160];  snprintf(url,  sizeof(url),  "%s/%03u.wav", ap->base_url, (unsigned)i);
      char path[32];  snprintf(path, sizeof(path), "/clips/%03u.wav", (unsigned)i);
      Serial.printf("[House %u] GET %s -> %s\n", g_houseId, url, path);

      WiFiClient client;
      client.setTimeout(20000);

      HTTPClient http;
      http.setConnectTimeout(15000);
      http.setTimeout(20000);
      http.setReuse(false);
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.useHTTP10(true);

      if (!http.begin(client, url)) {
        Serial.println("[ASSET] http.begin failed — skipping");
        code = 2;
        http.end();
        continue;
      }

      int rc = http.GET();
      Serial.printf("[ASSET] HTTP GET -> %d\n", rc);
      if (rc != HTTP_CODE_OK) {
        code = 2; // partial failure
        http.end();
        continue;
      }

      File f = LittleFS.open(path, FILE_WRITE);
      if (!f) {
        Serial.println("[ASSET] open path failed — skipping");
        code = 2;
        http.end();
        continue;
      }

      WiFiClient* s = http.getStreamPtr();
      uint8_t buf[2048];
      int r = 0;
      size_t total = 0;
      while ((r = s->readBytes((char*)buf, sizeof(buf))) > 0) {
        f.write(buf, r);
        total += r;
      }
      f.close();
      http.end();

      okCount++;
      Serial.printf("[House %u] WROTE %s (%u bytes)\n", g_houseId, path, (unsigned)total);
    }
  }

  // 4) Leave Wi-Fi (OTA path), restore ESPNOW
  PizzaOta::endWifi();
  delay(30);
  PizzaNow::begin(ESPNOW_CHANNEL);
  PizzaNow::onReceive(onRx);

  // 5) Report result to Central (over ESPNOW)
  AssetResultPayload ar{};
  ar.house_id   = g_houseId;
  ar.ok         = (okCount == ap->count) ? 1 : 0;
  ar.count_done = okCount;
  ar.code       = code;

  uint8_t out[64];
  size_t n = PizzaProtocol::pack(ASSET_RESULT, (Role)PIZZA_ROLE, g_houseId, g_seq++, &ar, sizeof(ar), out, sizeof(out));
  PizzaNow::sendBroadcast(out, n);
  Serial.printf("[House %u] ASSET_SYNC done: ok=%u count=%u code=%u\n",
                g_houseId, ar.ok, ar.count_done, ar.code);

  // 6) Reboot on success (mirrors OTA’s success behavior)
  if (ar.ok) {
    Serial.println("[House] ASSET_SYNC success → rebooting...");
    delay(200);
    ESP.restart();
  }
}

static void sendHello() {
  HelloPayload hp{};
  strlcpy(hp.fw, PizzaIdentity::fw(), sizeof(hp.fw));
  hp.proto = PROTOCOL_VERSION;
  PizzaIdentity::mac(hp.mac);

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(HELLO, (Role)PIZZA_ROLE, g_houseId, g_seq++, &hp, sizeof(hp), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("HELLO sent (node) id=%u", (unsigned)g_houseId);
}

static void sendDeliverScan(const uint8_t* uid, uint8_t uidLen) {
  DeliverScanPayload p{};
  p.house_id = g_houseId;
  p.uid_len  = uidLen;
  memcpy(p.uid, uid, min<uint8_t>(uidLen, sizeof(p.uid)));

  uint8_t buf[128];
  size_t n = PizzaProtocol::pack(DELIVER_SCAN, (Role)PIZZA_ROLE, g_houseId, g_seq++, &p, sizeof(p), buf, sizeof(buf));
  PizzaNow::sendBroadcast(buf, n);
  PZ_LOGI("DELIVER_SCAN uidLen=%u", uidLen);
}

static void onRx(const MsgHeader& hdr, const uint8_t* payload, uint16_t len, const uint8_t /*srcMac*/[6]) {
  if (hdr.type == HELLO_REQ) {
    uint32_t jitter = 50 + (esp_random() % 300); // 50..350 ms
    g_helloDueAt = millis() + jitter + ((g_houseId & 7) * 40);
    return;
  }

  /*** onRx: delivery verdict from Central ***/
  if (hdr.type == DELIVER_RESULT && len >= sizeof(DeliverResultPayload)) {
    const DeliverResultPayload* r = (const DeliverResultPayload*)payload;
    if (hdr.house_id == g_houseId) {
      g_haveResult = true;
      g_lastOk     = (r->ok != 0);
      g_lastReason = r->reason;

      if (g_lastOk) {
        g_fx = EFFECT_OK_PULSE; g_fxUntil = millis() + 600;
        playResultBeep(true, 180);           // <— play OK beep
        PZ_LOGI("DELIVER_RESULT OK");
      } else {
        g_fx = EFFECT_ERR_PULSE; g_fxUntil = millis() + 600;
        playResultBeep(false, 160);          // <— play ERR beep
        PZ_LOGI("DELIVER_RESULT ERR reason=%u", r->reason);
      }
    }
    return;
  }

  if (hdr.type == SOUND_PLAY && len >= sizeof(SoundPlayPayload)) {
    const SoundPlayPayload* sp = (const SoundPlayPayload*)payload;
    if (sp->house_id == g_houseId) {
      uint8_t vol = sp->vol ? sp->vol : 200;
      PizzaAudioFS::setVolume(vol);
      if (!PizzaAudioFS::playClip(sp->clip_id, /*loop=*/false)) {
        Serial.printf("[House %u] SOUND_PLAY: missing /clips/%03u.wav\n", g_houseId, sp->clip_id);
      }
      g_fx = EFFECT_YELLOW_PING; g_fxUntil = millis() + 240;
      PZ_LOGI("SOUND_PLAY clip=%u vol=%u", sp->clip_id, vol);
    }
    return;
  }

  if (hdr.type == CLAIM && len >= sizeof(ClaimPayload)) {
    const ClaimPayload* cp = (const ClaimPayload*)payload;

    uint8_t myMac[6]; PizzaIdentity::mac(myMac);
    bool macMatch = memcmp(myMac, cp->target_mac, 6) == 0;

    if (macMatch && (g_houseId == 0 || cp->force)) {
      PZ_LOGI("CLAIM received: setting house_id=%u (force=%u)", cp->house_id, cp->force);
      cfgSaveHouseId(cp->house_id);
      delay(50);
      ESP.restart();  // come back with new ID and HELLO
    }
  }

  if (hdr.type == OTA_START && len >= sizeof(OtaStartPayload)) {
    const OtaStartPayload* p = (const OtaStartPayload*)payload;
    if (!matchOtaTarget(p)) return;

    // ACK quickly (still ok from callback)
    OtaAckPayload ack{}; ack.accept = 1; ack.code = 0;
    uint8_t out[64];
    size_t n = PizzaProtocol::pack(OTA_ACK, (Role)PIZZA_ROLE, g_houseId, g_seq++, &ack, sizeof(ack), out, sizeof(out));
    PizzaNow::sendBroadcast(out, n);

    // Defer actual OTA to loop()
    strlcpy(g_otaUrl, p->url, sizeof(g_otaUrl));
    strlcpy(g_otaVer, p->ver, sizeof(g_otaVer));
    g_otaPending = true;
    PZ_LOGI("OTA queued: %s", g_otaUrl);
    return;
  }

  if (hdr.type == HOUSE_DIGITAL_SET && len >= sizeof(HouseDigitalSetPayload)) {
    const HouseDigitalSetPayload* p = (const HouseDigitalSetPayload*)payload;
    if (p->house_id == g_houseId) {
      // Window LEDs
      if (p->flags & 0x01) {
        applyWindow(p->win_fx, p->win_h, p->win_s, p->win_v, p->win_speed);
      }

      // Speaker (FS-based audio)
      if (p->flags & 0x04) {
        if (p->spk_flags & 0x02) {
          PizzaAudioFS::stop();
        } else {
          PizzaAudioFS::setVolume(p->spk_vol);
          if (!PizzaAudioFS::playClip(p->spk_clip, (p->spk_flags & 0x01))) {
            Serial.printf("[House %u] Missing /clips/%03u.wav – no audio\n", g_houseId, p->spk_clip);
          }
        }
      }
    }
    return;
  }

  if (hdr.type == ASSET_SYNC && len >= sizeof(AssetSyncPayload)) {
    const AssetSyncPayload* ap = (const AssetSyncPayload*)payload;
    if (ap->house_id == g_houseId) {
      if (g_assetBusy || g_assetPending) {
        Serial.printf("[House %u] ASSET_SYNC ignored (busy)\n", g_houseId);
      } else {
        memcpy((void*)&g_assetReq, ap, sizeof(AssetSyncPayload));
        g_assetPending = true;          // signal main loop to perform sync
        Serial.printf("[House %u] ASSET_SYNC queued (url=%s count=%u)\n",
                      g_houseId, g_assetReq.base_url, g_assetReq.count);
      }
    }
    return;
  }

  if (hdr.type == NET_CFG_SET && len >= sizeof(NetCfgSetPayload)) {
    const NetCfgSetPayload* p = (const NetCfgSetPayload*)payload;

    NetCfg::Value v{};
    strlcpy(v.ssid, p->ssid, sizeof(v.ssid));
    strlcpy(v.pass, p->pass, sizeof(v.pass));
    strlcpy(v.base, p->base, sizeof(v.base));

    bool ok = NetCfg::save(v);
    Serial.printf("[Node] NET_CFG_SET: ssid=\"%s\" base=\"%s\" save=%s\n",
                  v.ssid, v.base, ok?"OK":"FAIL");

    // Optional: immediate apply strategy
    // If you run STA for asset/OTA fetch, you can reconnect here, or just reboot:
    esp_restart();  // <— simplest: reboot to apply new Wi-Fi/host settings
    return;
  }

}

static void cfgLoad() {
  prefs.begin("pizza", false);
  g_houseId = prefs.getUChar("house_id", 0);  // 0 = unclaimed
  prefs.end();
}

static void cfgSaveHouseId(uint8_t id) {
  prefs.begin("pizza", false);
  prefs.putUChar("house_id", id);
  prefs.end();
  g_houseId = id;
}

static bool matchOtaTarget(const OtaStartPayload* p) {
  if (p->target_role != (uint8_t)PIZZA_ROLE) return false;
  if (p->scope == 0) return true; // ALL
  for (uint8_t i=0; i<sizeof(p->ids); i++) if (p->ids[i] == g_houseId) return true;
  return false;
}

// Progress -> LED bar fill
static void nodeOtaProgress(size_t written, size_t total) {
  static uint8_t lastPct = 255;
  uint8_t pct = 0;
  if (total > 0) {
    pct = (uint8_t)((written * 100ULL) / total);
  } else {
    static uint8_t s=0; s = (s + 3) % 100; pct = s; // spinner
  }
  if (pct == lastPct) return;
  lastPct = pct;

  // map to pixels
  uint16_t lit = (uint16_t)((pct * (uint32_t)LED_COUNT) / 100);
  for (uint16_t i=0; i<LED_COUNT; i++) {
    // yellow-ish progress color
    strip.setPixelColor(i, (i < lit) ? strip.Color(80,80,0) : 0);
  }
  strip.show();

  // brief all-green at 100% (will show ~250ms before reboot)
  if (pct == 100) {
    for (uint16_t i=0; i<LED_COUNT; i++) strip.setPixelColor(i, strip.Color(0,80,0));
    strip.show();
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);  // let USB/serial settle

  // LEDs first
  pinMode(PIN_WS2812, OUTPUT);
  strip.begin();
  strip.setBrightness(128);
  strip.clear(); strip.show();

  // quick proof-of-life
  for (int k=0;k<3;k++){
    uint32_t c=strip.Color(k==0?255:0, k==1?255:0, k==2?255:0);
    for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i,c);
    strip.show(); delay(150);
  }
  strip.clear(); strip.show();

  NetCfg::Value net{};
  NetCfg::load(net);

  PizzaOta::setProgressCallback(nodeOtaProgress);

  auto rr = esp_reset_reason();
  PZ_LOGI("HouseNode boot fw=%s mac=%s reset_reason=%d",
          PizzaIdentity::fw(), PizzaIdentity::macStr().c_str(), (int)rr);

  cfgLoad();

  // Radio next
  if (!PizzaNow::begin(ESPNOW_CHANNEL)) { PZ_LOGE("ESPNOW init failed"); }
  PizzaNow::onReceive(onRx);
  sendHello();
  delay(200);

  // RFID
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  if (!PizzaRfid::begin(RC522_CS, RC522_RST)) { PZ_LOGE("RFID init failed"); }
  delay(10);

  // AUDIO last
  PizzaAudioFS::begin(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  ensureBeepWavs();
  PizzaAudioFS::setVolume(180);
  // run audio loop on the other core so it’s always serviced
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 3, &g_audioTask, 0);

  PZ_LOGI("Node init complete");
}

void loop() {
  PizzaNow::loop();

  if (g_helloDueAt && (int32_t)(millis() - g_helloDueAt) >= 0) {
    sendHello();
    g_helloDueAt = 0;
  }

  if (g_otaPending) {
    // take the job atomically
    noInterrupts();
    bool run = g_otaPending; g_otaPending = false;
    interrupts();

    if (run) {
      // Quiet LEDs during update
      strip.clear(); strip.show();

      auto res = PizzaOta::start(g_otaUrl, g_otaVer, OTA_TOTAL_MS);
      if (res != PizzaOta::OK) {
        OtaResultPayload rr{}; rr.ok = 0; rr.code = (uint8_t)res;
        uint8_t out[64];
        size_t n = PizzaProtocol::pack(OTA_RESULT, (Role)PIZZA_ROLE, g_houseId, g_seq++, &rr, sizeof(rr), out, sizeof(out));
        PizzaNow::sendBroadcast(out, n);
      }
      // success path reboots inside start()
    }
  }

  if (g_assetPending && !g_assetBusy) {
    g_assetBusy = true; // lock

    AssetSyncPayload req;
    noInterrupts();
    memcpy(&req, &g_assetReq, sizeof(req));
    g_assetPending = false;
    interrupts();

    // stop audio before pivot to avoid I²S contention
    PizzaAudioFS::stop();

    doAssetSync(&req);

    g_assetBusy = false; // unlock
  }

  /*** Delivery FSM tick ***/
  static uint32_t nextPoll = 0;
  if ((int32_t)(millis() - nextPoll) >= 0) {
    nextPoll = millis() + 50; // ~20 Hz check; readUid() already gates on field presence

    switch (g_state) {
      case HS_IDLE: {
        // Card present? Read UID once and send exactly one scan.
        uint8_t uid[10]; uint8_t uidLen = 0;
        if (PizzaRfid::readUid(uid, uidLen)) {
          memcpy(g_lastUid, uid, uidLen); g_lastLen = uidLen;
          sendDeliverScan(uid, uidLen);                       // existing helper
          g_haveResult   = false;
          g_state        = HS_WAIT_RESULT;
          g_stateDeadline= millis() + RESULT_TIMEOUT_MS;
        }
      } break;

      case HS_WAIT_RESULT: {
        if (g_haveResult) {
          // LEDs/audio were kicked in onRx; now enforce removal before next try
          g_absentSince = 0;
          g_state = HS_WAIT_REMOVAL;
        } else if ((int32_t)(millis() - g_stateDeadline) >= 0) {
          // Timeout behaves like a fail → require removal
          g_lastOk = false; g_lastReason = 0xFE; // pseudo "timeout"
          g_fx = EFFECT_ERR_PULSE; g_fxUntil = millis() + 600;
          g_absentSince = 0;
          g_state = HS_WAIT_REMOVAL;
        }
      } break;

      case HS_WAIT_REMOVAL: {
        // Stay here until no card for a short, stable window
        uint8_t uid[10]; uint8_t uidLen = 0;
        if (!PizzaRfid::readUid(uid, uidLen)) {
          if (g_absentSince == 0) g_absentSince = millis();
          if ((int32_t)(millis() - g_absentSince) >= (int32_t)REMOVAL_STABLE_MS) {
            g_state = HS_IDLE; // re-arm
          }
        } else {
          g_absentSince = 0; // still present
        }
      } break;
    }
  }

  // LED effects (no heartbeat; idle = off)
  uint32_t now = millis();

  switch (g_fx) {
    case EFFECT_OK_PULSE:
      if (now <= g_fxUntil) {
        for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(0,64,0));
        strip.show();
      } else { strip.clear(); strip.show(); g_fx = EFFECT_NONE; }
      break;

    case EFFECT_ERR_PULSE:
      if (now <= g_fxUntil) {
        for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(64,0,0));
        strip.show();
      } else { strip.clear(); strip.show(); g_fx = EFFECT_NONE; }
      break;

    case EFFECT_YELLOW_PING:
      if (now <= g_fxUntil) {
        for (uint16_t i=0;i<LED_COUNT;i++) strip.setPixelColor(i, strip.Color(80,80,0));
        strip.show();
      } else { strip.clear(); strip.show(); g_fx = EFFECT_NONE; }
      break;

    case EFFECT_NONE:
    default:
      // stay off
      break;
  }

  if (g_winFx == WIN_FX_RAINBOW) {
    if ((int32_t)(now - g_nextFxAt) >= 0) {
      static uint8_t hue = 0; hue += (g_winSpd? g_winSpd : 4);
      for (uint16_t i=0;i<LED_COUNT;i++){
        uint8_t h = hue + (i*3);
        strip.setPixelColor(i, hsv2rgb(h, g_winS?g_winS:255, g_winV?g_winV:120));
      }
      strip.show(); PizzaAudioFS::loop();
      g_nextFxAt = now + 30;
    }
  } else if (g_winFx == WIN_FX_PARTY) {
    if ((int32_t)(now - g_nextFxAt) >= 0) {
      for (uint16_t i=0;i<LED_COUNT;i++){
        uint8_t h = (uint8_t)esp_random();
        strip.setPixelColor(i, hsv2rgb(h, 200, g_winV?g_winV:120));
      }
      strip.show(); PizzaAudioFS::loop();
      uint16_t pace = 40 + (255 - g_winSpd); // faster with higher speed
      g_nextFxAt = now + pace;
    }
  }

  delay(1); // cooperative yield
}
