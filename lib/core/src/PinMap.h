// GPIO capability table + role assignment for the ESP32 classic (WROOM-32) build — pure C++, no
// Arduino, host-testable. src/config/Pins.h is a thin alias of the constants here, exactly like
// Ads1115Bus.h is an alias of AdcRegs.h (decision 28): a table that only exists in src/ cannot be
// asserted, and this table has already been wrong twice.
//
// What was wrong, and why it is asserted now (test/test_pins/):
//   * AGENTS.md 3 assigned **GPIO 33 to both the vibration motor and the Page button**. That is not
//     a code bug, it is a defect in a table nobody could test — and a shared pin in a life-support
//     device fails in a way nobody sees until the motor back-biases a button read. Resolved 2026-11-19:
//     33 stays the Page button (input, and 33 has an internal pull-up, unlike 34-39), the vibrator
//     moves to GPIO 16 (decision 32).
//   * "avoid 16/17 (UART0 on most devkits)" is **false on this board**. The console is UART0 on
//     GPIO1(TX)/GPIO3(RX) — the DevKit-V1 CP2102 is wired there, and the core's own variant header
//     for this board says `TX = 1; RX = 3`. Bench command `g` measures it: those two pads come up
//     claimed by UART0, and the boot log and bench RX both work on them. 16/17 are therefore real
//     pads, and after the map below they are the *only* free, non-strapping, output-capable ones.
//   * The "free pins" list people compute from the die (0..39) is an illusion on this hardware.
//     Measured 2026-11-19 with bench command `g`: **GPIO 24 and 28..31 do not exist on the
//     ESP32-D0WD-V3 die at all** (the core's periman answers "Invalid pin" for them), and **20, 37
//     and 38** are real die pads that the ESP32-WROOM-32 module does not bond out. 6..11 are inside
//     the module's SPI flash path. Counting pins off the die yields a phantom spare and then a
//     dead-GPIO chase that costs an evening.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pins {

constexpr int kGpioCount = 40;

// Capability of one GPIO as fitted: ESP32-D0WD-V3 die in an ESP32-WROOM-32 module on a
// DevKit-V1-class board. "Bonded" means it reaches the board headers, which is the only thing a
// wiring diagram can act on.
enum Cap : uint32_t {
    kBonded  = 1u << 0,  // broken out on the module and reachable on the board headers
    kOutput  = 1u << 1,  // can drive a load (34..39 cannot — they are input-only)
    kInput   = 1u << 2,
    kPull    = 1u << 3,  // internal pull-up/pull-down available. Measured: 16/17 do, 34..39 do not
                         // (the driver answers "GPIO number error") — a button on 34..39 needs an
                         // external resistor, and that is a BOM item, not a detail.
    kStrap   = 1u << 4,  // level at reset selects boot mode: never load it without thinking
    kAdc1    = 1u << 5,  // ADC1: analog reads still work with WiFi on
    kAdc2    = 1u << 6,  // ADC2: analog reads are impossible while WiFi is active
    kFlash   = 1u << 7,  // inside the module's SPI flash bus: never usable as GPIO
    kConsole = 1u << 8,  // this board's UART0 to the CP2102: touching it kills the console
    kBoard   = 1u << 9,  // the devkit already has something on it (LED on 2)
    kDieOnly = 1u << 10, // a real pad on the die, but this module does not bond it out
};

// Two different reasons a pin cannot be reached, and they were previously conflated. Measured 2026-11-19
// with bench command `g`: the core's periman rejects GPIO **24 and 28..31** as "Invalid pin" — they do
// not exist on the ESP32-D0WD-V3 die at all — while **20, 37 and 38** are pads on the die that the
// ESP32-WROOM-32 module simply does not bring to its pads. Same practical answer (wire nothing to them),
// different claim, and a wiring diagram should say which.

