// INA219 register map, config fields and conversion math (AGENTS.md 2 "Sense: V_BAT ... INA219
// (0x40) on I2C0", M3). Pure C++ so test/test_power pins it (decision 28's pattern).
//
// PROVENANCE (decision 37 — when a part's real-world behaviour lives in deployed vendor code,
// that code is the reference, and the transcription is anchored on its own worked numbers):
// transcribed from adafruit/Adafruit_INA219 (master, read in full 2026-11-20): the register
// addresses, every config field mask and field value below is byte-for-byte that library's
// INA219_REG_* / INA219_CONFIG_* constants; the three calibration presets (cal word, current
// divider, power multiplier) and the raw->engineering conversions are its init()/
// setCalibration_*/getX_raw()/getX_mA()/getPower_mW()/getBusVoltage_V()/getShuntVoltage_mV()
// bodies, including its documented formulas (Cal = trunc(0.04096 / (Current_LSB x RSHUNT));
// PowerLSB = 20 x Current_LSB). The TI INA219 datasheet was NOT in hand while writing this —
// so every claim here is "the deployed library says", not "the datasheet says", and the first
// fitted module gets a meter cross-check at the bench before any current figure is trusted
// (the shunt-ohms assumption below is the one design-specific fact this transcription carries).
//
// DESIGN-SPECIFIC ASSUMPTION, stated because it changes every current/power number: these
// presets assume a 0.1 ohm sense resistor (the library says so in its own @note). The AGENTS.md
// BOM names the INA219 breakout (Adafruit #904 class, 0.1 ohm) for battery V/I; the 10 mOhm
// coil shunt is a different part, read by the ADS1115, never by this driver. When a board with
// a different sense resistor is fitted, the cal/divider/multiplier triple must be re-derived
// from the formula above — the test pins the formula precisely so that re-derivation is a
// calculation, not a legend hunt.
//
// The deployed library also re-writes the CAL register before every CURRENT/POWER read, with
// an explicit comment: a sharp load drop can reset the INA219, clearing CAL, which zeroes
// CURRENT and POWER silently. A solenoid pulsing ~1 A off the same rail is exactly that sharp
// load — so the driver keeps that re-write (src/sensors/Ina219.h), and it is recorded here as
// load-bearing, not as library noise.
#pragma once

#include <stdint.h>

