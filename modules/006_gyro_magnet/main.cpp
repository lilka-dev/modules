// LILKA MOD GYRO MAGNET bring-up test (BMI260 accel/gyro + TMAG5273A1 3D Hall magnetometer).
//
// Lilka v2 extension header -> module J2:
//   3 RX(44)=I2C_SDA | 4 TX(43)=I2C_SCL | 5 GPIO48=MAG_INT# | 6 GPIO47=GYR_INT1 | 7 GPIO21=GYR_INT2
//   8 GPIO14, 9 GPIO13, 10 GPIO12 = GND (wired straight to GND, never drive them)
// BMI260 SDO only goes to TP5, so its address (0x68/0x69) is found by scanning.
//
// Serial commands: 't' rerun self-test, 'k' boot back to app0 (Keira).

#include <Arduino.h>
#include <Wire.h>
#include <esp_ota_ops.h>
#include "bmi260_config/bmi260_config.h"

static const int PIN_SDA = 44;
static const int PIN_SCL = 43;
static const int PIN_MAG_INT = 48;
static const int PIN_GYR_INT1 = 47;
static const int PIN_GYR_INT2 = 21;
static const int IDLE_PINS[] = {48, 47, 21, 14, 13, 12}; // header pins 5..10 (pull-ups, GND wiring), MSB first
static const uint8_t EXPECTED_IDLE = 0x38;

static const uint8_t TMAG_ADDR = 0x35; // TMAG5273A1 default
static uint8_t bmiAddr = 0;
static int fails = 0;
static bool bmiReady = false;
static bool magReady = false;

static volatile uint32_t int1Count = 0, int2Count = 0, magIntCount = 0;
static void IRAM_ATTR onInt1() {
    int1Count++;
}
static void IRAM_ATTR onInt2() {
    int2Count++;
}
static void IRAM_ATTR onMagInt() {
    magIntCount++;
}

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        bool ok_ = (cond);                                                                                   \
        Serial.printf("[%s] ", ok_ ? " OK " : "FAIL");                                                       \
        Serial.printf(__VA_ARGS__);                                                                          \
        Serial.println();                                                                                    \
        if (!ok_) fails++;                                                                                   \
    } while (0)

static bool present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool writeReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t n) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(data, n);
    return Wire.endTransmission() == 0;
}

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t v) {
    return writeReg(addr, reg, &v, 1);
}

static bool readReg(uint8_t addr, uint8_t reg, uint8_t* data, size_t n) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)n) != n) return false;
    for (size_t i = 0; i < n; i++) data[i] = Wire.read();
    return true;
}

static uint8_t readReg(uint8_t addr, uint8_t reg) {
    uint8_t v = 0xFF;
    readReg(addr, reg, &v, 1);
    return v;
}

static uint8_t readIdleLevels() {
    uint8_t levels = 0;
    for (int p : IDLE_PINS) {
        pinMode(p, INPUT);
        levels = (levels << 1) | digitalRead(p);
    }
    return levels;
}

// ---------------- BMI260 ----------------
enum : uint8_t {
    BMI_CHIP_ID = 0x00,
    BMI_ERR_REG = 0x02,
    BMI_ACC_X = 0x0C,
    BMI_INTERNAL_STATUS = 0x21,
    BMI_TEMP = 0x22,
    BMI_ACC_CONF = 0x40,
    BMI_ACC_RANGE = 0x41,
    BMI_GYR_CONF = 0x42,
    BMI_GYR_RANGE = 0x43,
    BMI_INT1_IO_CTRL = 0x53,
    BMI_INT2_IO_CTRL = 0x54,
    BMI_INT_LATCH = 0x55,
    BMI_INT_MAP_DATA = 0x58,
    BMI_INIT_CTRL = 0x59,
    BMI_INIT_ADDR_0 = 0x5B,
    BMI_INIT_DATA = 0x5E,
    BMI_PWR_CONF = 0x7C,
    BMI_PWR_CTRL = 0x7D,
    BMI_CMD = 0x7E,
};

