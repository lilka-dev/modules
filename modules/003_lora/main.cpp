// LILKA MOD LORA bring-up test (E22-900M2xS = SX1262 or E22-400M2xS = SX1268, TCXO, external RXEN/TXEN switch).
//
// Lilka v2 extension header -> module J2:
//   1 GND | 2 3V3 | 3 RX(44)=SCK | 4 TX(43)=MISO | 5 GPIO48=MOSI | 6 GPIO47=NSS
//   7 GPIO21=RXEN | 8 GPIO14=TXEN | 9 GPIO13=NRST | 10 GPIO12=BUSY | 11 3V3 | 12 GND
// DIO1 is not routed, so IRQs are polled over SPI.
//
// Serial commands: 't' rerun self-test, 'k' boot back to app0 (Keira).

#include <Arduino.h>
#include <SPI.h>
#include <esp_ota_ops.h>

static const int PIN_MOSI = 48;
static const int PIN_NSS = 47;
static const int PIN_RXEN = 21;
static const int PIN_TXEN = 14;
static const int PIN_NRST = 13;
static const int PIN_BUSY = 12;
// Header pins 3/4 are UART TX/RX; detect which way round they are wired.
static int pinSck = 44;
static int pinMiso = 43;

// 868.1 MHz for E22-900M* (SX1262); the lora_433 env builds 433.175 MHz for E22-400M* (SX1268).
#ifndef LORA_FREQ_HZ
#    define LORA_FREQ_HZ 868100000
#endif
static const uint32_t FREQ_HZ = LORA_FREQ_HZ;
static const int8_t TX_POWER_DBM = 0; // keep low: safe even with a poor antenna

static SPIClass loraSpi(HSPI);
static const SPISettings spiSettings(2000000, MSBFIRST, SPI_MODE0);
static int fails = 0;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        bool ok_ = (cond);                                                                                   \
        Serial.printf("[%s] ", ok_ ? " OK " : "FAIL");                                                       \
        Serial.printf(__VA_ARGS__);                                                                          \
        Serial.println();                                                                                    \
        if (!ok_) fails++;                                                                                   \
    } while (0)

static bool waitBusy(uint32_t timeoutMs = 100) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY)) {
        if (millis() - t0 > timeoutMs) return false;
        delayMicroseconds(10);
    }
    return true;
}

// Generic SX126x transfer: writes tx[], returns the bytes clocked back into rx[].
static bool xfer(const uint8_t* tx, uint8_t* rx, size_t n) {
    if (!waitBusy()) return false;
    loraSpi.beginTransaction(spiSettings);
    digitalWrite(PIN_NSS, LOW);
    for (size_t i = 0; i < n; i++) {
        uint8_t r = loraSpi.transfer(tx[i]);
        if (rx) rx[i] = r;
    }
    digitalWrite(PIN_NSS, HIGH);
    loraSpi.endTransaction();
    return true;
}

static bool cmd(std::initializer_list<uint8_t> bytes) {
    uint8_t buf[16];
    size_t n = 0;
    for (uint8_t b : bytes) buf[n++] = b;
    return xfer(buf, nullptr, n);
}

static uint8_t readReg(uint16_t addr) {
    uint8_t tx[5] = {0x1D, (uint8_t)(addr >> 8), (uint8_t)addr, 0, 0};
    uint8_t rx[5] = {};
    xfer(tx, rx, 5);
    return rx[4];
}

static void writeReg(uint16_t addr, uint8_t v) {
    cmd({0x0D, (uint8_t)(addr >> 8), (uint8_t)addr, v});
}

static uint8_t getStatus() {
    uint8_t tx[2] = {0xC0, 0}, rx[2] = {};
    xfer(tx, rx, 2);
    return rx[1];
}

static uint16_t getIrq() {
    uint8_t tx[4] = {0x12, 0, 0, 0}, rx[4] = {};
    xfer(tx, rx, 4);
    return (rx[2] << 8) | rx[3];
}

static uint16_t getDeviceErrors() {
    uint8_t tx[4] = {0x17, 0, 0, 0}, rx[4] = {};
    xfer(tx, rx, 4);
    return (rx[2] << 8) | rx[3];
}

static float getRssiInst() {
    uint8_t tx[3] = {0x15, 0, 0}, rx[3] = {};
    xfer(tx, rx, 3);
    return -rx[2] / 2.0f;
}

static void writeBuffer(uint8_t offset, const uint8_t* data, size_t n) {
    uint8_t tx[2 + 64] = {0x0E, offset};
    memcpy(tx + 2, data, n);
    xfer(tx, nullptr, 2 + n);
}

static void readBuffer(uint8_t offset, uint8_t* data, size_t n) {
    uint8_t tx[3 + 64] = {0x1E, offset, 0};
    uint8_t rx[3 + 64] = {};
    xfer(tx, rx, 3 + n);
    memcpy(data, rx + 3, n);
}

