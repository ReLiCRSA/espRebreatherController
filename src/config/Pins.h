// Pin map. See AGENTS.md section 3.
//
// OPEN CONFLICT: AGENTS.md section 3 assigns GPIO 33 to both the vibration motor and the
// Page button. That is a documentation defect, not a resolved choice, so the Page button
// is left UNASSIGNED here (-1) rather than silently given a pin that is already driven.
// A shared pin in a life-support device fails in a way nobody notices until the motor
// back-biases a button read. Logged in AGENTS.md section 20; needs the user's choice.
#pragma once

namespace pins {

// I2C0: OLED (0x3C) + barometer (0x7C / 0x77) + INA219 (0x40).
constexpr int I2C0_SDA = 21;
constexpr int I2C0_SCL = 22;

// I2C1: the two ADS1115 converters, kept off I2C0 so a stuck OLED or a baro clock-stretch
// can never delay a cell sample.
constexpr int I2C1_SDA = 18;
constexpr int I2C1_SCL = 19;

// ADS1115 conversion-complete (DRDY/DOUT) lines. The dive task owns these: a coil pulse is
// only scheduled while no conversion is integrating (AGENTS.md 8, risk 4).
constexpr int ADS_U1_DRDY = 23;
constexpr int ADS_U2_DRDY = 4;

// Solenoid low-side drive. The 10k gate pulldown holds it off through reset and a watchdog bite.
constexpr int SOLENOID_OUT = 26;

// Arm sequence: two physical buttons, deliberately not reachable from the web page.
constexpr int BTN_ARM_A = 27;
constexpr int BTN_ARM_B = 32; // "Select"

// Manual inject override. Must work with the firmware dead: this is a hardware path.
constexpr int BTN_MANUAL_INJECT = 13;

constexpr int BUZZER_OUT = 25;
constexpr int VIBRATOR_OUT = 33;

// Unassigned pending the GPIO 33 conflict above. Drivers must check < 0 before pinMode.
constexpr int BTN_PAGE = -1;

// External hardware watchdog heartbeat. A dead MCU stops toggling it; the watchdog IC then
// resets the board and holds the solenoid gate low.
constexpr int WDT_HEARTBEAT = 14;

} // namespace pins