static bool bmiLoadConfig() {
    writeReg(bmiAddr, BMI_CMD, 0xB6); // soft reset
    delay(2);
    readReg(bmiAddr, BMI_CHIP_ID); // dummy read (switches to I2C after reset)
    writeReg(bmiAddr, BMI_PWR_CONF, 0x00);
    delayMicroseconds(450);
    writeReg(bmiAddr, BMI_INIT_CTRL, 0x00);
    const size_t chunk = 64;
    for (size_t off = 0; off < sizeof(g_bmi260_config_tbin); off += chunk) {
        uint8_t a[2] = {(uint8_t)((off / 2) & 0x0F), (uint8_t)((off / 2) >> 4)};
        writeReg(bmiAddr, BMI_INIT_ADDR_0, a, 2);
        if (!writeReg(bmiAddr, BMI_INIT_DATA, g_bmi260_config_tbin + off, chunk)) return false;
    }
    writeReg(bmiAddr, BMI_INIT_CTRL, 0x01);
    uint32_t t0 = millis();
    while (millis() - t0 < 150) {
        if ((readReg(bmiAddr, BMI_INTERNAL_STATUS) & 0x0F) == 0x01) return true;
        delay(5);
    }
    return false;
}

struct Imu {
    float ax, ay, az; // g
    float gx, gy, gz; // dps
};

static bool bmiRead(Imu& m) {
    uint8_t b[12];
    if (!readReg(bmiAddr, BMI_ACC_X, b, 12)) return false;
    auto s16 = [&](int i) { return (int16_t)(b[i] | (b[i + 1] << 8)); };
    m.ax = s16(0) / 4096.0f; // +-8 g
    m.ay = s16(2) / 4096.0f;
    m.az = s16(4) / 4096.0f;
    m.gx = s16(6) / 16.384f; // +-2000 dps
    m.gy = s16(8) / 16.384f;
    m.gz = s16(10) / 16.384f;
    return true;
}

static float bmiTemp() {
    uint8_t b[2];
    readReg(bmiAddr, BMI_TEMP, b, 2);
    int16_t t = (int16_t)(b[0] | (b[1] << 8));
    return t / 512.0f + 23.0f;
}

static void testBmi() {
    uint8_t id = readReg(bmiAddr, BMI_CHIP_ID);
    CHECK(id == 0x27, "BMI260 @0x%02X CHIP_ID=0x%02X (expect 0x27)", bmiAddr, id);

    uint32_t t0 = millis();
    bool loaded = bmiLoadConfig();
    uint8_t st = readReg(bmiAddr, BMI_INTERNAL_STATUS);
    CHECK(loaded, "BMI260 config (8 KB) uploaded in %lu ms, INTERNAL_STATUS=0x%02X (expect 0x01 init_ok)",
          millis() - t0, st);
    if (!loaded) return;

    writeReg(bmiAddr, BMI_PWR_CTRL, 0x0E);  // acc + gyr + temp on
    writeReg(bmiAddr, BMI_ACC_CONF, 0xA8);  // 100 Hz, normal, perf mode
    writeReg(bmiAddr, BMI_ACC_RANGE, 0x02); // +-8 g
    writeReg(bmiAddr, BMI_GYR_CONF, 0xA9);  // 200 Hz, normal, perf mode
    writeReg(bmiAddr, BMI_GYR_RANGE, 0x00); // +-2000 dps
    writeReg(bmiAddr, BMI_INT1_IO_CTRL, 0x0A); // output, push-pull, active high
    writeReg(bmiAddr, BMI_INT2_IO_CTRL, 0x0A);
    writeReg(bmiAddr, BMI_INT_LATCH, 0x00);
    writeReg(bmiAddr, BMI_INT_MAP_DATA, 0x44); // data ready -> INT1 and INT2
    delay(100);
    uint8_t err = readReg(bmiAddr, BMI_ERR_REG);
    CHECK(err == 0, "BMI260 ERR_REG=0x%02X after configuring accel/gyro", err);

    // Average at rest.
    Imu sum = {}, m;
    int n = 0;
    float gMax = 0;
    int1Count = int2Count = 0;
    for (int i = 0; i < 100; i++) {
        if (bmiRead(m)) {
            sum.ax += m.ax;
            sum.ay += m.ay;
            sum.az += m.az;
            sum.gx += m.gx;
            sum.gy += m.gy;
            sum.gz += m.gz;
            gMax = max(gMax, max(fabsf(m.gx), max(fabsf(m.gy), fabsf(m.gz))));
            n++;
        }
        delay(5);
    }
    uint32_t c1 = int1Count, c2 = int2Count;
    float ax = sum.ax / n, ay = sum.ay / n, az = sum.az / n;
    float gx = sum.gx / n, gy = sum.gy / n, gz = sum.gz / n;
    float g = sqrtf(ax * ax + ay * ay + az * az);
    CHECK(n == 100 && g > 0.85f && g < 1.15f, "Accel at rest: (%.3f, %.3f, %.3f) g, |a| = %.3f g (expect ~1)", ax, ay,
          az, g);
    CHECK(fabsf(gx) < 5 && fabsf(gy) < 5 && fabsf(gz) < 5 && gMax > 0.0f,
          "Gyro at rest: (%.2f, %.2f, %.2f) dps, peak %.2f dps (keep the Lilka still)", gx, gy, gz, gMax);
    float t = bmiTemp();
    CHECK(t > 5 && t < 60, "BMI260 temperature %.1f C", t);
    CHECK(c1 > 20 && c2 > 20, "GYR_INT1 (GPIO47) %lu / GYR_INT2 (GPIO21) %lu data-ready pulses in ~0.5 s",
          (unsigned long)c1, (unsigned long)c2);

    // Static level check: unmap interrupts, then drive each INT pin idle-high / idle-low via its polarity bit.
    writeReg(bmiAddr, BMI_INT_MAP_DATA, 0x00);
    const struct {
        uint8_t reg;
        int pin;
        const char* name;
    } ints[] = {{BMI_INT1_IO_CTRL, PIN_GYR_INT1, "INT1/GPIO47"}, {BMI_INT2_IO_CTRL, PIN_GYR_INT2, "INT2/GPIO21"}};
    for (auto& it : ints) {
        writeReg(bmiAddr, it.reg, 0x08); // output, push-pull, active low -> idles high
        delay(2);
        int hi = digitalRead(it.pin);
        writeReg(bmiAddr, it.reg, 0x0A); // active high -> idles low
        delay(2);
        int lo = digitalRead(it.pin);
        writeReg(bmiAddr, it.reg, 0x00); // output off -> 10k pull-up
        delay(2);
        pinMode(it.pin, INPUT_PULLDOWN);
        delay(2);
        int floatLvl = digitalRead(it.pin);
        pinMode(it.pin, INPUT);
        CHECK(hi == 1 && lo == 0, "%s static levels: drive-high -> %d, drive-low -> %d, released (+pulldown) -> %d",
              it.name, hi, lo, floatLvl);
    }
    writeReg(bmiAddr, BMI_INT1_IO_CTRL, 0x00);
    writeReg(bmiAddr, BMI_INT2_IO_CTRL, 0x00);
    bmiReady = true;
}