static void setRfSwitch(bool tx) {
    digitalWrite(PIN_RXEN, tx ? LOW : HIGH);
    digitalWrite(PIN_TXEN, tx ? HIGH : LOW);
}

static bool hwReset() {
    digitalWrite(PIN_NRST, LOW);
    delay(2);
    bool busyDuringReset = digitalRead(PIN_BUSY);
    digitalWrite(PIN_NRST, HIGH);
    uint32_t t0 = millis();
    bool ready = waitBusy(200);
    Serial.printf("       BUSY in reset=%d, ready after %lu ms\n", busyDuringReset, millis() - t0);
    return ready;
}

static void spiBegin() {
    loraSpi.end();
    loraSpi.begin(pinSck, pinMiso, PIN_MOSI, -1);
}

static bool detectPins() {
    const int orders[2][2] = {{44, 43}, {43, 44}};
    for (auto& o : orders) {
        pinSck = o[0];
        pinMiso = o[1];
        spiBegin();
        hwReset();
        uint8_t a = readReg(0x0740), b = readReg(0x0741);
        Serial.printf("       SCK=%d MISO=%d -> sync word regs 0x%02X 0x%02X\n", pinSck, pinMiso, a, b);
        if (a == 0x14 && b == 0x24) return true;
    }
    return false;
}

