// Bosch BMP085/BMP180 register map, calibration-ROM rule and pressure compensation — pure C++,
// no Arduino, host-testable (test/test_baro/).
//
// Why this lives in lib/core and not in src/sensors/Bmp085.cpp: decision 28's reason again. Every
// constant here feeds the decompression input, and constants that only exist in src/ cannot be
// reached by `pio test -e native`.
//
// PROVENANCE, stated per fact — on 2026-11-20 this project mis-transcribed a baro datasheet and
// convicted a good part of being a clone (AGENTS.md decision 36, risk 21). Read the whole block
// before changing anything below.
//
//   VERIFIED AT SOURCE (BST-BMP180-DS000-09 rev 2.5, text-extracted 2026-11-20):
//     - I2C address: Table 7 "BMP180 addresses" A7..A1 = 1 1 1 0 1 1 1 + W/R -> 7-bit 0x77.
//     - E2PROM calibration 0xAA..0xBF, 11 words, "Every sensor module has individual
//       coefficients", Table 5 order AC1 AC2 AC3 AC4 AC5 AC6 B1 B2 MB MC MD.
//     - Validity rule, verbatim: "The data communication can be checked by checking that none of
//       the words has the value 0 or 0xFFFF."
//     - Control 0xF4: temperature 0x2E (4.5 ms); pressure 0x34/0x74/0xB4/0xF4 for oss 0..3,
//       max conversion 4.5 / 7.5 / 13.5 / 25.5 ms.
//     - Results at 0xF6 (MSB), 0xF7 (LSB), 0xF8 (XLSB, "extend the 16 bit word to up to 19 bits").
//     - id D0h = 55h; pressure range 300 ... 1100 hPa; "1 hPa corresponds to 8.43 m at sea level".
//
//   THE COMPENSATION ALGORITHM — a transcription of the Bosch API as deployed, NOT datasheet
//   text. The DS formula appendix is page images (no text layer; searched: the example constants
//   below appear ZERO times in the DS text) and its body defers to ANSI C "available from Bosch".
//   On 2026-11-20 the first transcription of this algorithm (written partly from memory) was
//   cross-checked line-by-line against the deployed Adafruit BMP085 library (Adafruit-BMP085-
//   Library master, cloned and read 2026-11-20) and found wrong in four places:
//     1. X2 in computeB5 is (mc << 11), not << 16;
//     2. B3 scales by (<< oversampling), not a hard-coded << 2;
//     3. B7 multiplies by (50000UL >> oversampling), not a fixed 50000;
//     4. the B4 path builds X2 from b1 ((b1 * ((B6*B6)>>12)) >> 16), and the B7 overflow branch
//        is p = (B7/B4)*2 for B7 >= 2^31 and (B7*2)/B4 below — a remembered variant had these
//        swapped and the b1 term replaced by ac1.
//   What anchors the arithmetic (and what my remembered "7295/27898/23843" constants never were —
//   those appear in no source and are NOT self-consistent with any published form): the library's
//   debug block, commented "use datasheet numbers!", carries ac1=408 ac2=-72 ac3=-14383 ac4=32741
//   ac5=32757 ac6=23153 b1=6190 b2=4 mc=-8711 md=2868 with UT=27898, UP=23843, oss=0. Under the
//   deployed arithmetic that set yields B5=2400 -> T=(B5+8)>>4=150 = 15.0 C EXACTLY. Under my
//   wrong <<16 it yielded 431.6 C. Self-consistency at the documented result is the discriminator.
//
//   What this file therefore IS: int32/uint32 arithmetic identical to the deployed reference —
//   deliberately NOT widened, because the point is a faithful transcription the bench can
//   cross-check against the Adafruit library on the same physical part. What it is NOT: proven
//   correct for this specific die by anything inside it. test/test_baro pins it against the
//   example set and against a second copy of the reference arithmetic; the acceptance test is on
//   hardware — ~1013 mbar at the surface, moving when the sensor is squeezed (bench 'b'), and
//   agreement with the Adafruit library reading the same module (item 17 closure, 2).
#pragma once

#include <cstdint>

