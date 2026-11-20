// LPS28DFW register-map and scaling tests (lib/core/src/BaroLps28.h, decision 39 tier-2).
//
// Provenance discipline as everywhere else in this project (decision 37, risk 21): every value
// asserted here comes from ST's own vendor driver lps28dfw_reg.h/.c (github.com/STMicroelectronics/
// lps28dfw, (c) 2024, fetched and read 2026-11-20) — WHO_AM_I 0xB4, the CTRL_REG1/2 bit positions,
// the sensitivity constants (4096 LSB/hPa at FS 1260, 2048 at FS 4060, temperature LSB/100 C) —
// never from memory. The scaling is cross-checked against a second copy of the vendor's own
// arithmetic shape (24-bit word / sensitivity) as a discriminator for the left-aligned assembly.
//
// STATUS: the part is UNPROCURED (decision 39 procurement). These tests pin the transcription;
// they are NOT belief that a fitted module behaves — arrival acceptance is bench 'b' (WHO_AM_I,
// FS_MODE writeback, a Stockholm-station mbar, squeeze movement), same rule as every part here
// (decision 29).
#include "BaroLps28.h"
#include "Depth.h"
#include <stdio.h>
#include "unity.h"

using namespace lps28;

void setUp() {}
void tearDown() {}

// Vendor-shape reference copy (decision 37's pattern): ST assembles a 24-bit word and divides by
// the sensitivity directly; our header assembles left-aligned and divides by sensitivity*256.
// Inside either full-scale window (bit 23 clear) the two are the same number. The reference here
// discriminates the assembly and the divisors from anything else that could produce plausible
// mbar (risk 13: plausible numbers are the failure mode, not these anchors).
static float vendorRefHPa(uint8_t xl, uint8_t l, uint8_t h, bool fs2) {
    const int32_t raw24 = (static_cast<int32_t>(h) << 16) | (static_cast<int32_t>(l) << 8) | xl;
    return static_cast<float>(raw24) / (fs2 ? 2048.0f : 4096.0f);
}

void test_sensitivity_anchors_both_modes() {
    // 1000 hPa exact in both modes: 4096 LSB/hPa and 2048 LSB/hPa, from the vendor's stated
    // sensitivity constants.
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0x3E8000, 1000 * 4096, "1000 hPa at FS1260 raw word");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0x1F4000, 1000 * 2048, "1000 hPa at FS4060 raw word");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1000.0f, hPaFromRaw1260(pressureRawAligned(0x00, 0x80, 0x3E)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1000.0f, hPaFromRaw4060(pressureRawAligned(0x00, 0x40, 0x1F)));

    // Full-scale ends: 1260 hPa and 4060 hPa. NOTE (decision 39): 4060 is ABSOLUTE — the gauge
    // water column at this bench is the ceiling test below computes, not "40 m underwater".
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1260.0f, hPaFromRaw1260(pressureRawAligned(0x00, 0xC0, 0x4E)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 4060.0f, hPaFromRaw4060(pressureRawAligned(0x00, 0xE0, 0x7E)));

    // A bench-coherent reading: 978 hPa at mode 2 = 0x1E9000 exactly.
    const int32_t bench = pressureRawAligned(0x00, 0x90, 0x1E);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 978.0f, hPaFromRaw4060(bench));

    // hPaFromRaw selects by the LATCHED mode, and the mode bit must not be interchangeable:
    // reading a mode-2 word with mode-1 scaling halves the answer — a silent 2x depth error if
    // the ceiling ever came from the write request instead of the readback (driver gates this).
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 489.0f, hPaFromRaw1260(bench));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 978.0f, hPaFromRaw(bench, true));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 489.0f, hPaFromRaw(bench, false));
}

void test_left_aligned_assembly_matches_vendor_shape() {
    // Grid over both modes' in-range words: the header's left-aligned assembly divided by
    // sensitivity*256 must equal the vendor's 24-bit-word / sensitivity form to float precision.
    int checked = 0;
    for (int h = 0x00; h <= 0x7E; h += 0x11) {
        for (int l = 0x00; l <= 0xFF; l += 0x3D) {
            const uint8_t xl = static_cast<uint8_t>((h ^ l) & 0xFF);
            const int32_t aligned = pressureRawAligned(xl, static_cast<uint8_t>(l),
                                                       static_cast<uint8_t>(h));
            TEST_ASSERT_FLOAT_WITHIN(0.02f, vendorRefHPa(xl, static_cast<uint8_t>(l),
                                                         static_cast<uint8_t>(h), false),
                                     hPaFromRaw1260(aligned));
            TEST_ASSERT_FLOAT_WITHIN(0.02f, vendorRefHPa(xl, static_cast<uint8_t>(l),
                                                         static_cast<uint8_t>(h), true),
                                     hPaFromRaw4060(aligned));
            ++checked;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(8 * 5, checked, "the grid no longer covers both modes' words");
}

void test_twos_complement_negative_and_temperature() {
    // Negative branch pinned even though an absolute-pressure sensor should never see it in
    // range: -1 hPa at mode 2 is raw -2048 = 0xFFF800. A sign-extension bug here would rail the
    // reading to +4000-ish hPa plausible-looking garbage (risk 13's class).
    const int32_t neg = pressureRawAligned(0x00, 0xF8, 0xFF);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(-524288, neg, "0xFFF800 must sign-extend to -2048*256");
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, hPaFromRaw4060(neg));

    // Temperature: vendor from_lsb_to_celsius = LSB/100, 16-bit two's complement.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, tempCFromRaw(0xC4, 0x09));    // 2500
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -23.0f, tempCFromRaw(0x04, 0xF7));   // -2300 = 0xF704
}

