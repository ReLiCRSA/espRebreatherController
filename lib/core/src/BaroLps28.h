// LPS28DFW register map, dual full-scale config and scaling (AGENTS.md 2, decision 39).
//
// TIER-2 depth sensor: fallback when the MS5837-30BA is not fitted or fails its identity chain.
// Mode 2 measures 260-4060 hPa ABSOLUTE — a ~31 m water column at this bench, NOT the "40 m
// underwater" the breakout's product page claims, which treats the absolute full scale as gauge
// (4060 hPa abs minus ~978 mbar surface is ~30.7 m of salt — the computed figure from
// depth::ceilingMeters() is what every banner prints; decision 38: derive, never recite).
//
// PROVENANCE — read at source (decision 37, risk 21):
//  - STMicroelectronics vendor driver, Sensors Software Solution Team, (c) 2024:
//    lps28dfw_reg.h / lps28dfw_reg.c from github.com/STMicroelectronics/lps28dfw (master),
//    fetched and read 2026-11-20. Every register address, bit position, the WHO_AM_I value, the
//    ODR/AVG encodings and the scaling divisors below are transcribed from that code — ST's own,
//    not a third-party port. The vendor .c states its sensitivity directly: 1048576 = 4096*256
//    (FS 1260 hPa), 524288 = 2048*256 (FS 4060 hPa); temperature LSB/100 C.
//  - DS13317 (LPS28DFW DS) title: "Dual full-scale, 1260 hPa and 4060 hPa, absolute digital
//    output barometer with water-resistant package" — the range behind kFsMinBar and the two
//    full-scale values; the vendor enums LPS28DFW_1260hPa / _4060hPa corroborate. The DS PDF
//    would not re-fetch on this bench this week (ST blocks automated fetches); nothing here
//    rests on remembered DS text — the vendor driver carries the tables and is the algorithm
//    reference anyway (decision 37).
//  - I2C address: ST documents bits 101110xb — 7-bit 0x5C (SA0=0) / 0x5D (SA0=1); the vendor
//    header quotes the same as 8-bit bytes (0xB8 write / 0xB9 read for SA0=0). Quote the bits,
//    never do arithmetic on the quoted bytes (decision 36 — that is how 0x3E/0x3B were born).
//  - PART UNPROCURED as of 2026-11-20: written-but-never-measured, same honesty as the MS5837
//    side. Arrival acceptance is bench 'b': WHO_AM_I, FS_MODE writeback, a real mbar at the
//    bench (Stockholm-station band, named place/hour/source, risk 21) and movement under a
//    port squeeze.
//
// This part has NO factory PROM to CRC — identity is weaker than the MS5837 chain by design of
// the silicon, so it is established the way decision 29 establishes everything else: by active
// capability. WHO_AM_I 0xB4, the FS_MODE write actually latching (the dual-FS capability IS the
// part — an LPS22HH or LPS25HW at a wrong address will not present it), and one real conversion
// landing inside the configured full-scale window.
#pragma once

#include <stdint.h>

