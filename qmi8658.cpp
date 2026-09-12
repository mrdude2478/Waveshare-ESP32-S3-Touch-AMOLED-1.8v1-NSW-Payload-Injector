// QMI8658 driver, accelerometer-only, tuned for "shake to switch song".
//
// Register map is from the QST QMI8658 datasheet (confirmed against a
// working implementation on a similar ESP32-S3 AMOLED touch board):
//   0x00 WHO_AM_I    (should read back 0x05)
//   0x02 CTRL1       (interface/auto-increment config)
//   0x03 CTRL2       (accelerometer full-scale + output data rate)
//   0x08 CTRL7       (per-sensor enable: bit0 = accel, bit1 = gyro)
//   0x35 AX_L        (start of 6-byte burst: AX_L/H, AY_L/H, AZ_L/H)
//
// This board wires the chip onto the same I2C bus (Wire, IIC_SDA/IIC_SCL)
// already used for the touch panel and the AXP2101 PMU, at address 0x6A or
// 0x6B depending on how SA0 is strapped - qmi8658_init() probes both.
#include "qmi8658.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "HWCDC.h"

extern HWCDC USBSerial;

#define QMI8658_WHO_AM_I      0x00
#define QMI8658_WHO_AM_I_VAL  0x05
#define QMI8658_CTRL1         0x02
#define QMI8658_CTRL2         0x03
#define QMI8658_CTRL7         0x08
#define QMI8658_AX_L          0x35

// CTRL2 = 0x23 below selects +/-8g full scale, which is 4096 LSB/g
// (32768 counts / 8g). If you change CTRL2's scale bits, update this too.
static const float ACCEL_LSB_PER_G = 4096.0f;

// A "shake" is detected as a peak in total acceleration magnitude above
// SHAKE_HIGH_G, followed later by a dip below SHAKE_LOW_G before the next
// peak is allowed to count. That hysteresis gap is what stops a single
// vigorous shake from being counted twice (once on the way up, once on the
// way down) - resting magnitude is ~1.0g, so these are both comfortably
// above that.
static const float SHAKE_HIGH_G = 1.8f;
static const float SHAKE_LOW_G  = 1.3f;

// 3 peaks within 2 seconds counts as an intentional "shake the board"
// gesture. After it fires, a cooldown blocks new counting for a bit so
// the jolt of setting the board back down doesn't start a fresh count.
static const uint8_t  SHAKE_COUNT_TARGET = 3;
static const uint32_t SHAKE_WINDOW_MS    = 2000;
static const uint32_t SHAKE_COOLDOWN_MS  = 1500;

// 50Hz is plenty for a gesture this coarse and keeps I2C traffic light.
static const uint32_t POLL_INTERVAL_MS = 20;

static uint8_t qmiAddr = 0;
static bool chipAvailable = false;

static bool armedForPeak = true;
static uint8_t shakeCount = 0;
static uint32_t firstShakeAt = 0;
static uint32_t cooldownUntil = 0;
static uint32_t lastPollAt = 0;

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    // Repeated start (no stop) before the read, same pattern the rest of
    // this codebase's I2C peripherals expect.
    if (Wire.endTransmission(false) != 0) return false;
    size_t got = Wire.requestFrom((int)addr, (int)len);
    if (got != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool qmi8658_init(void) {
    const uint8_t candidates[2] = {0x6B, 0x6A};
    for (uint8_t i = 0; i < 2; i++) {
        uint8_t who = 0;
        if (readRegs(candidates[i], QMI8658_WHO_AM_I, &who, 1) && who == QMI8658_WHO_AM_I_VAL) {
            qmiAddr = candidates[i];
            break;
        }
    }

    if (qmiAddr == 0) {
        USBSerial.println("[QMI8658] not found at 0x6A or 0x6B - shake-to-switch disabled");
        chipAvailable = false;
        return false;
    }

    writeReg(qmiAddr, QMI8658_CTRL1, 0x60); // enable register auto-increment for burst reads
    writeReg(qmiAddr, QMI8658_CTRL2, 0x23); // accel: +/-8g full scale, ~1kHz ODR
    writeReg(qmiAddr, QMI8658_CTRL7, 0x01); // enable accelerometer only (no gyro needed here)

    delay(2); // let the first conversion land before anyone polls

    USBSerial.printf("[QMI8658] found at 0x%02X, accelerometer enabled\n", qmiAddr);
    chipAvailable = true;
    return true;
}

bool qmi8658_available(void) {
    return chipAvailable;
}

bool qmi8658_poll_for_shake(void) {
    if (!chipAvailable) return false;

    uint32_t now = millis();
    if (now - lastPollAt < POLL_INTERVAL_MS) return false;
    lastPollAt = now;

    uint8_t raw[6];
    if (!readRegs(qmiAddr, QMI8658_AX_L, raw, 6)) return false; // skip a dropped read, try again next poll

    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5] << 8) | raw[4]);

    float gx = ax / ACCEL_LSB_PER_G;
    float gy = ay / ACCEL_LSB_PER_G;
    float gz = az / ACCEL_LSB_PER_G;
    float mag = sqrtf(gx * gx + gy * gy + gz * gz);

    // An in-progress count that never reached the target expires so a
    // couple of stray bumps a while apart don't add up into a false switch.
    if (shakeCount > 0 && (now - firstShakeAt) > SHAKE_WINDOW_MS) {
        shakeCount = 0;
    }

    if (now < cooldownUntil) {
        // Keep the hysteresis state honest during cooldown so we don't
        // immediately fire again the instant it ends, but don't count yet.
        if (mag < SHAKE_LOW_G) armedForPeak = true;
        return false;
    }

    if (armedForPeak && mag > SHAKE_HIGH_G) {
        armedForPeak = false; // must dip below SHAKE_LOW_G before the next peak counts

        if (shakeCount == 0) firstShakeAt = now;
        shakeCount++;

        if (shakeCount >= SHAKE_COUNT_TARGET) {
            shakeCount = 0;
            cooldownUntil = now + SHAKE_COOLDOWN_MS;
            return true;
        }
    } else if (!armedForPeak && mag < SHAKE_LOW_G) {
        armedForPeak = true;
    }

    return false;
}
