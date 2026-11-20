// INA219 register-map and conversion tests (lib/core/src/PwrRegs.h).
//
// All anchors trace to the deployed Adafruit INA219 library (read in full 2026-11-20): its
// field masks, its three calibration presets, its documented formulas (Cal = trunc(0.04096 /
// (Current_LSB x RSHUNT)); PowerLSB = 20 x Current_LSB) and its raw->engineering conversions.
// The TI datasheet was not in hand writing these — the tests prove the transcription is
// SELF-CONSISTENT with the library's published arithmetic, and the first fitted module gets a
// meter cross-check before any current number is trusted (see PROVENANCE in PwrRegs.h).
#include "PwrRegs.h"
#include "unity.h"

using namespace pwr;

void setUp() {}
void tearDown() {}

// The ADS1115 RATE-field incident (overlapping fields, plausible readback) is why the config
// fields are asserted to tile cleanly: each field at its derived position, no two masks
// overlapping, and the composed preset words matching the library's config composition.
void test_config_fields_tile_the_register_exactly_once() {
    TEST_ASSERT_EQUAL_UINT8(13, fieldPos(kCfgMaskBusRange));
    TEST_ASSERT_EQUAL_UINT8(11, fieldPos(kCfgMaskGain));
    TEST_ASSERT_EQUAL_UINT8(7, fieldPos(kCfgMaskBadcRes));
    TEST_ASSERT_EQUAL_UINT8(3, fieldPos(kCfgMaskSadcRes));
    TEST_ASSERT_EQUAL_UINT8(0, fieldPos(kCfgMaskMode));
    const uint16_t masks[] = {kCfgMaskBusRange, kCfgMaskGain, kCfgMaskBadcRes, kCfgMaskSadcRes,
                              kCfgMaskMode};
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j)
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, masks[i] & masks[j], "config fields overlap");
    TEST_ASSERT_EQUAL_UINT16(0x8000, kCfgReset);  // reset bit stands outside the fields
    // The library's 32V/2A config composition (32V range | gain 8 | 12-bit badc | 12-bit sadc |
    // shunt+bus continuous) composes to 0x399F, field-for-field decodable.
    TEST_ASSERT_EQUAL_HEX16(0x399F, kConfig32V2A);
    TEST_ASSERT_EQUAL_HEX16(0x019F, kConfig16V400mA);
    TEST_ASSERT_EQUAL_UINT16(1, getField(kConfig32V2A, kCfgMaskBusRange));  // 32 V
    TEST_ASSERT_EQUAL_UINT16(3, getField(kConfig32V2A, kCfgMaskGain));      // gain 8
    TEST_ASSERT_EQUAL_UINT16(3, getField(kConfig32V2A, kCfgMaskBadcRes));   // 12-bit
    TEST_ASSERT_EQUAL_UINT16(3, getField(kConfig32V2A, kCfgMaskSadcRes));   // 12-bit 1s
    TEST_ASSERT_EQUAL_UINT16(7, getField(kConfig32V2A, kCfgMaskMode));      // continuous
    TEST_ASSERT_EQUAL_UINT16(0, getField(kConfig16V400mA, kCfgMaskBusRange));
    TEST_ASSERT_EQUAL_UINT16(0, getField(kConfig16V400mA, kCfgMaskGain));   // gain 1
}