void test_field_masks_and_design_config() {
    // CTRL_REG1: AVG[2:0], ODR[6:3]. The design word is continuous 10 Hz (ODR=3) + AVG field 0.
    const uint8_t wantCtrl1 = static_cast<uint8_t>((kDesignOdr << kOdrShift) | kDesignAvg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x18, wantCtrl1, "design CTRL_REG1 word moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(kDesignOdr, (wantCtrl1 >> kOdrShift) & kOdrMask, "ODR decode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(kDesignAvg, wantCtrl1 & kAvgMask, "AVG decode");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x78, static_cast<int>(kOdrMask << kOdrShift),
                                  "ODR field must sit at CTRL_REG1[6:3]");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, static_cast<int>((kOdrMask << kOdrShift) & kAvgMask),
                                  "ODR/AVG fields must not overlap");

    // CTRL_REG2 bits per the vendor header: ONESHOT0, SWRESET2, BDU3, FS_MODE6, BOOT7.
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, kBitOneshot, "ONESHOT bit moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, kBitSwReset, "SWRESET bit moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, kBitBdu, "BDU bit moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, kBitFsMode, "FS_MODE bit moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(7, kBitBoot, "BOOT bit moved");
    // FS_MODE is bit 6, NOT bit 7: an off-by-one here writes BOOT (memory boot, self-clearing)
    // and leaves the part at 1260 hPa while the ceiling claims 4.06 bar. The driver's readback
    // gate catches it on hardware; this catches it in CI.
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x48, (1u << kBitFsMode) | (1u << kBitBdu),
                                  "design CTRL_REG2 word (FS_MODE + BDU) moved");

    // WHO_AM_I and the address pair (ST bits 101110xb).
    TEST_ASSERT_EQUAL_INT_MESSAGE(0xB4, kWhoAmI, "WHO_AM_I constant moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x5C, kAddr7, "SA0=0 7-bit address moved");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0x5D, kAddr7Alt, "SA0=1 7-bit address moved");
}

void test_range_gates_and_derived_depth_ceiling() {
    // Window edges per the dual-FS design (both modes bottom at 260 hPa).
    TEST_ASSERT_FALSE(inRangeBar(0.25f, true));
    TEST_ASSERT_TRUE(inRangeBar(0.26f, true));
    TEST_ASSERT_TRUE(inRangeBar(4.06f, true));
    TEST_ASSERT_FALSE(inRangeBar(4.07f, true));
    TEST_ASSERT_TRUE(inRangeBar(1.26f, false));
    TEST_ASSERT_FALSE(inRangeBar(1.27f, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.06f, rangeCeilingBar(true));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.26f, rangeCeilingBar(false));

    // The decision-39 ceiling, DERIVED not recited (decision 38): 4.06 bar abs minus the learned
    // bench Psurf 977.89 mbar, salt hydrostatics. This is the figure the BARO_RANGE banner
    // prints for tier 2 — and it is under the "40 m underwater" the breakout's product page
    // claims, because that marketing treats an ABSOLUTE full scale as if it were gauge.
    const float ceilBench = depth::ceilingMeters(0.97789f, rangeCeilingBar(true), 1.025f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.7f, ceilBench);
    const float ceilSL = depth::ceilingMeters(1.01325f, rangeCeilingBar(true), 1.025f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.3f, ceilSL);
    TEST_ASSERT_TRUE_MESSAGE(ceilSL < 40.0f,
                             "the derived tier-2 ceiling reached the marketing figure — "
                             "re-derive the hydrostatics (decision 38)");
    // And it is an order of magnitude beyond the BMP180 bench tier (< 1.3 m) — the reason tier 2
    // is armable with a banner and tier 3 is bench-only.
    const float ceilBmp = depth::ceilingMeters(0.97789f, 1.10f, 1.025f);
    TEST_ASSERT_TRUE(ceilBmp < 1.3f);
    TEST_ASSERT_TRUE(ceilBench > 10.0f * ceilBmp);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sensitivity_anchors_both_modes);
    RUN_TEST(test_left_aligned_assembly_matches_vendor_shape);
    RUN_TEST(test_twos_complement_negative_and_temperature);
    RUN_TEST(test_field_masks_and_design_config);
    RUN_TEST(test_range_gates_and_derived_depth_ceiling);
    return UNITY_END();
}