namespace pwr {

// Default I2C address 0x40 (A0+A1 to GND). The full strap set per the deployed library's
// INA219_CALC_ADDRESS comment: 0x40 | A1x4 | A0, with A=GND 00, V+ 01 (SDA/SCL straps not
// implemented by the library). 0x40 is the design's fixed strap.
constexpr uint8_t kAddr7 = 0x40;

// Registers (deployed library INA219_REG_*).
constexpr uint8_t kRegConfig = 0x00;
constexpr uint8_t kRegShuntVoltage = 0x01;
constexpr uint8_t kRegBusVoltage = 0x02;
constexpr uint8_t kRegPower = 0x03;
constexpr uint8_t kRegCurrent = 0x04;
constexpr uint8_t kRegCalibration = 0x05;

// Config register fields (deployed library masks, verbatim): bit 15 RST, 14:13 bus range,
// 12:11 gain, 10:7 bus ADC, 6:3 shunt ADC, 2:0 mode.
constexpr uint16_t kCfgReset = 0x8000;
constexpr uint16_t kCfgMaskBusRange = 0x2000;
constexpr uint16_t kCfgMaskGain = 0x1800;
constexpr uint16_t kCfgMaskBadcRes = 0x0780;
constexpr uint16_t kCfgMaskSadcRes = 0x0078;
constexpr uint16_t kCfgMaskMode = 0x0007;

// Field values (deployed library enums; only the ones this design uses are spelled out fully,
// the tables are complete enough to decode any word the chip returns).
constexpr uint16_t kCfgBusRange16V = 0x0000;
constexpr uint16_t kCfgBusRange32V = 0x2000;
constexpr uint16_t kCfgGain1x40mV = 0x0000;
constexpr uint16_t kCfgGain2x80mV = 0x0800;
constexpr uint16_t kCfgGain4x160mV = 0x1000;
constexpr uint16_t kCfgGain8x320mV = 0x1800;
constexpr uint16_t kCfgBadc12Bit = 0x0180;
constexpr uint16_t kCfgSadc12Bit1s532us = 0x0018;
constexpr uint16_t kCfgModePowerDown = 0x00;
constexpr uint16_t kCfgModeSandVoltContinuous = 0x07;

// Field access: position derived from each mask, so a typo in a mask (the exact defect class
// that bit the ADS1115 RATE field — overlapping fields, plausible readback) is a test failure
// rather than a bench chase.
inline uint8_t fieldPos(uint16_t mask) {
    uint8_t p = 0;
    while (mask && !(mask & 1u)) { mask >>= 1; ++p; }
    return p;
}
inline uint16_t getField(uint16_t word, uint16_t mask) { return (word & mask) >> fieldPos(mask); }

// ---------------- calibration presets (deployed library, 0.1 ohm shunt assumed) ----------------

struct Preset {
    uint16_t cal;          // CAL register word
    uint32_t divider_mA;   // mA per raw Current word = raw / divider
    float mult_mW;         // mW per raw Power word
    uint16_t config;       // config word written for this preset
};

// setCalibration_32V_2A: LSB 100 uA, Cal = trunc(0.04096/(0.0001 x 0.1)) = 4096 (0x1000),
// divider 10, power multiplier 2 (PowerLSB 2 mW), config 32V range + gain 8 + 12-bit both ADCs
// + shunt+bus continuous. This is the design's battery-rail preset (1S2P pack, 3.4-4.2 V,
// peak currents ~1 A — comfortably inside the 3.2 A counter-overflow point, and far inside
// the 32 V bus range the preset assumes).
constexpr uint16_t kConfig32V2A = 0x2000u | 0x1800u | 0x0180u | 0x0018u | 0x0007u;  // = 0x399F

constexpr Preset kPreset32V2A = {4096u, 10u, 2.0f, kConfig32V2A};

// setCalibration_32V_1A: LSB 40 uA, Cal = 10240 (0x2800), divider 25, multiplier 0.8 mW.
constexpr Preset kPreset32V1A = {10240u, 25u, 0.8f, kConfig32V2A};

// setCalibration_16V_400mA: LSB 50 uA, Cal = 8192 (0x2000), divider 20, multiplier 1.0 mW,
// 16 V range + gain 1.
constexpr uint16_t kConfig16V400mA = 0x0000u | 0x0000u | 0x0180u | 0x0018u | 0x0007u;  // = 0x019F
constexpr Preset kPreset16V400mA = {8192u, 20u, 1.0f, kConfig16V400mA};

// The library's documented formula, kept as a function so a future non-0.1-ohm shunt is derived
// from it rather than from a remembered number (risk 21). Cal = trunc(0.04096 / (I_amps x R_ohms)).
// Exact integer arithmetic on purpose: with floats, 0.1 is unrepresentable and the documented
// exact case 0.04096/(0.0001 x 0.1) comes out 4095.9998... and truncates to 4095 — measured
// 2026-11-20 when the test caught its own anchor. Inputs in nanoamps and nanoohms;
// Cal = trunc(4.096e16 / (nA x nohm)).
inline uint32_t calRegister(uint64_t currentLsbNanoamps, uint64_t shuntNanoohms) {
    if (!currentLsbNanoamps || !shuntNanoohms) return 0;
    return static_cast<uint32_t>(40960000000000000ull / (currentLsbNanoamps * shuntNanoohms));
}

// ---------------- raw -> engineering (deployed library conversions, verbatim behaviour) ----------------

// BUS_SHUNT_VOLTAGE register (0x01): 16-bit signed, LSB 10 uV, full scale +/-327.67 mV.
inline float shuntmV(int16_t raw) { return raw * 0.01f; }

// BUS_VOLTAGE register (0x02): value>>3 drops CNVR+OVF flags, LSB 4 mV. The deployed library
// does (int16_t)((value >> 3) * 4) then * 0.001 — reproduced exactly, flag bits included
// (test asserts what the reference does for a set-flag word rather than pretending it cannot).
inline float busVolts(uint16_t word) {
    return static_cast<int16_t>((word >> 3) * 4u) * 0.001f;
}

// CURRENT register (0x04): signed raw, /divider_mA -> mA.
inline float current_mA(int16_t raw, uint32_t divider_mA) {
    return static_cast<float>(raw) / static_cast<float>(divider_mA);
}

// POWER register (0x03): raw *mult_mW -> mW.
inline float power_mW(int16_t raw, float mult_mW) { return static_cast<float>(raw) * mult_mW; }

// Plausibility for this design's battery rail: 1S2P LiPo sits 3.0-4.25 V by spec; the INA219
// ACKing while its bus-voltage register reads implausible (railed, zero, or nonsense) is an
// INA219 that cannot be trusted for V_BAT — the arming gate needs V_BAT (AGENTS.md 13). These
// bounds are the identity check's window, deliberately wider than the arming thresholds: the
// check asks "is this sensor measuring", not "is the pack charged".
constexpr float kRailPlausibleMinV = 2.5f;
constexpr float kRailPlausibleMaxV = 5.5f;

}  // namespace pwr
