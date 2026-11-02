// LILKA MOD RF bring-up test (nRF24L01+PA+LNA).
//
// Lilka v2 extension header -> module J2:
//   1 GND | 2 3V3 | 3 RX(44)=SCK | 4 TX(43)=MISO | 5 GPIO48=MOSI | 6 GPIO47=CSN
//   7 GPIO21=CE | 8 GPIO14=IRQ# | 9 GPIO13=GND | 10 GPIO12=3V3 | 11 3V3 | 12 GND
// GPIO13/GPIO12 are wired straight to GND/3V3 on the board: never drive them.
//
// RF test: the ESP32-S3 itself floods 802.11 frames on a known Wi-Fi channel and the
// nRF24 must see the carrier (RPD) appear on exactly those nRF channels.
//
// Serial commands: 't' rerun self-test, 'k' boot back to app0 (Keira).

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>

static const int PIN_MOSI = 48;
static const int PIN_CSN = 47;
static const int PIN_CE = 21;
static const int PIN_IRQ = 14;
static const int IDLE_PINS[] = {47, 21, 14, 13, 12}; // header pins 6..10 (pull-ups, GND/3V3 wiring), MSB first
static const uint8_t EXPECTED_IDLE = 0x1D;
static int pinSck = 44;
static int pinMiso = 43;

static const int WIFI_CH = 13;                        // 2472 MHz
static const int NRF_CH_CENTER = 72;                  // 2400 + 72
static const int NRF_CHANNELS = 126;

static SPIClass rfSpi(HSPI);
static const SPISettings spiSettings(4000000, MSBFIRST, SPI_MODE0);
static int fails = 0;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        bool ok_ = (cond);                                                                                   \
        Serial.printf("[%s] ", ok_ ? " OK " : "FAIL");                                                       \
        Serial.printf(__VA_ARGS__);                                                                          \
        Serial.println();                                                                                    \
        if (!ok_) fails++;                                                                                   \
    } while (0)

enum : uint8_t {
    REG_CONFIG = 0x00,
    REG_EN_AA = 0x01,
    REG_EN_RXADDR = 0x02,
    REG_SETUP_AW = 0x03,
    REG_SETUP_RETR = 0x04,
    REG_RF_CH = 0x05,
    REG_RF_SETUP = 0x06,
    REG_STATUS = 0x07,
    REG_RPD = 0x09,
    REG_TX_ADDR = 0x10,
    REG_FIFO_STATUS = 0x17,
    REG_DYNPD = 0x1C,
    REG_FEATURE = 0x1D,
};

static uint8_t xfer(uint8_t cmd, const uint8_t* tx, uint8_t* rx, size_t n) {
    rfSpi.beginTransaction(spiSettings);
    digitalWrite(PIN_CSN, LOW);
    uint8_t status = rfSpi.transfer(cmd);
    for (size_t i = 0; i < n; i++) {
        uint8_t r = rfSpi.transfer(tx ? tx[i] : 0xFF);
        if (rx) rx[i] = r;
    }
    digitalWrite(PIN_CSN, HIGH);
    rfSpi.endTransaction();
    return status;
}

static uint8_t readReg(uint8_t reg) {
    uint8_t v = 0;
    xfer(reg & 0x1F, nullptr, &v, 1);
    return v;
}

static void writeReg(uint8_t reg, uint8_t v) {
    xfer(0x20 | (reg & 0x1F), &v, nullptr, 1);
}

static uint8_t status() {
    return xfer(0xFF, nullptr, nullptr, 0);
}

static void spiBegin() {
    rfSpi.end();
    rfSpi.begin(pinSck, pinMiso, PIN_MOSI, -1);
}

static uint8_t readIdleLevels() {
    uint8_t levels = 0;
    for (int p : IDLE_PINS) {
        pinMode(p, INPUT);
        levels = (levels << 1) | digitalRead(p);
    }
    return levels;
}

static bool detectPins() {
    const int orders[2][2] = {{44, 43}, {43, 44}};
    for (auto& o : orders) {
        pinSck = o[0];
        pinMiso = o[1];
        spiBegin();
        uint8_t aw = readReg(REG_SETUP_AW), st = status();
        Serial.printf("       SCK=%d MISO=%d -> SETUP_AW=0x%02X STATUS=0x%02X\n", pinSck, pinMiso, aw, st);
        if (aw >= 1 && aw <= 3 && (st & 0x80) == 0 && st != 0x00) return true;
    }
    return false;
}

