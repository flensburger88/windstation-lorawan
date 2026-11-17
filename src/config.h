#pragma once
// ---------------------------------------------------------------------------
// Heltec WiFi LoRa 32 V3 – feste Pins der Platine
// ---------------------------------------------------------------------------
#define PIN_PRG        0    // PRG-Taster (LOW = gedrückt)
#define PIN_LED        35   // weiße LED
#define PIN_VEXT       36   // schaltet Vext (OLED); LOW = an
#define PIN_OLED_SDA   17
#define PIN_OLED_SCL   18
#define PIN_OLED_RST   21
#define PIN_VBAT       1    // Akkuspannung über Teiler 390k/100k
#define PIN_VBAT_CTRL  37   // gibt den Teiler frei
#define VBAT_CTRL_ON   LOW  // V3.0/3.1: LOW, V3.2: HIGH (bei 0 V umdrehen)
#define VBAT_FACTOR    4.9f

// SX1262
#define LORA_SCK   9
#define LORA_MISO  11
#define LORA_MOSI  10
#define LORA_NSS   8
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_DIO1  14

// ---------------------------------------------------------------------------
// Windsensor (an Header J3; beides RTC-GPIOs -> später ULP-Zähler möglich)
// ---------------------------------------------------------------------------
#define PIN_ANEMO  6    // Reed-Kontakt gegen GND, 10k Pull-up auf 3V3
#define PIN_VANE   7    // ADC1_CH6, Windfahne gegen GND, 10k Pull-up auf 3V3

// Anemometer: m/s pro Hz Schaltfrequenz.
// Misol/SparkFun/Ventus-Ersatzteile: 2,4 km/h pro Hz = 0,6667 m/s.
// Davis 6410: 1,006 m/s pro Hz. Mit dem Datenblatt der Dachboden-Station prüfen!
#define ANEMO_MS_PER_HZ    0.6667f
#define ANEMO_DEBOUNCE_US  3000      // Reed prellt; 3 ms erlauben > 300 Hz

// Windfahne: Pull-up-Widerstand und Versorgung des Spannungsteilers
#define VANE_PULLUP_OHM    10000.0f
#define VANE_VCC_MV        3300.0f
#define VANE_MAX_ERR_MV    200       // weiter weg vom Tabellenwert = ungültig
// Nordabgleich: Grad, die auf die Rohrichtung addiert werden (Montage am Mast)
#define VANE_OFFSET_DEG    0

// ---------------------------------------------------------------------------
// LoRaWAN / Sende-Logik (Prototyp: festes Intervall, Tag/Nacht kommt später)
// ---------------------------------------------------------------------------
#define LORAWAN_FPORT        1
#define SEND_INTERVAL_MIN    5    // TTN Fair Use: <= 30 s Airtime pro Tag
#define MANUAL_MIN_SPACING_S 30   // PRG-Taste: frühestens alle 30 s
#define MAX_QUEUED_MINUTES   15
