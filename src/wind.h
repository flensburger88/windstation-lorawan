#pragma once
#include <Arduino.h>

// Ein Minutenwert, so wie er in den Payload geht
struct MinuteRecord {
  uint16_t avgDms;   // Mittelwert in 0,1 m/s
  uint16_t gustDms;  // Böe (max. 3-s-Mittel) in 0,1 m/s
  uint8_t  dir;      // Richtung in 1,5°-Schritten (0..239), 255 = ungültig
};

// Live-Werte der letzten Sekunde (Anzeige + Kalibrierung)
struct LiveValues {
  uint32_t totalPulses;
  uint32_t pulses1s;
  float    hz;
  float    speed;      // m/s, 1-s-Wert
  float    gustMinute; // m/s, bisherige Böe der laufenden Minute
  uint16_t vaneMv;
  int16_t  dirDeg;     // -1 = ungültig
  int16_t  vaneErrMv;  // Abstand zum nächsten Tabellenwert
  uint8_t  vaneIndex;  // 0..15
};

void windBegin();
bool windTick();                         // true, wenn neuer 1-s-Wert vorliegt
bool windMinuteReady(MinuteRecord& out); // true, wenn eine Minute fertig ist
const LiveValues& windLive();
const char* compassName(int deg);