// clang-format off
//        0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20  21  22  23  24  25  26  27  28  29  30  31  32  33  34  35  36  37  38  39
constexpr uint32_t kCaps[kGpioCount] = {
    /*0*/ kBonded|kOutput|kInput|kPull|kStrap|kAdc2,
    /*1*/ kBonded|kOutput|kInput|kPull|kConsole,
    /*2*/ kBonded|kOutput|kInput|kPull|kStrap|kAdc2|kBoard,
    /*3*/ kBonded|kOutput|kInput|kPull|kConsole,
    /*4*/ kBonded|kOutput|kInput|kPull|kAdc2,
    /*5*/ kBonded|kOutput|kInput|kPull|kStrap,
    /*6*/ kFlash, /*7*/ kFlash, /*8*/ kFlash, /*9*/ kFlash, /*10*/ kFlash, /*11*/ kFlash,
    /*12*/ kBonded|kOutput|kInput|kPull|kStrap|kAdc2,
    /*13*/ kBonded|kOutput|kInput|kPull|kAdc2,
    /*14*/ kBonded|kOutput|kInput|kPull|kAdc2,
    /*15*/ kBonded|kOutput|kInput|kPull|kStrap|kAdc2,
    /*16*/ kBonded|kOutput|kInput|kPull,     // free; also UART2 RX. On a WROVER module these two
    /*17*/ kBonded|kOutput|kInput|kPull,     // pads are PSRAM CS/clock — this build is WROOM.
    /*18*/ kBonded|kOutput|kInput|kPull,
    /*19*/ kBonded|kOutput|kInput|kPull,
    /*20*/ kDieOnly,                          // die pad, not bonded out on ESP32-WROOM-32
    /*21*/ kBonded|kOutput|kInput|kPull,
    /*22*/ kBonded|kOutput|kInput|kPull,
    /*23*/ kBonded|kOutput|kInput|kPull,
    /*24*/ 0,                                 // no such pad on this die (periman: "Invalid pin")
    /*25*/ kBonded|kOutput|kInput|kPull|kAdc2,
    /*26*/ kBonded|kOutput|kInput|kPull|kAdc2,
    /*27*/ kBonded|kOutput|kInput|kPull|kAdc2,
    /*28*/ 0, /*29*/ 0, /*30*/ 0, /*31*/ 0,   // no such pads on this die
    /*32*/ kBonded|kOutput|kInput|kPull|kAdc1, // also XTAL_32K_XP: a 32.768 kHz crystal would take it
    /*33*/ kBonded|kOutput|kInput|kPull|kAdc1, // also XTAL_32K_XN
    /*34*/ kBonded|kInput|kAdc1,
    /*35*/ kBonded|kInput|kAdc1,
    /*36*/ kBonded|kInput|kAdc1,               // SENSOR_VP
    /*37*/ kDieOnly,                           // die pads this module does not bond out
    /*38*/ kDieOnly,
    /*39*/ kBonded|kInput|kAdc1,               // SENSOR_VN
};
// clang-format on

inline uint32_t caps(int gpio) {
    return (gpio >= 0 && gpio < kGpioCount) ? kCaps[gpio] : 0u;
}
inline bool bonded(int gpio) { return (caps(gpio) & kBonded) != 0u; }
inline bool outputCapable(int gpio) { return (caps(gpio) & kOutput) != 0u; }
inline bool inputCapable(int gpio) { return (caps(gpio) & kInput) != 0u; }
inline bool pullCapable(int gpio) { return (caps(gpio) & kPull) != 0u; }
inline bool strapping(int gpio) { return (caps(gpio) & kStrap) != 0u; }
inline bool consolePad(int gpio) { return (caps(gpio) & kConsole) != 0u; }
inline bool flashPad(int gpio) { return (caps(gpio) & kFlash) != 0u; }
inline bool dieOnlyPad(int gpio) { return (caps(gpio) & kDieOnly) != 0u; }
// No such pad on the ESP32-D0WD-V3 die: the Arduino core rejects it outright.
inline bool noSuchPad(int gpio) { return caps(gpio) == 0u; }