namespace lps28 {

// ST bits 101110xb: 7-bit 0x5C with SA0=0 (the Adafruit #6067 default) / 0x5D with SA0=1
// (solder jumper on that board). Both probed, one device — the OLED 0x3C/0x3D precedent.
constexpr uint8_t kAddr7 = 0x5C;
constexpr uint8_t kAddr7Alt = 0x5D;

// WHO_AM_I (0x0F) content — vendor header LPS28DFW_ID. Gated, unlike the MS5837's unreliable
// ID-register story on clones: this is ST's own documented constant and the vendor driver gates
// on it too.
constexpr uint8_t kWhoAmI = 0xB4;

// ---------------- registers (vendor map) ----------------
constexpr uint8_t kRegInterruptCfg = 0x0B;
constexpr uint8_t kRegWhoAmI = 0x0F;
constexpr uint8_t kRegCtrl1 = 0x10;  // AVG[2:0], ODR[6:3]
constexpr uint8_t kRegCtrl2 = 0x11;  // ONESHOT[0], SWRESET[2], BDU[3], EN_LPFP[4], LPF_CFG[5], FS_MODE[6], BOOT[7]
constexpr uint8_t kRegCtrl3 = 0x12;  // IF_ADD_INC[0]
constexpr uint8_t kRegStatus = 0x27;  // P_DA[0], T_DA[1], P_OR[4], T_OR[5]
constexpr uint8_t kRegPressXl = 0x28;
constexpr uint8_t kRegPressL = 0x29;
constexpr uint8_t kRegPressH = 0x2A;
constexpr uint8_t kRegTempL = 0x2B;
constexpr uint8_t kRegTempH = 0x2C;

constexpr uint8_t kBitOneshot = 0;
constexpr uint8_t kBitSwReset = 2;
constexpr uint8_t kBitBdu = 3;
constexpr uint8_t kBitFsMode = 6;  // CTRL_REG2: 0 = FS 1260 hPa, 1 = FS 4060 hPa
constexpr uint8_t kBitBoot = 7;
constexpr uint8_t kBitPData = 0;   // STATUS: pressure data ready
constexpr uint8_t kOdrShift = 3;   // CTRL_REG1 ODR field at [6:3]
constexpr uint8_t kOdrMask = 0x0Fu;
constexpr uint8_t kAvgMask = 0x07u;

// ODR encodings (vendor enum): 0 power-down, 1 one-shot, 2..8 = 1/10/25/50/75/100/200 Hz.
enum Odr : uint8_t { kOdrPowerDown = 0, kOdrOneShot = 1, kOdr1Hz = 2, kOdr10Hz = 3,
                     kOdr25Hz = 4, kOdr50Hz = 5, kOdr75Hz = 6, kOdr100Hz = 7, kOdr200Hz = 8 };

// The dive cadence: continuous 10 Hz, AVG field 0 (4-sample average; the LPS2x averaging works
// at fixed ODR — vendor mode struct pairs AVG with ODR). 1 Hz depth sampling never needs more.
constexpr uint8_t kDesignOdr = kOdr10Hz;
constexpr uint8_t kDesignAvg = 0;  // 4_AVG per the vendor enum

// ---------------- ranges ----------------
// Both modes bottom at 260 hPa (DS title range); full scales 1260 / 4060 hPa absolute.
constexpr float kFsMinBar = 0.26f;
constexpr float kFs1260Bar = 1.26f;
constexpr float kFs4060Bar = 4.06f;

// Ceiling by FS_MODE bit value — what Barometer::rangeCeilingBar() reports, so Depth clamps and
// the BARO_RANGE banner quote the mode actually latched, never the mode we hoped we wrote.
inline float rangeCeilingBar(bool fsMode2) { return fsMode2 ? kFs4060Bar : kFs1260Bar; }
inline bool inRangeBar(float ambBar, bool fsMode2) {
    return ambBar >= kFsMinBar && ambBar <= rangeCeilingBar(fsMode2);
}

// ---------------- scaling (vendor formulas, bit-exact) ----------------
// The vendor assembles 24-bit two's-complement XL/L/H then multiplies by 256 (left-aligned
// int32) before dividing. This assembles it left-aligned directly; the bit pattern is identical
// for every raw inside either full-scale window (max in-range words: 0x4EC000 at FS 1260,
// 0x7EE000 at FS 4060 — both below 0x800000), and it stays defined arithmetic outside them too,
// where some vendor-style assembly paths run into signed overflow (raws with bit 23 set
// multiplied by 256 exceed int32 range).

// 24-bit two's complement from XL/L/H, left-aligned in int32 (vendor 'raw * 256' form).
inline int32_t pressureRawAligned(uint8_t xl, uint8_t l, uint8_t h) {
    return (static_cast<int32_t>(static_cast<int8_t>(h)) << 24) |
           (static_cast<int32_t>(l) << 16) |
           (static_cast<int32_t>(xl) << 8);
}

// hPa. Divisors are the vendor's, verbatim: 4096 LSB/hPa * 256 (FS 1260), 2048 * 256 (FS 4060).
inline float hPaFromRaw1260(int32_t rawAligned) {
    return static_cast<float>(rawAligned) / 1048576.0f;
}
inline float hPaFromRaw4060(int32_t rawAligned) {
    return static_cast<float>(rawAligned) / 524288.0f;
}
inline float hPaFromRaw(int32_t rawAligned, bool fsMode2) {
    return fsMode2 ? hPaFromRaw4060(rawAligned) : hPaFromRaw1260(rawAligned);
}

// Temperature: vendor from_lsb_to_celsius = LSB / 100.0f, 16-bit two's complement.
inline float tempCFromRaw(uint8_t tl, uint8_t th) {
    const int16_t raw = static_cast<int16_t>(tl | (static_cast<uint16_t>(th) << 8));
    return static_cast<float>(raw) / 100.0f;
}

// Soft-reset settle: vendor status_get exposes swreset self-clear; the DS PDF did not re-fetch
// this week, so the driver POLLS the bit instead of trusting any remembered fixed time (risk 21:
// no recited waits). The constant here is the driver's timeout bound, not a datasheet claim.
constexpr uint32_t kSwResetTimeoutMs = 200;

}  // namespace lps28