// ---------------- TMAG5273 ----------------
enum : uint8_t {
    TM_DEVICE_CONFIG_1 = 0x00,
    TM_DEVICE_CONFIG_2 = 0x01,
    TM_SENSOR_CONFIG_1 = 0x02,
    TM_SENSOR_CONFIG_2 = 0x03,
    TM_T_CONFIG = 0x07,
    TM_INT_CONFIG_1 = 0x08,
    TM_DEVICE_ID = 0x0D,
    TM_MANUFACTURER_ID_LSB = 0x0E,
    TM_T_MSB = 0x10,
    TM_X_MSB = 0x12,
    TM_CONV_STATUS = 0x18,
    TM_DEVICE_STATUS = 0x1C,
};

struct Mag {
    float x, y, z; // uT
    float t;       // C
};

static float magRangeMt = 40.0f;

static bool magRead(Mag& m) {
    uint8_t b[8];
    if (!readReg(TMAG_ADDR, TM_T_MSB, b, 8)) return false;
    auto s16 = [&](int i) { return (int16_t)((b[i] << 8) | b[i + 1]); };
    m.t = 25.0f + (s16(0) - 17508) / 60.1f;
    float k = magRangeMt * 1000.0f / 32768.0f;
    m.x = s16(2) * k;
    m.y = s16(4) * k;
    m.z = s16(6) * k;
    return true;
}