// ---------------------------------------------------------------------------
// The assignment. These constants are the single source of truth: src/config/Pins.h includes
// this header rather than carrying its own numbers, and the table below is built from them, so a
// pin claimed twice is one row with a duplicate value that test/test_pins/ fails on.
// ---------------------------------------------------------------------------
constexpr int I2C0_SDA = 21;            // OLED + baro + INA219
constexpr int I2C0_SCL = 22;
constexpr int I2C1_SDA = 18;            // the two ADS1115 converters, on their own bus
constexpr int I2C1_SCL = 19;
constexpr int ADS_U1_DRDY = 23;         // conversion-complete lines (open item 14: not relied on yet)
constexpr int ADS_U2_DRDY = 4;
constexpr int SOLENOID_OUT = 26;        // 10k gate pulldown; fails closed through reset and a bite
constexpr int BTN_ARM_A = 27;
constexpr int BTN_ARM_B = 32;           // "Select"
constexpr int BTN_PAGE = 33;            // "Page" — was unassigned while it collided with the motor
constexpr int BTN_MANUAL_INJECT = 13;   // hardware override path, works with firmware dead
constexpr int BUZZER_OUT = 25;
constexpr int VIBRATOR_OUT = 16;        // moved off 33 (decision 32). Drive it through a transistor
                                        // with a gate pulldown: an internal pull is not a fail-safe.
constexpr int WDT_HEARTBEAT = 14;       // external watchdog: a dead MCU stops toggling it

// The spare. After the map above this is the only free, non-strapping, output-capable pad on the
// module, so it must be spent deliberately (a SPI display clock is the likely claimant, see
// AGENTS.md 20 open item 15) and not found by accident during a bring-up.
constexpr int SPARE_OUT = 17;

// One row per function; `role` is what the wiring diagram and the console call it.
struct Assignment {
    const char* role;
    int gpio;
    bool needsOutput;  // false = input (button / DRDY sense)
};

constexpr Assignment kAssignments[] = {
    {"I2C0_SDA", I2C0_SDA, true},
    {"I2C0_SCL", I2C0_SCL, true},
    {"I2C1_SDA", I2C1_SDA, true},
    {"I2C1_SCL", I2C1_SCL, true},
    {"ADS_U1_DRDY", ADS_U1_DRDY, false},
    {"ADS_U2_DRDY", ADS_U2_DRDY, false},
    {"SOLENOID_OUT", SOLENOID_OUT, true},
    {"BTN_ARM_A", BTN_ARM_A, false},
    {"BTN_ARM_B", BTN_ARM_B, false},
    {"BTN_PAGE", BTN_PAGE, false},
    {"BTN_MANUAL_INJECT", BTN_MANUAL_INJECT, false},
    {"BUZZER_OUT", BUZZER_OUT, true},
    {"VIBRATOR_OUT", VIBRATOR_OUT, true},
    {"WDT_HEARTBEAT", WDT_HEARTBEAT, true},
};
constexpr size_t kAssignmentCount = sizeof(kAssignments) / sizeof(kAssignments[0]);

inline const Assignment* findAssignment(int gpio) {
    for (size_t i = 0; i < kAssignmentCount; ++i)
        if (kAssignments[i].gpio == gpio) return &kAssignments[i];
    return nullptr;
}

// Why a pad cannot be used, or nullptr if nothing in this table rules it out. Returning the reason
// (rather than a bare bool) is what lets test/test_pins/ assert the *claim*, which is the part that
// was wrong: "16/17 are UART0 on most devkits" and "there are free pins at 20/24/28..31" were both
// assertions nobody could check.
inline const char* unusableReason(int gpio) {
    if (gpio < 0 || gpio >= kGpioCount) return "not a GPIO";
    const uint32_t c = kCaps[gpio];
    if (c == 0u) return "no such pad on the ESP32-D0WD-V3 die";
    if (c & kDieOnly) return "die pad, not bonded out on ESP32-WROOM-32";
    if (c & kFlash) return "inside the module's SPI flash path";
    if (c & kConsole) return "this board's UART0 console to the CP2102";
    if (findAssignment(gpio)) return "assigned in this table";
    if (c & kStrap) return "boot strapping pin";
    if (c & kBoard) return "devkit hardware already on it";
    return nullptr;
}

// Genuinely spare: reachable, unclaimed, not strapping, not flash, not the console.
inline bool isSpare(int gpio) { return unusableReason(gpio) == nullptr; }

}  // namespace pins
