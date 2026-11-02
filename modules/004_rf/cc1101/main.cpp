// LILKA MOD RF carrier with a CC1101 module (nRF24-compatible 8-pin layout).
//
// Lilka v2 extension header -> carrier J2 -> CC1101 module:
//   3 RX(44)=SCK | 4 TX(43)=MISO(GDO1) | 5 GPIO48=MOSI | 6 GPIO47=CSN
//   7 GPIO21=GDO0 (nRF CE position) | 8 GPIO14=GDO2 (nRF IRQ position)
//   9 GPIO13=GND | 10 GPIO12=3V3 -- hard-wired straps, never drive them.
//
// Serial commands: 't' rerun self-test, 'k' boot back to app0 (Keira).

#include <Arduino.h>
#include <SPI.h>
#include <esp_ota_ops.h>

static const int PIN_MOSI = 48;
static const int PIN_CSN = 47;
static const int PIN_GDO0 = 21;
static const int PIN_GDO2 = 14;
static int pinSck = 44;
static int pinMiso = 43;

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
    IOCFG2 = 0x00,
    IOCFG0 = 0x02,
    FIFOTHR = 0x03,
    SYNC1 = 0x04,
    SYNC0 = 0x05,
    PKTLEN = 0x06,
    PKTCTRL1 = 0x07,
    PKTCTRL0 = 0x08,
    ADDR = 0x09,
    FSCTRL1 = 0x0B,
    FREQ2 = 0x0D,
    FREQ1 = 0x0E,
    FREQ0 = 0x0F,
    MDMCFG4 = 0x10,
    MDMCFG3 = 0x11,
    MDMCFG2 = 0x12,
    DEVIATN = 0x15,
    MCSM0 = 0x18,
    FOCCFG = 0x19,
    AGCCTRL2 = 0x1B,
    FREND0 = 0x22,
    FSCAL3 = 0x23,
    FSCAL2 = 0x24,
    FSCAL1 = 0x25,
    FSCAL0 = 0x26,
    TEST2 = 0x2C,
    TEST1 = 0x2D,
    TEST0 = 0x2E,
    // status registers (read with burst bit)
    PARTNUM = 0x30,
    VERSION = 0x31,
    RSSI = 0x34,
    MARCSTATE = 0x35,
    TXBYTES = 0x3A,
    PATABLE = 0x3E,
    TXFIFO = 0x3F,
    // strobes
    SRES = 0x30,
    SCAL = 0x33,
    SRX = 0x34,
    STX = 0x35,
    SIDLE = 0x36,
    SFRX = 0x3A,
    SFTX = 0x3B,
};

static bool waitMisoLow(uint32_t us = 5000) {
    uint32_t t0 = micros();
    while (digitalRead(pinMiso)) {
        if (micros() - t0 > us) return false;
    }
    return true;
}

static uint8_t xfer(uint8_t hdr, const uint8_t* tx, uint8_t* rx, size_t n) {
    rfSpi.beginTransaction(spiSettings);
    digitalWrite(PIN_CSN, LOW);
    waitMisoLow();
    uint8_t st = rfSpi.transfer(hdr);
    for (size_t i = 0; i < n; i++) {
        uint8_t r = rfSpi.transfer(tx ? tx[i] : 0);
        if (rx) rx[i] = r;
    }
    digitalWrite(PIN_CSN, HIGH);
    rfSpi.endTransaction();
    return st;
}

static uint8_t strobe(uint8_t s) {
    return xfer(s, nullptr, nullptr, 0);
}

static void writeReg(uint8_t a, uint8_t v) {
    xfer(a, &v, nullptr, 1);
}

static uint8_t readReg(uint8_t a) {
    uint8_t v = 0;
    xfer(a | 0x80, nullptr, &v, 1);
    return v;
}

static uint8_t readStatus(uint8_t a) {
    uint8_t v = 0;
    xfer(a | 0xC0, nullptr, &v, 1);
    return v;
}

static uint8_t marcState() {
    return readStatus(MARCSTATE) & 0x1F;
}

static bool waitMarc(uint8_t state, uint32_t ms) {
    uint32_t t0 = millis();
    while (marcState() != state) {
        if (millis() - t0 > ms) return false;
    }
    return true;
}

static float rssiDbm() {
    int8_t r = (int8_t)readStatus(RSSI);
    return r / 2.0f - 74.0f;
}

