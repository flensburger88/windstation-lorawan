#include "wind.h"
#include "config.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Anemometer: Interrupt zählt Reed-Schließungen
// ---------------------------------------------------------------------------
static volatile uint32_t s_pulses = 0;
static volatile uint32_t s_lastCountUs = 0;

static void IRAM_ATTR onAnemo() {
  uint32_t now = micros();
  if (now - s_lastCountUs >= ANEMO_DEBOUNCE_US) {
    s_pulses++;
    s_lastCountUs = now;
  }
}

// ---------------------------------------------------------------------------
// Windfahne: Standard-Widerstandsnetzwerk (Misol/SparkFun/Argent, 16 Stellungen)
// Stimmt die Dachboden-Fahne nicht damit überein: im Sensortest die mV je
// Stellung notieren und hier die Widerstände (oder direkt mV) eintragen.
// ---------------------------------------------------------------------------
struct VanePos { float deg; float ohm; };
static const VanePos VANE_TABLE[16] = {
  {  0.0, 33000}, { 22.5,  6570}, { 45.0,  8200}, { 67.5,   891},
  { 90.0,  1000}, {112.5,   688}, {135.0,  2200}, {157.5,  1410},
  {180.0,  3900}, {202.5,  3140}, {225.0, 16000}, {247.5, 14120},
  {270.0,120000}, {292.5, 42120}, {315.0, 64900}, {337.5, 21880},
};
static float s_vaneExpectedMv[16];

static uint16_t readVaneMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VANE);
  return sum / 16;
}

static void decodeVane(uint16_t mv, LiveValues& lv) {
  int best = 0;
  float bestErr = 1e9;
  for (int i = 0; i < 16; i++) {
    float e = fabsf(mv - s_vaneExpectedMv[i]);
    if (e < bestErr) { bestErr = e; best = i; }
  }
  lv.vaneIndex = best;
  lv.vaneErrMv = (int16_t)lroundf(bestErr);
  if (bestErr > VANE_MAX_ERR_MV) { lv.dirDeg = -1; return; }
  int deg = (int)lroundf(VANE_TABLE[best].deg) + VANE_OFFSET_DEG;
  lv.dirDeg = ((deg % 360) + 360) % 360;
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------
static LiveValues s_live;
static uint32_t s_lastTickMs = 0, s_lastPulses = 0;

// letzte drei 1-s-Werte für die Böe (WMO: 3-s-Mittel)
static uint32_t s_ringP[3], s_ringMs[3];
static uint8_t  s_ringN = 0, s_ringPos = 0;

// laufende Minute
static uint32_t s_minPulses = 0, s_minMs = 0;
static float    s_minGust = 0, s_sumX = 0, s_sumY = 0;
static uint16_t s_dirSamples = 0;
static bool     s_minuteReady = false;
static MinuteRecord s_minuteRec;

void windBegin() {
  for (int i = 0; i < 16; i++) {
    float r = VANE_TABLE[i].ohm;
    s_vaneExpectedMv[i] = VANE_VCC_MV * r / (r + VANE_PULLUP_OHM);
  }
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VANE, ADC_11db);
  pinMode(PIN_ANEMO, INPUT_PULLUP);  // zusätzlich externer 10k
  attachInterrupt(digitalPinToInterrupt(PIN_ANEMO), onAnemo, FALLING);
  s_lastTickMs = millis();
}

static void closeMinute() {
  float avg = s_minMs ? s_minPulses * 1000.0f / s_minMs * ANEMO_MS_PER_HZ : 0;
  float gust = s_ringN >= 3 ? s_minGust : avg;
  if (gust < avg) gust = avg;
  s_minuteRec.avgDms  = (uint16_t)lroundf(avg * 10);
  s_minuteRec.gustDms = (uint16_t)lroundf(gust * 10);
  if (s_dirSamples == 0) {
    s_minuteRec.dir = 255;
  } else {
    float deg = atan2f(s_sumX, s_sumY) * 180.0f / PI;
    if (deg < 0) deg += 360;
    s_minuteRec.dir = (uint8_t)(lroundf(deg / 1.5f) % 240);
  }
  s_minuteReady = true;
  s_minPulses = s_minMs = 0;
  s_minGust = s_sumX = s_sumY = 0;
  s_dirSamples = 0;
}

bool windTick() {
  uint32_t now = millis();
  uint32_t dt = now - s_lastTickMs;
  if (dt < 1000) return false;
  s_lastTickMs = now;

  noInterrupts();
  uint32_t p = s_pulses;
  interrupts();
  uint32_t dp = p - s_lastPulses;
  s_lastPulses = p;

  s_live.totalPulses = p;
  s_live.pulses1s = dp;
  s_live.hz = dp * 1000.0f / dt;
  s_live.speed = s_live.hz * ANEMO_MS_PER_HZ;

  // Böe über die letzten 3 Werte
  s_ringP[s_ringPos] = dp;
  s_ringMs[s_ringPos] = dt;
  s_ringPos = (s_ringPos + 1) % 3;
  if (s_ringN < 3) s_ringN++;
  if (s_ringN == 3) {
    uint32_t sp = s_ringP[0] + s_ringP[1] + s_ringP[2];
    uint32_t sm = s_ringMs[0] + s_ringMs[1] + s_ringMs[2];
    float g = sp * 1000.0f / sm * ANEMO_MS_PER_HZ;
    if (g > s_minGust) s_minGust = g;
  }
  s_live.gustMinute = s_minGust;

  // Richtung: Vektormittel, gewichtet mit der Geschwindigkeit
  s_live.vaneMv = readVaneMv();
  decodeVane(s_live.vaneMv, s_live);
  if (s_live.dirDeg >= 0) {
    float w = s_live.speed > 0.05f ? s_live.speed : 0.05f;  // bei Flaute trotzdem Richtung
    float rad = s_live.dirDeg * PI / 180.0f;
    s_sumX += w * sinf(rad);
    s_sumY += w * cosf(rad);
    s_dirSamples++;
  }

  s_minPulses += dp;
  s_minMs += dt;
  if (s_minMs >= 60000) closeMinute();
  return true;
}

bool windMinuteReady(MinuteRecord& out) {
  if (!s_minuteReady) return false;
  out = s_minuteRec;
  s_minuteReady = false;
  return true;
}

const LiveValues& windLive() { return s_live; }

const char* compassName(int deg) {
  static const char* names[16] = {"N","NNO","NO","ONO","O","OSO","SO","SSO",
                                  "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  if (deg < 0) return "?";
  return names[((deg * 10 + 112) / 225) % 16];
}
