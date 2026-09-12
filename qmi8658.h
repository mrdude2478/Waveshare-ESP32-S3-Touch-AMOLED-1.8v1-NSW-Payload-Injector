#pragma once
#include <stdint.h>

// Minimal QMI8658 driver covering just what shake-to-switch-song needs:
// probe the chip, configure the accelerometer, and turn raw readings into
// a debounced "3 shakes happened" event. Not a general-purpose IMU driver
// (no gyro, no interrupt-pin support) - see qmi8658.cpp's top comment if
// you want to extend it.

// Call once, after Wire.begin(IIC_SDA, IIC_SCL) has already run (this board
// shares one I2C bus between the touch panel, PMU, and this chip). Probes
// both common QMI8658 addresses (0x6A, 0x6B) and configures the
// accelerometer. Returns false if no QMI8658 answered - in that case every
// other function here is a harmless no-op, so it's safe to call
// unconditionally even if your board doesn't have the chip populated.
bool qmi8658_init(void);

// True once qmi8658_init() has found and configured the chip.
bool qmi8658_available(void);

// Call this often - once per loop() iteration is ideal. It rate-limits its
// own I2C traffic internally (see POLL_INTERVAL_MS in the .cpp), so calling
// it more often than that just costs a cheap millis() check, not an extra
// I2C transaction.
//
// Returns true exactly once: the instant a run of SHAKE_COUNT_TARGET
// distinct shakes is detected within the detection window. That single
// `true` is the cue to do something (e.g. switchToRandomSong()) - the
// internal counter resets itself afterward and a cooldown prevents the
// deceleration of setting the board back down from starting a new count.
bool qmi8658_poll_for_shake(void);
