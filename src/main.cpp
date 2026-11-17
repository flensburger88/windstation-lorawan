// Windstation Kollund – Prototyp
// Heltec WiFi LoRa 32 V3: Windsensor auslesen + LoRaWAN-Uplinks über TTN
//
// Seriell (115200) kommt jede Sekunde eine Zeile mit Rohwerten (Kalibrierung),
// jede Minute ein Minutenwert. PRG-Taste = sofort senden + LinkCheck.
#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include "config.h"
#include "wind.h"

#ifndef NO_LORA
#include <RadioLib.h>
#include <Preferences.h>
#include "secrets.h"
#endif

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);

// Warteschlange der Minutenwerte, älteste zuerst
static MinuteRecord queue[MAX_QUEUED_MINUTES];
static uint8_t queueLen = 0;

static char loraLine[24] = "LoRa: aus";
static char linkLine[24] = "";
static uint16_t vbatMv = 0;

static uint16_t readVbatMv() {
  pinMode(PIN_VBAT_CTRL, OUTPUT);
  digitalWrite(PIN_VBAT_CTRL, VBAT_CTRL_ON);
  delay(5);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VBAT);
  digitalWrite(PIN_VBAT_CTRL, !VBAT_CTRL_ON);
  return (uint16_t)(sum / 8 * VBAT_FACTOR);
}

static void pushMinute(const MinuteRecord& r) {
  if (queueLen == MAX_QUEUED_MINUTES) {           // voll: ältesten verwerfen
    memmove(queue, queue + 1, sizeof(MinuteRecord) * (MAX_QUEUED_MINUTES - 1));
    queueLen--;
  }
  queue[queueLen++] = r;
}

// ---------------------------------------------------------------------------
// LoRaWAN
// ---------------------------------------------------------------------------
#ifndef NO_LORA
static SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
static LoRaWANNode node(&radio, &EU868);
static Preferences store;

static uint8_t appKey[] = LW_APP_KEY;
static uint8_t nwkKey[] = LW_NWK_KEY;

static bool joined = false;
static uint32_t nextJoinMs = 0, joinBackoffMs = 60000;
static uint32_t lastSendMs = 0;
static uint32_t uplinks = 0;
static bool firstUplink = true;

static void restoreNonces() {
  store.begin("lorawan", true);
  if (store.isKey("nonces")) {
    uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    store.getBytes("nonces", buf, sizeof(buf));
    node.setBufferNonces(buf);
  }
  store.end();
}

static void saveNonces() {
  // DevNonce muss über Neustarts steigen, sonst lehnt TTN den Join ab
  store.begin("lorawan", false);
  store.putBytes("nonces", node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  store.end();
}

static bool loraBegin() {
  if (LW_DEV_EUI == 0) {
    Serial.println(F("[LoRa] secrets.h nicht ausgefüllt -> kein Join"));
    snprintf(loraLine, sizeof(loraLine), "LoRa: Keys fehlen");
    return false;
  }
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int16_t st = radio.begin();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] SX1262 Init-Fehler %d\n", st);
    snprintf(loraLine, sizeof(loraLine), "SX1262 Fehler %d", st);
    return false;
  }
  Serial.println(F("[LoRa] SX1262 ok"));
  return true;
}

static void tryJoin() {
  snprintf(loraLine, sizeof(loraLine), "LoRa: Join ...");
  Serial.println(F("[LoRa] OTAA-Join ..."));
  node.beginOTAA(LW_JOIN_EUI, LW_DEV_EUI, nwkKey, appKey);
  restoreNonces();
  int16_t st = node.activateOTAA();
  saveNonces();
  if (st == RADIOLIB_LORAWAN_NEW_SESSION) {
    joined = true;
    joinBackoffMs = 60000;
    lastSendMs = millis() - SEND_INTERVAL_MIN * 60000UL + 15000;  // 1. Uplink nach 15 s
    Serial.println(F("[LoRa] Join ok"));
    snprintf(loraLine, sizeof(loraLine), "LoRa: verbunden");
  } else {
    Serial.printf("[LoRa] Join fehlgeschlagen (%d), neuer Versuch in %lu s\n", st, joinBackoffMs / 1000);
    snprintf(loraLine, sizeof(loraLine), "Join fehlg. %d", st);
    nextJoinMs = millis() + joinBackoffMs;
    if (joinBackoffMs < 30UL * 60000) joinBackoffMs *= 2;
  }
}