static void configureLora() {
    cmd({0x80, 0x00});                   // SetStandby(STDBY_RC)
    cmd({0x96, 0x01});                   // SetRegulatorMode(DC-DC)
    cmd({0x97, 0x02, 0x00, 0x01, 0x40}); // SetDIO3AsTcxoCtrl(1.8V, 5 ms)
    cmd({0x89, 0x7F});                   // Calibrate(all)
    delay(10);
    waitBusy(500);
    cmd({0x8A, 0x01}); // SetPacketType(LoRa)
    if (FREQ_HZ < 500000000)
        cmd({0x98, 0x6B, 0x6F}); // CalibrateImage(430-440 MHz)
    else if (FREQ_HZ < 900000000)
        cmd({0x98, 0xD7, 0xDB}); // CalibrateImage(863-870 MHz)
    else
        cmd({0x98, 0xE1, 0xE9}); // CalibrateImage(902-928 MHz)
    waitBusy(500);
    uint32_t frf = (uint32_t)(((uint64_t)FREQ_HZ << 25) / 32000000ULL);
    cmd({0x86, (uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf});
    cmd({0x95, 0x04, 0x07, 0x00, 0x01});                   // SetPaConfig (SX1262 / SX1268)
    cmd({0x8E, (uint8_t)TX_POWER_DBM, 0x04});              // SetTxParams(power, 200us ramp)
    cmd({0x8B, 0x07, 0x04, 0x01, 0x00});                   // SF7, BW125, CR4/5, LDRO off
    cmd({0x8F, 0x00, 0x00});                               // SetBufferBaseAddress
    cmd({0x08, 0x02, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); // IRQ mask: TxDone|RxDone|HdrErr|CrcErr|Timeout
    writeReg(0x08D8, readReg(0x08D8) | 0x1E);              // errata 15.2: TX clamp
}

static void setPacketParams(uint8_t len) {
    cmd({0x8C, 0x00, 0x08, 0x00, len, 0x01, 0x00}); // preamble 8, explicit hdr, CRC on, std IQ
}

static void startRx() {
    setPacketParams(0xFF);
    setRfSwitch(false);
    cmd({0x02, 0xFF, 0xFF});             // ClearIrqStatus
    cmd({0x82, 0xFF, 0xFF, 0xFF});       // SetRx(continuous)
}

static uint32_t txCounter = 0;

static bool transmit(uint32_t* airMs) {
    char msg[32];
    int n = snprintf(msg, sizeof(msg), "LILKA-LORA #%lu", (unsigned long)txCounter++);
    cmd({0x80, 0x00});
    setPacketParams(n);
    writeBuffer(0, (const uint8_t*)msg, n);
    cmd({0x02, 0xFF, 0xFF});
    setRfSwitch(true);
    uint32_t t0 = millis();
    cmd({0x83, 0x00, 0x00, 0x00}); // SetTx(no timeout)
    uint16_t irq = 0;
    while (millis() - t0 < 2000) {
        irq = getIrq();
        if (irq & 0x0001) break;
        delay(1);
    }
    *airMs = millis() - t0;
    setRfSwitch(false);
    Serial.printf("       TX \"%s\" irq=0x%04X\n", msg, irq);
    return irq & 0x0001;
}

static void selfTest() {
    fails = 0;
    Serial.println("\n===== LILKA MOD LORA self-test =====");
    pinMode(PIN_NSS, OUTPUT);
    digitalWrite(PIN_NSS, HIGH);
    pinMode(PIN_NRST, OUTPUT);
    pinMode(PIN_RXEN, OUTPUT);
    pinMode(PIN_TXEN, OUTPUT);
    pinMode(PIN_BUSY, INPUT);
    setRfSwitch(false);

    CHECK(detectPins(), "SPI link: sync word reads 0x1424 (SCK=GPIO%d, MISO=GPIO%d)", pinSck, pinMiso);
    if (fails) {
        Serial.println("No SPI response - check module seating / 3V3 / NSS / RST.");
        return;
    }
    CHECK(hwReset(), "NRST/BUSY: chip leaves reset and BUSY goes low");

    uint8_t st = getStatus();
    CHECK(((st >> 4) & 7) == 2, "GetStatus=0x%02X (chip mode %d, expect 2=STBY_RC)", st, (st >> 4) & 7);

    uint8_t pattern[32], back[32];
    for (int i = 0; i < 32; i++) pattern[i] = (uint8_t)(i * 37 + 0x5A);
    writeBuffer(0, pattern, 32);
    readBuffer(0, back, 32);
    CHECK(memcmp(pattern, back, 32) == 0, "Data buffer write/read-back 32 bytes (MOSI/MISO integrity)");

    cmd({0x07, 0x00, 0x00}); // ClearDeviceErrors
    configureLora();
    uint16_t err = getDeviceErrors();
    CHECK((err & 0x7F) == 0, "Calibration/TCXO: device errors=0x%04X%s", err, (err & 0x20) ? " (XOSC_START_ERR: TCXO?)" : "");
    st = getStatus();
    CHECK(((st >> 4) & 7) == 2, "Back in STBY_RC after config (status 0x%02X)", st);

    startRx();
    delay(20);
    st = getStatus();
    CHECK(((st >> 4) & 7) == 5, "RX mode entered (status 0x%02X, mode %d)", st, (st >> 4) & 7);
    float sum = 0, mn = 0, mx = -200;
    for (int i = 0; i < 20; i++) {
        float r = getRssiInst();
        sum += r;
        mn = min(mn, r);
        mx = max(mx, r);
        delay(5);
    }
    float avg = sum / 20;
    CHECK(avg < -70 && avg > -135, "RX noise floor @%.1f MHz: avg %.1f dBm (min %.1f, max %.1f)", FREQ_HZ / 1e6, avg,
          mn, mx);

    uint32_t air = 0;
    bool txOk = transmit(&air);
    CHECK(txOk && air > 20 && air < 200, "TX done at %d dBm, time on air %lu ms (expect ~40-60 ms)", TX_POWER_DBM,
          (unsigned long)air);

    startRx();
    Serial.printf("===== RESULT: %s (%d failure%s) =====\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    Serial.printf("Listening on %.3f MHz SF7/BW125, TX beacon every 10 s. 't'=retest, 'k'=back to Keira.\n",
                  FREQ_HZ / 1e6);
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) delay(10);
    delay(500);
    selfTest();
}

void loop() {
    static uint32_t lastTx = millis();
    static uint32_t lastRepeat = millis();

    if (Serial.available()) {
        char c = Serial.read();
        if (c == 't') selfTest();
        if (c == 'k') {
            const esp_partition_t* p =
                esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
            Serial.printf("Switching boot to %s\n", p ? p->label : "?");
            if (p) esp_ota_set_boot_partition(p);
            delay(200);
            esp_restart();
        }
    }

    if (fails == 0) {
        uint16_t irq = getIrq();
        if (irq & 0x0002) {
            uint8_t tx[4] = {0x13, 0, 0, 0}, rx[4] = {};
            xfer(tx, rx, 4);
            uint8_t len = rx[2], start = rx[3];
            uint8_t ps[5] = {0x14, 0, 0, 0, 0}, pr[5] = {};
            xfer(ps, pr, 5);
            uint8_t data[64] = {};
            readBuffer(start, data, min<uint8_t>(len, 63));
            Serial.printf("RX len=%u rssi=%.1f snr=%.2f crcErr=%d: %s\n", len, -pr[2] / 2.0f, (int8_t)pr[3] / 4.0f,
                          (irq & 0x0040) != 0, (char*)data);
            cmd({0x02, 0xFF, 0xFF});
        }
        if (millis() - lastTx > 10000) {
            uint32_t air;
            bool ok = transmit(&air);
            Serial.printf("       beacon %s, %lu ms\n", ok ? "sent" : "FAILED", (unsigned long)air);
            startRx();
            lastTx = millis();
        }
    }
    // Repeat the report periodically so a late serial connection still sees it.
    if (millis() - lastRepeat > 30000) {
        selfTest();
        lastRepeat = millis();
        lastTx = millis();
    }
    delay(5);
}