static void testMag() {
    uint8_t man[2] = {};
    readReg(TMAG_ADDR, TM_MANUFACTURER_ID_LSB, man, 2);
    uint8_t dev = readReg(TMAG_ADDR, TM_DEVICE_ID);
    CHECK(man[0] == 0x49 && man[1] == 0x54, "TMAG5273 @0x%02X manufacturer ID 0x%02X%02X (expect 0x5449 'TI')",
          TMAG_ADDR, man[1], man[0]);
    magRangeMt = (dev & 0x03) == 2 ? 133.0f : 40.0f;
    CHECK((dev & 0x03) == 1, "TMAG5273 DEVICE_ID=0x%02X (VER=%d -> x1 = +-40/80 mT, A1 variant)", dev, dev & 0x03);

    writeReg(TMAG_ADDR, TM_DEVICE_CONFIG_1, 0x14);  // 32x averaging (lower noise)
    writeReg(TMAG_ADDR, TM_SENSOR_CONFIG_1, 0x70);  // X, Y, Z enabled
    writeReg(TMAG_ADDR, TM_SENSOR_CONFIG_2, 0x00);  // +-40 mT on all axes
    writeReg(TMAG_ADDR, TM_T_CONFIG, 0x01);         // temperature on
    writeReg(TMAG_ADDR, TM_INT_CONFIG_1, 0xA4);     // INT on each result, 10 us pulse, INT pin
    writeReg(TMAG_ADDR, TM_DEVICE_CONFIG_2, 0x02);  // continuous measure
    delay(50);
    uint8_t dst = readReg(TMAG_ADDR, TM_DEVICE_STATUS);

    Mag sum = {}, m;
    int n = 0;
    magIntCount = 0;
    for (int i = 0; i < 50; i++) {
        if (magRead(m)) {
            sum.x += m.x;
            sum.y += m.y;
            sum.z += m.z;
            sum.t += m.t;
            n++;
        }
        delay(10);
    }
    uint32_t ints = magIntCount;
    float x = sum.x / n, y = sum.y / n, z = sum.z / n, t = sum.t / n;
    float b = sqrtf(x * x + y * y + z * z);
    CHECK((dst & 0x0F) == 0, "TMAG5273 DEVICE_STATUS=0x%02X (no VCC/OTP/INT errors)", dst);
    CHECK(n == 50 && b > 5 && b < 3000, "Field: (%.1f, %.1f, %.1f) uT, |B| = %.1f uT (Earth ~25-65 uT + local magnets)",
          x, y, z, b);
    CHECK(t > 5 && t < 60, "TMAG5273 temperature %.1f C", t);
    CHECK(ints > 10, "MAG_INT# (GPIO48) %lu conversion-done pulses in ~0.5 s", (unsigned long)ints);
    magReady = true;
}

static void selfTest() {
    fails = 0;
    bmiReady = magReady = false;
    Serial.println("\n===== LILKA MOD GYRO MAGNET self-test =====");

    detachInterrupt(PIN_GYR_INT1);
    detachInterrupt(PIN_GYR_INT2);
    detachInterrupt(PIN_MAG_INT);

    // Release the INT lines (sensors may still drive them from a previous run) before reading the idle levels.
    Wire.end();
    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    writeReg(0x68, BMI_CMD, 0xB6);
    writeReg(0x69, BMI_CMD, 0xB6);
    writeReg(TMAG_ADDR, TM_INT_CONFIG_1, 0x00);
    writeReg(TMAG_ADDR, TM_DEVICE_CONFIG_2, 0x00);
    delay(5);
    uint8_t idle = readIdleLevels();
    CHECK(idle == EXPECTED_IDLE, "Idle levels, header pins 5-10 = 0x%02X, expect 0x%02X (pull-ups + GND wiring)", idle, EXPECTED_IDLE);

    Serial.print("       I2C scan:");
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (present(a)) {
            Serial.printf(" 0x%02X", a);
            found++;
        }
    }
    Serial.println(found ? "" : " (nothing)");

    bmiAddr = present(0x68) ? 0x68 : present(0x69) ? 0x69 : 0;
    CHECK(bmiAddr != 0, "BMI260 answers on I2C (0x68/0x69)");
    CHECK(present(TMAG_ADDR), "TMAG5273A1 answers on I2C (0x35)");

    attachInterrupt(PIN_GYR_INT1, onInt1, RISING);
    attachInterrupt(PIN_GYR_INT2, onInt2, RISING);
    attachInterrupt(PIN_MAG_INT, onMagInt, FALLING);

    if (bmiAddr) testBmi();
    if (present(TMAG_ADDR)) testMag();

    Serial.printf("===== RESULT: %s (%d failure%s) =====\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    Serial.println("Live data every 250 ms. 't'=retest, 'k'=back to Keira.");
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 4000) delay(10);
    delay(500);
    selfTest();
}

void loop() {
    static uint32_t lastLive = 0, lastTest = millis();
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 't') {
            selfTest();
            lastTest = millis();
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
    if (millis() - lastLive > 250) {
        lastLive = millis();
        Imu i = {};
        Mag m = {};
        if (bmiReady) bmiRead(i);
        if (magReady) magRead(m);
        float heading = atan2f(m.y, m.x) * 180.0f / PI;
        if (heading < 0) heading += 360;
        Serial.printf("acc %+6.2f %+6.2f %+6.2f g | gyr %+8.1f %+8.1f %+8.1f dps | mag %+7.1f %+7.1f %+7.1f uT | hdg %5.1f\n",
                      i.ax, i.ay, i.az, i.gx, i.gy, i.gz, m.x, m.y, m.z, heading);
    }
    if (millis() - lastTest > 60000) {
        selfTest();
        lastTest = millis();
    }
    delay(5);
}