namespace baro {

// ---------------------------------------------------------------------------
// Registers (VERIFIED AT SOURCE above).
// ---------------------------------------------------------------------------
constexpr uint8_t kRegCtrlMeas = 0xF4;
constexpr uint8_t kRegAdcMsb = 0xF6;   // 0xF6 MSB, 0xF7 LSB, 0xF8 XLSB
constexpr uint8_t kRegAdcLsb = 0xF7;
constexpr uint8_t kRegAdcXlsb = 0xF8;
constexpr uint8_t kRegChipId = 0xD0;
constexpr uint8_t kChipIdBmp085180 = 0x55;  // DS: "id D0h 55h"
constexpr uint8_t kRegSoftReset = 0xE0;     // DS lists the register; the 0xB6 sequence is Bosch API, not DS text
constexpr uint8_t kSoftResetSeq = 0xB6;
constexpr uint8_t kCalFirstReg = 0xAA;
constexpr int kCalWords = 11;

constexpr uint8_t kCmdTemperature = 0x2E;
constexpr uint8_t kCmdPressureBase = 0x34;  // + (oss << 6): 0x34 / 0x74 / 0xB4 / 0xF4
constexpr int kOssMax = 3;

// The 11 coefficients in DS Table 5 order, big-endian from 0xAA upward. Typed per the reference
// implementation (ac4..ac6 unsigned); raw bit patterns are what the validity rule inspects.
struct Cal {
    int16_t ac1, ac2, ac3;
    uint16_t ac4, ac5, ac6;
    int16_t b1, b2;
    int16_t mb, mc, md;
};

// The datasheet's own communication check — NOT a value check. Coefficients are per-chip factory
// trim, so comparing AC1 against a remembered number is meaningless: the bench module's AC1 =
// 0x219C (8604) passes this rule and was wrongly called a fault by a rule that does not exist
// (decision 36). Returns the Table-5 index of the first bad word, or -1 when all 11 are usable.
inline int calRomFirstBadWord(const Cal& c) {
    const uint16_t w[kCalWords] = {
        static_cast<uint16_t>(c.ac1), static_cast<uint16_t>(c.ac2), static_cast<uint16_t>(c.ac3),
        c.ac4,                        c.ac5,                        c.ac6,
        static_cast<uint16_t>(c.b1),  static_cast<uint16_t>(c.b2),  static_cast<uint16_t>(c.mb),
        static_cast<uint16_t>(c.mc),  static_cast<uint16_t>(c.md),
    };
    for (int i = 0; i < kCalWords; ++i)
        if (w[i] == 0x0000 || w[i] == 0xFFFF) return i;
    return -1;
}
inline bool calRomPlausible(const Cal& c) { return calRomFirstBadWord(c) < 0; }

// oss -> command byte and max conversion time, both DS-verified. Integer microseconds: the driver
// waits with micros(), not with float ms.
inline uint8_t pressureCommand(int oss) {
    if (oss < 0) oss = 0;
    if (oss > kOssMax) oss = kOssMax;
    return static_cast<uint8_t>(kCmdPressureBase + (oss << 6));
}
inline uint32_t pressureConvUs(int oss) {
    switch (oss < 0 ? 0 : (oss > kOssMax ? kOssMax : oss)) {
        case 0: return 4500;
        case 1: return 7500;
        case 2: return 13500;
        default: return 25500;
    }
}
constexpr uint32_t kTempConvUs = 4500;  // DS: temperature conversion 4.5 ms

// Assemble the pressure result (DS 5.7). At oss>0 the 0xF6/0xF7 pair holds only the TOP 16 bits —
// 0xF8 carries the rest — so a 16-bit-only read must be treated as unscaled, not silently used.
// The deployed reference always reads XLSB and shifts (8-oss); with hasXlsb=false the driver
// states that by returning the raw 16-bit value.
inline uint32_t assembleUp(uint8_t msb, uint8_t lsb, uint8_t xlsb, bool hasXlsb, int oss) {
    if (hasXlsb) {
        const int s = oss < 0 ? 0 : (oss > kOssMax ? kOssMax : oss);
        const uint32_t w = (static_cast<uint32_t>(msb) << 16) | (static_cast<uint32_t>(lsb) << 8) | xlsb;
        return w >> (8 - s);
    }
    return (static_cast<uint32_t>(msb) << 8) | lsb;
}
// UT is an unsigned 16-bit conversion result; the computation widens it to int32.
inline uint16_t assembleUt(uint8_t msb, uint8_t lsb) {
    return static_cast<uint16_t>((static_cast<uint16_t>(msb) << 8) | lsb);
}

// DS pressure range. Outside it the number means nothing and the driver says so, instead of
// reporting a plausible-looking extrapolation (risk 1).
constexpr float kRangeMinPa = 30000.0f;
constexpr float kRangeMaxPa = 110000.0f;
constexpr float kRangeMaxBar = 1.10f;  // what Bmp085::rangeCeilingBar() reports

struct Reading {
    bool converted = false;  // arithmetic completed (cal ROM usable, no divide-by-zero)
    bool inRange = false;    // result inside the DS 300-1100 hPa window
    float tempC = 0.0f;      // from the same UT the pressure was computed from
    float pressurePa = 0.0f;
    int32_t b5 = 0;          // DS intermediate, exposed so the bench can print raw-chain evidence
};

// computeB5 from the deployed reference — mc << 11, NOT << 16 (see PROVENANCE: << 16 turns the
// datasheet's own example into 431.6 C; << 11 yields its documented 15.0 C exactly). A zero
// denominator (possible with a corrupt-but-nonzero ROM) returns 0 rather than trapping;
// calRomFirstBadWord() is the gate that rejects such a ROM anyway.
inline int32_t b5FromUt(const Cal& c, int32_t ut) {
    const int32_t x1 = (ut - static_cast<int32_t>(c.ac6)) * static_cast<int32_t>(c.ac5) >> 15;
    const int32_t den = x1 + static_cast<int32_t>(c.md);
    if (den == 0) return 0;
    const int32_t x2 = (static_cast<int32_t>(c.mc) << 11) / den;
    return x1 + x2;
}

// Pressure + temperature, Bosch API form as deployed (see PROVENANCE — read it before editing;
// four separate transcription faults lived in this function until the 2026-11-20 cross-check).
// int32_t/uint32_t types match the reference deliberately: signed overflow past these formulas is
// reached only by railed or corrupt inputs, the reference behaves the same way, and the guards
// (cal ROM check, b4 == 0 check) reject what they can. oss is clamped to 0..3.
// Shifts on negative intermediates are arithmetic on gcc/clang (xtensa and host alike) — the same
// semantics the reference relies on.
inline Reading compensate(const Cal& c, int32_t ut, uint32_t up, int oss) {
    Reading r;
    if (oss < 0) oss = 0;
    if (oss > kOssMax) oss = kOssMax;
    if (calRomFirstBadWord(c) >= 0) return r;  // a corrupt ROM must not produce a number at all

    const int32_t b5 = b5FromUt(c, ut);
    r.b5 = b5;
    r.tempC = static_cast<float>((b5 + 8) >> 4) / 10.0f;  // (B5+8)>>4 is in 0.1 degC

    const int32_t b6 = b5 - 4000;
    int32_t x1 = (static_cast<int32_t>(c.b2) * ((b6 * b6) >> 12)) >> 11;
    int32_t x2 = (static_cast<int32_t>(c.ac2) * b6) >> 11;
    const int32_t x3 = x1 + x2;
    const int32_t b3 = (((static_cast<int32_t>(c.ac1) * 4 + x3) << oss) + 2) / 4;

    x1 = (static_cast<int32_t>(c.ac3) * b6) >> 13;
    x2 = (static_cast<int32_t>(c.b1) * ((b6 * b6) >> 12)) >> 16;
    const int32_t x4 = ((x1 + x2) + 2) >> 2;
    const uint32_t b4 = (static_cast<uint32_t>(c.ac4) * static_cast<uint32_t>(x4 + 32768)) >> 15;
    if (b4 == 0) return r;

    const uint32_t b7 = (up - static_cast<uint32_t>(b3)) * static_cast<uint32_t>(50000UL >> oss);
    int32_t p;
    if (b7 < 0x80000000u)
        p = static_cast<int32_t>((b7 * 2u) / b4);
    else
        p = static_cast<int32_t>((b7 / b4) * 2u);

    // Non-linear end correction, exactly as deployed: X1=(p>>8)^2, *3038>>16; X2=-7357*p>>16.
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    p += (x1 + x2 + 3791) >> 4;

    r.pressurePa = static_cast<float>(p);
    r.converted = true;
    r.inRange = r.pressurePa >= kRangeMinPa && r.pressurePa <= kRangeMaxPa;
    return r;
}

}  // namespace baro