// --- Wi-Fi frame flood used as a known 2.4 GHz signal source -------------------------------------
static volatile bool flooding = false;

static void floodTask(void*) {
    // Minimal beacon frame (broadcast), SSID "LILKA-RF-TEST".
    static const uint8_t beacon[] = {
        0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55,
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x01, 0x04,
        0x00, 0x0D, 'L', 'I', 'L', 'K', 'A', '-', 'R', 'F', '-', 'T', 'E', 'S', 'T', 0x01, 0x08, 0x82,
        0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C, 0x03, 0x01, WIFI_CH,
    };
    for (;;) {
        if (flooding) {
            esp_wifi_80211_tx(WIFI_IF_AP, beacon, sizeof(beacon), false);
            vTaskDelay(0);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void wifiStart() {
    static bool started = false;
    if (started) return;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("LILKA-RF-TEST", nullptr, WIFI_CH, 1);
    esp_wifi_set_max_tx_power(78);
    xTaskCreatePinnedToCore(floodTask, "flood", 4096, nullptr, 1, nullptr, 0);
    started = true;
}

// Sweep all channels in RX mode, counting RPD (carrier > -64 dBm at the chip) hits.
static void scan(uint16_t* hits, int passes) {
    memset(hits, 0, sizeof(uint16_t) * NRF_CHANNELS);
    writeReg(REG_CONFIG, 0x0B); // PWR_UP | PRIM_RX | EN_CRC
    delay(5);
    for (int p = 0; p < passes; p++) {
        for (int ch = 0; ch < NRF_CHANNELS; ch++) {
            digitalWrite(PIN_CE, LOW);
            writeReg(REG_RF_CH, ch);
            digitalWrite(PIN_CE, HIGH);
            delayMicroseconds(260);
            if (readReg(REG_RPD) & 1) hits[ch]++;
        }
    }
    digitalWrite(PIN_CE, LOW);
}

static uint32_t bandSum(const uint16_t* hits, int lo, int hi) {
    uint32_t s = 0;
    for (int ch = max(lo, 0); ch <= min(hi, NRF_CHANNELS - 1); ch++) s += hits[ch];
    return s;
}

static void printSpectrum(const char* title, const uint16_t* hits, int passes) {
    static const char shades[] = " .:-=+*#%@";
    Serial.printf("       %-9s|", title);
    for (int ch = 0; ch < NRF_CHANNELS; ch++) {
        int lvl = hits[ch] == 0 ? 0 : 1 + (hits[ch] * 8) / passes;
        Serial.print(shades[min(lvl, 9)]);
    }
    Serial.println("|");
}

static void selfTest() {
    fails = 0;
    Serial.println("\n===== LILKA MOD RF (nRF24L01+PA+LNA) self-test =====");

    uint8_t idle = readIdleLevels();
    CHECK(idle == EXPECTED_IDLE, "Idle levels, header pins 6-10 = 0x%02X, expect 0x%02X (pull-ups + GND/3V3 wiring)", idle, EXPECTED_IDLE);

    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    pinMode(PIN_CE, OUTPUT);
    digitalWrite(PIN_CE, LOW);
    pinMode(PIN_IRQ, INPUT);
    delay(110); // power-on reset

    CHECK(detectPins(), "SPI link (SCK=GPIO%d, MISO=GPIO%d)", pinSck, pinMiso);
    if (fails > (idle != EXPECTED_IDLE)) {
        Serial.println("No SPI response - check module seating / 3V3 / CSN.");
        return;
    }

    // Write/read-back: 5-byte address and a few config registers.
    const uint8_t addr[5] = {0xA5, 0x5A, 0xC3, 0x3C, 0x96};
    uint8_t back[5] = {};
    xfer(0x20 | REG_TX_ADDR, addr, nullptr, 5);
    xfer(REG_TX_ADDR, nullptr, back, 5);
    CHECK(memcmp(addr, back, 5) == 0, "TX_ADDR write/read-back %02X%02X%02X%02X%02X", back[0], back[1], back[2],
          back[3], back[4]);

    writeReg(REG_RF_SETUP, 0x26); // 250 kbps, 0 dBm: only nRF24L01+ supports 250 kbps
    uint8_t rfs = readReg(REG_RF_SETUP);
    CHECK((rfs & 0x2F) == 0x26, "RF_SETUP=0x%02X (250 kbps accepted -> genuine L01+ / compatible)", rfs);

    writeReg(REG_FEATURE, 0x04);
    uint8_t feat = readReg(REG_FEATURE);
    CHECK(feat == 0x04, "FEATURE register writable (0x%02X)", feat);
    writeReg(REG_FEATURE, 0x00);

    // TX path + IRQ line: send one packet without auto-ack, expect TX_DS and IRQ# low.
    writeReg(REG_EN_AA, 0x00);
    writeReg(REG_SETUP_RETR, 0x00);
    writeReg(REG_RF_CH, 100);
    writeReg(REG_STATUS, 0x70);
    xfer(0xE1, nullptr, nullptr, 0); // FLUSH_TX
    xfer(0xE2, nullptr, nullptr, 0); // FLUSH_RX
    writeReg(REG_CONFIG, 0x0A);      // PWR_UP | EN_CRC, PRIM_TX
    delay(5);
    bool irqIdle = digitalRead(PIN_IRQ);
    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = i;
    xfer(0xA0, payload, nullptr, 32); // W_TX_PAYLOAD
    uint32_t t0 = micros();
    digitalWrite(PIN_CE, HIGH);
    delayMicroseconds(15);
    digitalWrite(PIN_CE, LOW);
    while (digitalRead(PIN_IRQ) && micros() - t0 < 10000) {
    }
    uint32_t txUs = micros() - t0;
    uint8_t st = status();
    bool irqActive = !digitalRead(PIN_IRQ);
    CHECK((st & 0x20) && irqIdle && irqActive, "TX 32 B @2500 MHz: TX_DS=%d, IRQ# idle=%d active-low=%d, %lu us",
          (st >> 5) & 1, irqIdle, irqActive, (unsigned long)txUs);
    writeReg(REG_STATUS, 0x70);
    CHECK(digitalRead(PIN_IRQ), "IRQ# released after clearing STATUS");

    // RX path: RPD spectrum with and without a known carrier from the ESP32-S3 Wi-Fi radio.
    writeReg(REG_RF_SETUP, 0x07); // 1 Mbps, max LNA gain
    const int passes = 40;
    uint16_t quiet[NRF_CHANNELS], loud[NRF_CHANNELS];
    wifiStart();
    flooding = false;
    delay(100);
    scan(quiet, passes);
    flooding = true;
    delay(50);
    scan(loud, passes);
    flooding = false;

    Serial.println("       nRF ch   |0         1         2         3         4         5         6         7         "
                   "8         9         10        11        12    |");
    printSpectrum("baseline", quiet, passes);
    printSpectrum("WiFi ch13", loud, passes);

    const int lo = NRF_CH_CENTER - 8, hi = NRF_CH_CENTER + 8;
    uint32_t inQ = bandSum(quiet, lo, hi), inL = bandSum(loud, lo, hi);
    uint32_t outQ = bandSum(quiet, 0, lo - 12) + bandSum(quiet, hi + 12, NRF_CHANNELS - 1);
    uint32_t outL = bandSum(loud, 0, lo - 12) + bandSum(loud, hi + 12, NRF_CHANNELS - 1);
    int peak = 0;
    for (int ch = 0; ch < NRF_CHANNELS; ch++)
        if (loud[ch] - min(loud[ch], quiet[ch]) > loud[peak] - min(loud[peak], quiet[peak])) peak = ch;
    Serial.printf("       hits in nRF ch %d-%d: baseline %lu -> with carrier %lu; elsewhere %lu -> %lu; peak delta ch %d\n",
                  lo, hi, (unsigned long)inQ, (unsigned long)inL, (unsigned long)outQ, (unsigned long)outL, peak);
    CHECK(inL > inQ + passes && peak >= lo - 4 && peak <= hi + 4,
          "RX/LNA: carrier on Wi-Fi ch %d (2472 MHz) detected at nRF ch %d", WIFI_CH, peak);

    writeReg(REG_CONFIG, 0x08); // power down
    Serial.printf("===== RESULT: %s (%d failure%s) =====\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    Serial.println("Repeats every 30 s. 't'=retest, 'k'=back to Keira.");
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) delay(10);
    delay(500);
    selfTest();
}

void loop() {
    static uint32_t last = millis();
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 't') {
            selfTest();
            last = millis();
        }
        if (c == 'k') {
            const esp_partition_t* p =
                esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
            Serial.printf("Switching boot to %s\n", p ? p->label : "?");
            if (p) esp_ota_set_boot_partition(p);
            delay(200);
            esp_restart();
        }
    }
    if (millis() - last > 30000) {
        selfTest();
        last = millis();
    }
    delay(5);
}