// Payload v1:
//  [0] Version=1  [1..2] Akku mV  [3..4] Windfahne roh mV  [5] Anzahl n
//  je Minute 5 Byte: Mittel 0,1 m/s (u16), Böe 0,1 m/s (u16), Richtung 1,5° (u8, 255=?)
static void sendUplink(bool withMacRequests) {
  uint8_t buf[6 + 5 * MAX_QUEUED_MINUTES];
  size_t maxLen = node.getMaxPayloadLen();
  uint8_t n = queueLen;
  while (n > 0 && 6 + 5 * n > maxLen) n--;

  vbatMv = readVbatMv();
  uint16_t vane = windLive().vaneMv;
  buf[0] = 1;
  buf[1] = vbatMv >> 8; buf[2] = vbatMv & 0xFF;
  buf[3] = vane >> 8;   buf[4] = vane & 0xFF;
  buf[5] = n;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t* o = buf + 6 + 5 * i;
    o[0] = queue[i].avgDms >> 8;  o[1] = queue[i].avgDms & 0xFF;
    o[2] = queue[i].gustDms >> 8; o[3] = queue[i].gustDms & 0xFF;
    o[4] = queue[i].dir;
  }
  size_t len = 6 + 5 * n;

  if (withMacRequests) {  // erzwingt einen Downlink -> sparsam verwenden (TTN: 10/Tag)
    node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);
    node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_DEVICE_TIME);
  }

  digitalWrite(PIN_LED, HIGH);
  uint8_t down[64];
  size_t downLen = sizeof(down);
  LoRaWANEvent_t evUp, evDown;
  int16_t st = node.sendReceive(buf, len, LORAWAN_FPORT, down, &downLen, false, &evUp, &evDown);
  digitalWrite(PIN_LED, LOW);
  lastSendMs = millis();

  if (st < RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] Uplink-Fehler %d\n", st);
    snprintf(loraLine, sizeof(loraLine), "Uplink Fehler %d", st);
    return;
  }
  uplinks++;
  // gesendete Minuten aus der Warteschlange nehmen
  memmove(queue, queue + n, sizeof(MinuteRecord) * (queueLen - n));
  queueLen -= n;

  Serial.printf("[LoRa] Uplink #%lu FCnt=%lu %u Byte, %u Minuten, DR%u, %.1f MHz, %d dBm, Airtime %lu ms\n",
                uplinks, (unsigned long)evUp.fCnt, (unsigned)len, n, evUp.datarate, evUp.freq,
                evUp.power, (unsigned long)node.getLastToA());
  snprintf(loraLine, sizeof(loraLine), "Up#%lu DR%u %u Min", uplinks, evUp.datarate, n);

  if (st > 0) {
    Serial.printf("[LoRa] Downlink in RX%d, RSSI %.0f dBm, SNR %.1f dB, %u Byte\n",
                  st, radio.getRSSI(), radio.getSNR(), (unsigned)downLen);
    snprintf(linkLine, sizeof(linkLine), "DL %.0fdBm %.0fdB", radio.getRSSI(), radio.getSNR());
  }
  if (withMacRequests) {
    uint8_t margin = 0, gwCnt = 0;
    if (node.getMacLinkCheckAns(&margin, &gwCnt) == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa] LinkCheck: Reserve %u dB, %u Gateway(s)\n", margin, gwCnt);
      snprintf(linkLine, sizeof(linkLine), "Link %udB %uGW", margin, gwCnt);
    } else {
      Serial.println(F("[LoRa] keine LinkCheck-Antwort"));
    }
    uint32_t unixTime = 0; uint8_t frac = 0;
    if (node.getMacDeviceTimeAns(&unixTime, &frac, true) == RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa] Netzwerkzeit (Unix UTC): %lu\n", (unsigned long)unixTime);
    }
  }
}
#endif