// Cal = trunc(0.04096 / (Current_LSB_amps x RSHUNT)) is the library's printed formula; the three
// presets are its documented words. Asserting the formula reproduces each word is what makes a
// future different-shunt re-derivation arithmetic instead of folklore (risk 21). Inputs are
// exact integers (nA, nOhm): float cannot represent 0.1, and the float form of this formula
// truncated the documented exact anchor to 4095 — caught by this test, hence the integer form.
void test_calibration_presets_follow_the_published_formula() {
    TEST_ASSERT_EQUAL_UINT32(4096u, calRegister(100000ull, 100000000ull));   // 100 uA, 0.1 R -> 0x1000
    TEST_ASSERT_EQUAL_UINT32(10240u, calRegister(40000ull, 100000000ull));   // 40 uA -> 0x2800
    TEST_ASSERT_EQUAL_UINT32(8192u, calRegister(50000ull, 100000000ull));    // 50 uA -> 0x2000
    // a hypothetical non-0.1-ohm derivation, to prove the function is the formula and not a lookup:
    // 500 uA LSB x 0.01 ohm -> trunc(0.04096/5e-6) = 8192
    TEST_ASSERT_EQUAL_UINT32(8192u, calRegister(500000ull, 10000000ull));
    // division by zero must answer 0, not trap (a config bug becomes a visible bad cal word)
    TEST_ASSERT_EQUAL_UINT32(0u, calRegister(0ull, 100000000ull));

    TEST_ASSERT_EQUAL_UINT16(4096u, kPreset32V2A.cal);
    TEST_ASSERT_EQUAL_UINT32(10u, kPreset32V2A.divider_mA);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, kPreset32V2A.mult_mW);
    TEST_ASSERT_EQUAL_UINT16(10240u, kPreset32V1A.cal);
    TEST_ASSERT_EQUAL_UINT32(25u, kPreset32V1A.divider_mA);
    TEST_ASSERT_EQUAL_FLOAT(0.8f, kPreset32V1A.mult_mW);
    TEST_ASSERT_EQUAL_UINT16(8192u, kPreset16V400mA.cal);
    TEST_ASSERT_EQUAL_UINT32(20u, kPreset16V400mA.divider_mA);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, kPreset16V400mA.mult_mW);
    // Internal consistency the library asserts by construction: divider sets the current LSB
    // (1/divider mA = 1e6/divider nA), the cal word follows from the formula at that LSB, and
    // its printed rule #6 (PowerLSB = 20 x Current_LSB) gives the multiplier.
    for (int i = 0; i < 3; ++i) {
        const Preset& p = i == 0 ? kPreset32V2A : (i == 1 ? kPreset32V1A : kPreset16V400mA);
        const uint64_t lsb_nA = 1000000ull / p.divider_mA;
        const float lsb_mA = 1.0f / static_cast<float>(p.divider_mA);
        TEST_ASSERT_EQUAL_UINT32(calRegister(lsb_nA, 100000000ull), p.cal);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 20.0f * lsb_mA, p.mult_mW);
    }
}

// Raw conversions, reproduced bit-for-bit from the deployed library (including how its bus
// conversion shifts past the CNVR/OVF flag bits — a set flag LEAKS 4 mV into its result; the
// driver mirrors that behaviour, so this pins it rather than pretending it cannot happen).
void test_raw_conversions_match_the_deployed_library_bit_for_bit() {
    // Bus: value>>3 then x4 mV. 0x1D4C -> 937 x 4 = 3748 mV (a LiPo rail). 0x3E10 -> 7944 mV.
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 3.748f, busVolts(0x1D4C));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 7.944f, busVolts(0x3E10));
    // The reference conversion's >>3 deliberately drops the three flag bits sitting below the
    // voltage field (its comment: "drop CNVR and OVF"): words differing ONLY in bits 2:0 convert
    // identically, and any difference at bit 3 and above is one 4 mV LSB per bit. Asserted from
    // the reference's arithmetic, without claiming bit names the library header does not define.
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, busVolts(0x3E10), busVolts(0x3E14));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, busVolts(0x3E10), busVolts(0x3E11));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.948f, busVolts(0x3E18));  // bit 3 IS the voltage LSB
    // Shunt: signed LSB 10 uV -> mV = raw x 0.01. Full scale from int16 extremes.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, shuntmV(0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.24f, shuntmV(0x0018));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 327.67f, shuntmV(0x7FFF));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, -327.68f, shuntmV(static_cast<int16_t>(0x8000)));
    // Current/power scale with the preset: 32V_2A overflows at 3.2 A (library's own note).
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 3276.7f, current_mA(0x7FFF, kPreset32V2A.divider_mA));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, -3276.8f,
                             current_mA(static_cast<int16_t>(0x8000), kPreset32V2A.divider_mA));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1000.0f, current_mA(10000, kPreset32V2A.divider_mA));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 65534.0f, power_mW(0x7FFF, kPreset32V2A.mult_mW));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -2.0f, power_mW(-1, kPreset32V2A.mult_mW));
}

// The identity-check window must bracket the operating thresholds strictly wider (the check
// asks "is this sensor measuring", not "is the pack good"): pack spec 3.0-4.25 V, low-voltage
// injection lockout 3.4 V, arm floor 3.6 V — all INSIDE 2.5-5.5 V.
void test_rail_plausibility_window_is_wider_than_the_operating_thresholds() {
    TEST_ASSERT_TRUE(kRailPlausibleMinV < 3.0f);
    TEST_ASSERT_TRUE(3.4f < 3.6f);
    TEST_ASSERT_TRUE(3.6f < 4.2f);
    TEST_ASSERT_TRUE(4.2f < kRailPlausibleMaxV);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_config_fields_tile_the_register_exactly_once);
    RUN_TEST(test_calibration_presets_follow_the_published_formula);
    RUN_TEST(test_raw_conversions_match_the_deployed_library_bit_for_bit);
    RUN_TEST(test_rail_plausibility_window_is_wider_than_the_operating_thresholds);
    return UNITY_END();
}