static void spiBegin() {
    rfSpi.end();
    rfSpi.begin(pinSck, pinMiso, PIN_MOSI, -1);
}

static void reset() {
    // Manual power-on reset sequence (datasheet 19.1.2).
    digitalWrite(PIN_CSN, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_CSN, LOW);
    delayMicroseconds(10);
    digitalWrite(PIN_CSN, HIGH);
    delayMicroseconds(45);
    strobe(SRES);
    delay(1);
}

static bool detectPins() {
    const int orders[2][2] = {{44, 43}, {43, 44}};
    for (auto& o : orders) {
        pinSck = o[0];
        pinMiso = o[1];
        spiBegin();
        pinMode(pinMiso, INPUT); // re-read MISO via GPIO for CHIP_RDYn polling
        spiBegin();
        reset();
        uint8_t part = readStatus(PARTNUM), ver = readStatus(VERSION);
        Serial.printf("       SCK=%d MISO=%d -> PARTNUM=0x%02X VERSION=0x%02X\n", pinSck, pinMiso, part, ver);
        if (part == 0x00 && ver != 0x00 && ver != 0xFF) return true;
    }
    return false;
}

// 2-FSK, 38.4 kbaud, 20 kHz deviation, 100 kHz RX BW, variable length, CRC (SmartRF defaults).
static void configure(float mhz) {
    strobe(SIDLE);
    writeReg(IOCFG2, 0x29); // CHIP_RDYn
    writeReg(IOCFG0, 0x06); // sync sent/received, deasserts at end of packet
    writeReg(FIFOTHR, 0x47);
    writeReg(SYNC1, 0xD3);
    writeReg(SYNC0, 0x91);
    writeReg(PKTLEN, 0x3D);
    writeReg(PKTCTRL1, 0x04);
    writeReg(PKTCTRL0, 0x05);
    writeReg(FSCTRL1, 0x06);
    uint32_t f = (uint32_t)(mhz * 65536.0f / 26.0f + 0.5f);
    writeReg(FREQ2, f >> 16);
    writeReg(FREQ1, f >> 8);
    writeReg(FREQ0, f);
    writeReg(MDMCFG4, 0xCA);
    writeReg(MDMCFG3, 0x83);
    writeReg(MDMCFG2, 0x13);
    writeReg(DEVIATN, 0x35);
    writeReg(MCSM0, 0x18); // autocal IDLE->RX/TX
    writeReg(FOCCFG, 0x16);
    writeReg(AGCCTRL2, 0x43);
    writeReg(FREND0, 0x10);
    writeReg(FSCAL3, 0xE9);
    writeReg(FSCAL2, 0x2A);
    writeReg(FSCAL1, 0x00);
    writeReg(FSCAL0, 0x1F);
    writeReg(TEST2, 0x81);
    writeReg(TEST1, 0x35);
    writeReg(TEST0, 0x09);
    uint8_t pa = mhz < 600 ? 0x60 : 0x50; // ~0 dBm (433 / 868 tables)
    writeReg(PATABLE, pa);
}

// GDOx output config: 0x2F = constant 0, 0x6F = inverted -> constant 1.
static bool checkGdo(uint8_t reg, int pin, const char* name) {
    writeReg(reg, 0x2F);
    delayMicroseconds(50);
    int lo = digitalRead(pin);
    writeReg(reg, 0x6F);
    delayMicroseconds(50);
    int hi = digitalRead(pin);
    CHECK(lo == 0 && hi == 1, "%s line on GPIO%d follows the chip (forced 0 -> %d, forced 1 -> %d)", name, pin, lo, hi);
    return lo == 0 && hi == 1;
}