// ---------------------------------------------------------------------------
// Anzeige
// ---------------------------------------------------------------------------
static void drawDisplay() {
  const LiveValues& lv = windLive();
  char l[32];
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 9, loraLine);
  snprintf(l, sizeof(l), "Wind %4.1f  Boe %4.1f", lv.speed, lv.gustMinute);
  display.drawStr(0, 20, l);
  if (lv.dirDeg >= 0) snprintf(l, sizeof(l), "Ri %3d %-3s  %4umV", lv.dirDeg, compassName(lv.dirDeg), lv.vaneMv);
  else                snprintf(l, sizeof(l), "Ri  ??     %4umV", lv.vaneMv);
  display.drawStr(0, 31, l);
  snprintf(l, sizeof(l), "Puls %lu  %4.1f Hz", (unsigned long)lv.totalPulses, lv.hz);
  display.drawStr(0, 42, l);
  snprintf(l, sizeof(l), "Queue %u  Akku %.2fV", queueLen, vbatMv / 1000.0f);
  display.drawStr(0, 53, l);
  display.drawStr(0, 64, linkLine);
  display.sendBuffer();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== Windstation Kollund – Prototyp ==="));

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_PRG, INPUT_PULLUP);
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);  // OLED an
  delay(50);
  display.begin();

  windBegin();
  vbatMv = readVbatMv();

#ifdef NO_LORA
  snprintf(loraLine, sizeof(loraLine), "Sensortest (Funk aus)");
#else
  if (!loraBegin()) nextJoinMs = UINT32_MAX;
#endif
  drawDisplay();
  Serial.println(F("Spalten: Pulse/s | Hz | m/s | Boe Min | Fahne mV | Index | Abw. mV | Richtung"));
}

void loop() {
  if (windTick()) {
    const LiveValues& lv = windLive();
    Serial.printf("%3lu | %5.2f | %5.2f | %5.2f | %4u mV | %2u | %4d | %s %d\n",
                  (unsigned long)lv.pulses1s, lv.hz, lv.speed, lv.gustMinute, lv.vaneMv,
                  lv.vaneIndex, lv.vaneErrMv, compassName(lv.dirDeg), lv.dirDeg);
    drawDisplay();
  }

  MinuteRecord rec;
  if (windMinuteReady(rec)) {
    pushMinute(rec);
    Serial.printf("--- Minute: Mittel %.1f m/s, Boe %.1f m/s, Richtung %s (Queue %u)\n",
                  rec.avgDms / 10.0f, rec.gustDms / 10.0f,
                  rec.dir == 255 ? "?" : String(rec.dir * 1.5f, 1).c_str(), queueLen);
  }

  // PRG-Taste (Flanke)
  static bool lastBtn = HIGH;
  static uint32_t btnMs = 0;
  bool btn = digitalRead(PIN_PRG);
  bool pressed = (lastBtn == HIGH && btn == LOW && millis() - btnMs > 300);
  if (pressed) btnMs = millis();
  lastBtn = btn;

#ifndef NO_LORA
  if (!joined && nextJoinMs != UINT32_MAX && (int32_t)(millis() - nextJoinMs) >= 0) {
    tryJoin();
    drawDisplay();
  }
  if (joined) {
    bool due = millis() - lastSendMs >= SEND_INTERVAL_MIN * 60000UL;
    bool manual = pressed && millis() - lastSendMs >= MANUAL_MIN_SPACING_S * 1000UL;
    if (pressed && !manual) Serial.println(F("[LoRa] PRG: zu früh, Mindestabstand 30 s"));
    if (due || manual) {
      sendUplink(manual || firstUplink);
      firstUplink = false;
      drawDisplay();
    }
  }
#else
  (void)pressed;
#endif
}
