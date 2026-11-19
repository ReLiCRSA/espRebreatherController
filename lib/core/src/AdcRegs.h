// ADS1115 register-map constants and field arithmetic — pure C++, no Arduino, host-testable.
//
// This lives in lib/core rather than in src/sensors/Ads1115Bus.h for one reason: the bit positions
// and bit meanings here are load-bearing and were wrong in two separate ways. (1) The rate field
// was taken from bits 10:8 (an ADS1x1x-family guess); on the bench module the power-on config
// register reads 0x8583, which only decodes as the datasheet's documented default (single-shot,
// PGA gain 2, 8 SPS) with the rate in bits 6:4. (2) MODE was read backwards — bit 8 is 1 for SINGLE-SHOT, 0 for continuous, so leaving
// it clear put the converter in continuous mode, which is exactly why the first on-hardware
// conversion test reported "OS bit stuck at 0". Both faults produced plausible-looking numbers and
// neither was visible in the code. Constants the whole cell chain is scaled by belong where
// `pio test -e native` can pin them — test/test_adc/. They are pinned against the word measured on
// the bench module on 2026-11-19 (cfg=0x8583 out of reset) and against the datasheet tables.
#pragma once

#include <cstdint>

namespace adc {

// ---------------------------------------------------------------------------
// ADS1115 config register (DS SBAS444, "Config Register"), address 0x01.
//   15 OS | 14:12 MUX | 11:9 PGA | 8 MODE | 7 DR | 6:4 RATE | 3 CT | 2 POL | 1 TM | 0 NOP
// Field-position discipline: a mask that overlaps another mask silently corrupts a setting, so
// test_config_field_masks_tile_the_register_without_overlap asserts they tile 0xFFFF exactly once.
// ---------------------------------------------------------------------------
constexpr uint16_t kRegConv = 0x00;
constexpr uint16_t kRegConfig = 0x01;
constexpr uint16_t kRegLoThresh = 0x02;
constexpr uint16_t kRegHiThresh = 0x03;
constexpr uint16_t kRegPtr = 0x05;
constexpr uint16_t kRegId = 0x0B;

constexpr uint16_t kOsMask = 0x8000;    // writes 1 to start; reads 0 while converting
constexpr uint16_t kMuxMask = 0x7000;
constexpr uint16_t kPgaMask = 0x0E00;
constexpr uint16_t kModeMask = 0x0100;  // bit 8: **1 = single-shot, 0 = continuous**
constexpr uint16_t kDrMask = 0x0080;    // comparator queue mode (ADS1115)
constexpr uint16_t kRateMask = 0x0070;  // bits 6:4 — NOT 10:8
constexpr uint16_t kCtMask = 0x0008;    // comparator traditional/queue
constexpr uint16_t kPolMask = 0x0004;
constexpr uint16_t kTmMask = 0x0002;    // comparator latch/traditional
constexpr uint16_t kNopMask = 0x0001;   // ALERT active low(1)/high(0)

// MODE written explicitly, always. Relying on the reset value of this bit is how a converter ends
// up in continuous mode, where the OS bit never returns and the mux is locked to one channel —
// which breaks the two-pairs-per-converter scheme entirely (AGENTS.md 2).
constexpr uint16_t kModeSingleShot = kModeMask;
constexpr uint16_t kModeContinuous = 0x0000;

// Mux (bits 14:12). Differential pairs only: each cell pair is wired across AIN0/AIN1 or AIN2/AIN3,
// and continuous mode locks the mux, so a converter feeding two pairs is walked by re-writing this
// field between single-shot conversions.
constexpr uint16_t kMuxAin0Ain1 = 0x0000;
constexpr uint16_t kMuxAin0Ain3 = 0x1000;
constexpr uint16_t kMuxAin1Ain3 = 0x2000;
constexpr uint16_t kMuxAin2Ain3 = 0x3000;
// 100..111 put the part in power-down with a floating input — never a reading, so never selected.
constexpr uint16_t kMuxPowerDown = 0x4000;

// PGA (bits 11:9): full-scale range and gain in one field. The output is always +/-32767, so the
// LSB is FSR/32768. Design gain 16 = PGA 101 = +/-0.256 V FSR, LSB 7.8125 uV (AGENTS.md 2).
constexpr uint16_t kPgaGain23 = 0x0000;  // +/-6.144 V, gain 2/3
constexpr uint16_t kPgaGain1 = 0x0200;   // +/-4.096 V
constexpr uint16_t kPgaGain2 = 0x0400;    // power-on default
constexpr uint16_t kPgaGain4 = 0x0600;
constexpr uint16_t kPgaGain8 = 0x0800;
constexpr uint16_t kPgaGain16 = 0x0A00;  // design gain
constexpr uint16_t kPgaDesign = kPgaGain16;

// RATE (bits 6:4) for the ADS1115: 8/16/32/64/128/250/475/860 SPS.
constexpr uint16_t kRate8 = 0x0000;      // power-on default
constexpr uint16_t kRate16 = 0x0010;
constexpr uint16_t kRate32 = 0x0020;     // 31.25 ms/conversion — design rate (16 SPS per cell)
constexpr uint16_t kRate64 = 0x0030;
constexpr uint16_t kRate128 = 0x0040;
constexpr uint16_t kRate250 = 0x0050;
constexpr uint16_t kRate475 = 0x0060;
constexpr uint16_t kRate860 = 0x0070;
constexpr uint16_t kRateDesign = kRate32;

// Measured power-on state of the bench module, 2026-11-19, equal to the datasheet's documented
// default. It is the anchor the field meanings are checked against: it decodes as single-shot,
// PGA gain 2 (+/-2.048 V), 8 SPS, mux AIN0/AIN1, not busy. If a change to a mask stops that decode, the mask
// moved or the meaning flipped.
constexpr uint16_t kPowerOnConfig = 0x8583;

// The ADS1115's documented ID register value. An ADS1015 answers 0x05 at the same address with the
// same register map, which is why identity is checked by *measured conversion period* instead: the
// RATE field means 32 SPS on an ADS1115 and 3200 SPS on an ADS1015, 100x apart.
constexpr uint8_t kIdAds1115 = 0x85;
constexpr uint8_t kIdAds1015 = 0x05;

// ---------------------------------------------------------------------------
// Decoding. Every helper takes the **config word**, never a pre-shifted field value: mixing the two
// is how the rate bug hid (a pre-shifted kRate32 passed to a field-value function decoded as 8 SPS,
// a perfectly ordinary-looking rate).
// ---------------------------------------------------------------------------
constexpr uint16_t muxField(uint16_t cfg) { return (cfg & kMuxMask) >> 12; }
constexpr uint16_t pgaField(uint16_t cfg) { return (cfg & kPgaMask) >> 9; }
constexpr uint16_t rateField(uint16_t cfg) { return (cfg & kRateMask) >> 4; }
constexpr bool isContinuous(uint16_t cfg) { return (cfg & kModeMask) == 0; }
constexpr bool isSingleShot(uint16_t cfg) { return (cfg & kModeMask) != 0; }
constexpr bool isConverting(uint16_t cfg) { return (cfg & kOsMask) == 0; }
constexpr bool isPowerDownMux(uint16_t cfg) { return muxField(cfg) >= 4; }

// Full-scale range in volts for a PGA field value (the 110/111 encodings are analogous-but-unused
// and report 0.256 V like 101, per the datasheet).
constexpr float kFsrVolts[8] = {6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f, 0.256f, 0.256f};
constexpr float kGain[8] = {0.6666667f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 16.0f, 16.0f};
constexpr int kSps[8] = {8, 16, 32, 64, 128, 250, 475, 860};

inline constexpr float fsrVoltsField(uint16_t field) { return kFsrVolts[field & 7]; }
inline constexpr float gainField(uint16_t field) { return kGain[field & 7]; }

// Volts per output code. This is what a raw cell code is multiplied by, so getting it wrong scales
// every ppO2 by 16x and still looks like a plausible cell signal.
inline constexpr float lsbVoltsField(uint16_t field) { return fsrVoltsField(field) / 32768.0f; }

inline constexpr int spsField(uint16_t field) { return kSps[field & 7]; }

// Conversion time in microseconds. The coil-pulse interlock (AGENTS.md 8) is sized on this: a pulse
// must not overlap an integrating converter. It is the *datasheet* figure — see the measured table
// below, which is what the first module on the bench actually does.
inline constexpr uint32_t convUsField(uint16_t field) {
    const int sps = spsField(field);
    return sps > 0 ? static_cast<uint32_t>(1000000u / static_cast<uint32_t>(sps)) : 0u;
}

// ---------------------------------------------------------------------------
// Measured rate table of the first module on the bench (2026-11-19, bench command 'd', one measured
// conversion period per RATE field):
//
//   field      0      1      2      3      4      5      6      7
//   datasheet  8     16     32     64    128    250    475    860  SPS
//   measured  8.1    8.1   16.1   16.1   31.9   31.9   63.7   63.7 SPS
//
// Reproduced on a second run; each adjacent field pair agreed to within 0.5%, which is what shows
// the field is quantised rather than the measurement being noisy.
//
// The part ignores bit 4 of the rate field and implements only four rates — 8/16/32/64 SPS, selected
// by bits 6:5 — and it never reaches the datasheet's 860 SPS. Together with the ID register reading
// 0x00 and the pointer register reading 0x00, that is the signature of a marked-not-TI die; it is
// not a genuine ADS1115's register map. Recorded here as measurement, not as a claim about which
// clone it is.
//
// The firmware therefore derives conversion timing from measurement at bring-up, not from this file.
// The datasheet table above stays normative for what the design *asks for*; the measured table is
// what the sample rate and the coil/ADC interlock window are actually built on.
constexpr int kMeasuredSpsBenchModule[8] = {8, 8, 16, 16, 32, 32, 63, 63};

// Percent difference between a measured conversion period and the datasheet's for the same field.
// Positive = slower than the datasheet. The threshold that matters is documented at the use site in
// src/main.cpp: a converter whose rate table is not the ADS1115's must be said out loud, because the
// sample rate the control loop and the interlock are designed around is then not what was designed.
inline int rateErrorPercent(uint32_t measuredUs, uint32_t nominalUs) {
    if (nominalUs == 0) return 0;
    const int64_t diff = static_cast<int64_t>(measuredUs) - static_cast<int64_t>(nominalUs);
    return static_cast<int>(diff * 100 / static_cast<int64_t>(nominalUs));
}

// Config-word-level accessors used by the dive loop.
inline constexpr float lsbVolts(uint16_t cfg) { return lsbVoltsField(pgaField(cfg)); }
inline constexpr float fsrVolts(uint16_t cfg) { return fsrVoltsField(pgaField(cfg)); }
inline constexpr int sps(uint16_t cfg) { return spsField(rateField(cfg)); }
inline constexpr uint32_t convUs(uint16_t cfg) { return convUsField(rateField(cfg)); }

// The config word the design runs each converter at: single-shot (explicitly — see kModeSingleShot),
// gain 16, 32 SPS, one differential pair, ALERT left in its default configuration. OS is included,
// so writing this word both configures the part and starts the conversion.
inline constexpr uint16_t designConfig(uint16_t mux = kMuxAin0Ain1) {
    return static_cast<uint16_t>(kOsMask | kModeSingleShot | mux | kPgaDesign | kRateDesign);
}

// A galvanic cell in air produces ~9-13 mV (AGENTS.md 2). At the design gain that is the number of
// output codes asserted in test/test_adc; it also says how much of the range a real cell uses,
// which is what makes a missing bias network detectable — a railed differential pair sits at full
// scale, not at ~1660 counts (AGENTS.md 19 risk 6).
inline constexpr int16_t codesForMillivolts(float mV, uint16_t cfg = designConfig()) {
    float codes = (mV / 1000.0f) / lsbVolts(cfg);
    if (codes > 32767.0f) codes = 32767.0f;
    if (codes < -32768.0f) codes = -32768.0f;
    return static_cast<int16_t>(codes < 0.0f ? codes - 0.5f : codes + 0.5f);
}

// Raw code -> millivolts. Sign is preserved: galvanic O2 polarity depends on which lead is which,
// and the per-channel `invert` flag is resolved on the bench (AGENTS.md 2).
inline constexpr float millivoltsOf(int16_t raw, uint16_t cfg = designConfig()) {
    return static_cast<float>(raw) * lsbVolts(cfg) * 1000.0f;
}

} // namespace adc