static void bandTest(float mhz) {
    configure(mhz);
    strobe(SCAL);
    bool idle = waitMarc(0x01, 10);
    uint8_t fscal1 = readReg(FSCAL1);
    CHECK(idle && fscal1 != 0x3F, "PLL calibrates at %.2f MHz (FSCAL1=0x%02X)", mhz, fscal1);

    strobe(SFRX);
    strobe(SRX);
    bool rx = waitMarc(0x0D, 10);
    delay(5);
    float sum = 0, mn = 0, mx = -200;
    for (int i = 0; i < 30; i++) {
        float r = rssiDbm();
        sum += r;
        mn = min(mn, r);
        mx = max(mx, r);
        delay(3);
    }
    float avg = sum / 30;
    CHECK(rx && avg < -60 && avg > -125, "RX at %.2f MHz: noise floor avg %.1f dBm (min %.1f, max %.1f)", mhz, avg, mn,
          mx);
    strobe(SIDLE);
    waitMarc(0x01, 10);

    // TX one packet; GDO0 (0x06) goes high at sync and low at end of packet.
    char msg[24];
    int n = snprintf(msg, sizeof(msg), "LILKA-CC1101 %.0f", mhz);
    strobe(SFTX);
    uint8_t buf[25];
    buf[0] = n;
    memcpy(buf + 1, msg, n);
    xfer(TXFIFO | 0x40, buf, nullptr, n + 1);
    uint32_t t0 = micros();
    strobe(STX);
    bool rose = false;
    while (micros() - t0 < 50000) {
        if (digitalRead(PIN_GDO0)) {
            rose = true;
            break;
        }
    }
    uint32_t tRise = micros() - t0;
    while (rose && digitalRead(PIN_GDO0) && micros() - t0 < 100000) {
    }
    uint32_t tFall = micros() - t0;
    bool backIdle = waitMarc(0x01, 20);
    uint8_t txb = readStatus(TXBYTES) & 0x7F;
    // 38.4 kbaud: (4 preamble + 2 sync + 1 len + n + 2 CRC) * 8 / 38400
    uint32_t expectUs = (uint32_t)((4 + 2 + 1 + n + 2) * 8 * 1e6 / 38400);
    CHECK(rose && backIdle && txb == 0, "TX \"%s\": GDO0 sync at %lu us, end at %lu us (~%lu us on air expected), FIFO empty",
          msg, (unsigned long)tRise, (unsigned long)tFall, (unsigned long)expectUs);
}

static void spectrum() {
    // Coarse RSSI sweep, useful to spot nearby transmitters (433 ISM / 868 SRD).
    const float bands[2][2] = {{430.0f, 440.0f}, {863.0f, 870.0f}};
    for (auto& b : bands) {
        Serial.printf("       RSSI %.0f-%.0f MHz:", b[0], b[1]);
        for (float f = b[0]; f <= b[1] + 0.01f; f += (b[1] - b[0]) / 20) {
            configure(f);
            strobe(SRX);
            waitMarc(0x0D, 10);
            delay(2);
            float peak = -200;
            for (int i = 0; i < 10; i++) peak = max(peak, rssiDbm());
            strobe(SIDLE);
            Serial.printf(" %.0f", peak);
        }
        Serial.println(" dBm");
    }
}

static void selfTest() {
    fails = 0;
    Serial.println("\n===== LILKA MOD RF + CC1101 self-test =====");
    pinMode(13, INPUT);
    pinMode(12, INPUT);
    CHECK(digitalRead(13) == 0 && digitalRead(12) == 1, "Header pin 9 (GPIO13) = GND, pin 10 (GPIO12) = 3V3");

    pinMode(PIN_CSN, OUTPUT);
    digitalWrite(PIN_CSN, HIGH);
    pinMode(PIN_GDO0, INPUT);
    pinMode(PIN_GDO2, INPUT);

    CHECK(detectPins(), "SPI link: PARTNUM=0x00 (SCK=GPIO%d, MISO=GPIO%d)", pinSck, pinMiso);
    if (fails > 1 || (fails == 1 && readStatus(PARTNUM) != 0)) {
        Serial.println("No CC1101 response - check module orientation / 3V3 / CSN.");
        return;
    }
    uint8_t ver = readStatus(VERSION);
    CHECK(ver == 0x14 || ver == 0x04 || ver == 0x17, "VERSION=0x%02X (0x14 = genuine CC1101)", ver);

    const uint8_t pat[3] = {0xA5, 0x3C, 0x96};
    uint8_t back[3] = {};
    xfer(SYNC1 | 0x40, pat, nullptr, 3); // SYNC1, SYNC0, PKTLEN burst
    xfer(SYNC1 | 0xC0, nullptr, back, 3);
    CHECK(memcmp(pat, back, 3) == 0, "Register burst write/read-back %02X %02X %02X", back[0], back[1], back[2]);

    checkGdo(IOCFG0, PIN_GDO0, "GDO0");
    checkGdo(IOCFG2, PIN_GDO2, "GDO2");

    bandTest(433.92f);
    bandTest(868.30f);
    spectrum();

    strobe(SIDLE);
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
