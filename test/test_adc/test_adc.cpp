// ADS1115 register-map tests. The bit positions and bit meanings in AdcRegs.h were wrong until
// measured on hardware, and nothing in src/ is compiled natively — so the constants the whole cell
// chain is scaled by live here, where they can be pinned. Anchor: the power-on config word read from
// the bench module on 2026-11-19, cfg=0x8583, which decodes as single-shot / PGA gain 2/3 / 8 SPS /
// mux AIN0-AIN1 / not busy — the datasheet's documented default. Two faults were found this way:
// the rate field sat in bits 10:8 (overlapping PGA), and MODE was read backwards, which put the
// converter in continuous mode and made the first on-hardware conversion test time out.
#include "AdcRegs.h"
#include "unity.h"

using namespace adc;

void setUp() {}
void tearDown() {}

// The masks must tile the register exactly, once each. This is the test that fails the moment a
// field moves: the wrong rate field overlapped PGA, and every number downstream stayed plausible.
void test_config_field_masks_tile_the_register_without_overlap() {
    const uint16_t all = kOsMask | kMuxMask | kPgaMask | kModeMask | kDrMask | kRateMask | kCtMask |
                         kPolMask | kTmMask | kNopMask;
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xFFFF, all, "config fields do not cover all 16 bits");

    const uint16_t fields[] = {kOsMask, kMuxMask, kPgaMask, kModeMask, kDrMask, kRateMask,
                               kCtMask, kPolMask, kTmMask, kNopMask};
    for (unsigned i = 0; i < 10; ++i)
        for (unsigned j = i + 1; j < 10; ++j)
            TEST_ASSERT_EQUAL_MESSAGE(0, fields[i] & fields[j],
                                      "config field masks overlap — one field has moved");
    // Rate sits below PGA. If this fails, the rate field has drifted up into the PGA bits — which
    // is exactly the bug that made a 32 SPS write land as 128 SPS.
    TEST_ASSERT_TRUE_MESSAGE(kRateMask < kPgaMask, "rate field is no longer below PGA");
}

// Hardware anchor, asserted as meanings rather than as a magic word.
void test_measured_power_on_config_decodes_as_the_datasheet_default() {
    TEST_ASSERT_FALSE_MESSAGE(isConverting(kPowerOnConfig), "power-on word reads as mid-conversion");
    TEST_ASSERT_EQUAL_UINT16(0, muxField(kPowerOnConfig));
    TEST_ASSERT_EQUAL_UINT16(2, pgaField(kPowerOnConfig));   // PGA 010 = +/-2.048 V, gain 2
    TEST_ASSERT_EQUAL_UINT16(0, rateField(kPowerOnConfig));
    TEST_ASSERT_TRUE_MESSAGE(isSingleShot(kPowerOnConfig),
                             "power-on default is not single-shot — MODE bit meaning flipped");
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, sps(kPowerOnConfig), "power-on rate is not the DS default 8 SPS");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.048f, fsrVolts(kPowerOnConfig), "power-on PGA FSR wrong");
    TEST_ASSERT_EQUAL_UINT32(125000, convUs(kPowerOnConfig));
}

// The design settings: gain 16 / +/-256 mV and 32 SPS (AGENTS.md 2), single-shot, one pair. MODE is
// asserted explicitly because leaving that bit clear selects continuous mode, where the mux is
// locked and OS never returns — the failure the bench measured.
void test_design_config_selects_gain16_32sps_and_explicit_single_shot() {
    const uint16_t cfg = designConfig(kMuxAin2Ain3);
    TEST_ASSERT_EQUAL_UINT16(3, muxField(cfg));
    TEST_ASSERT_EQUAL_UINT16(5, pgaField(cfg));
    TEST_ASSERT_EQUAL_UINT16(2, rateField(cfg));
    TEST_ASSERT_TRUE_MESSAGE(isSingleShot(cfg), "design config must set MODE=single-shot explicitly");
    TEST_ASSERT_FALSE_MESSAGE(isConverting(cfg), "design config must have OS set to start");
    TEST_ASSERT_FALSE_MESSAGE(isPowerDownMux(cfg), "design config selects a power-down mux");
    TEST_ASSERT_EQUAL_INT(32, sps(cfg));
    TEST_ASSERT_EQUAL_FLOAT(0.256f, fsrVolts(cfg));
    TEST_ASSERT_EQUAL_FLOAT(16.0f, gainField(pgaField(cfg)));
    TEST_ASSERT_EQUAL_UINT32(31250, convUs(cfg));
}

// LSB and gain against the datasheet tables. The "0.125 mV LSB" figure that appeared in this
// project's early notes is the *gain 1* value; at gain 16 the LSB is 7.8125 uV, so using it scales
// every ppO2 by 16x while still looking like a cell signal.
void test_lsb_and_gain_tables_match_the_datasheet() {
    const float fsr[6] = {6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f};
    const float gain[6] = {0.6666667f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    for (uint16_t f = 0; f < 6; ++f) {
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6, fsr[f], fsrVoltsField(f), "FSR wrong");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-6, gain[f], gainField(f), "gain wrong");
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-12, fsr[f] / 32768.0f, lsbVoltsField(f), "LSB wrong");
    }
    // Exact, not rounded: +/-0.256 V over 32768 steps.
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-12, 0.0000078125f, lsbVolts(designConfig()),
                                     "design LSB is not 7.8125 uV");
    // The size of the bug these tables exist to prevent: gain 1 vs gain 16 differ by exactly 16x.
    TEST_ASSERT_FLOAT_WITHIN(1e-6, 16.0f, lsbVoltsField(1) / lsbVoltsField(5));
}

// A galvanic cell in air is ~9-13 mV (AGENTS.md 2). How much of the converter that uses is a design
// claim, and it is also what makes a railed input distinguishable from a real cell.
void test_cell_signal_at_design_gain_uses_its_expected_share_of_range() {
    TEST_ASSERT_EQUAL_INT16_MESSAGE(1152, codesForMillivolts(9.0f), "9 mV in air: wrong code count");
    TEST_ASSERT_EQUAL_INT16_MESSAGE(1664, codesForMillivolts(13.0f), "13 mV in air: wrong code count");
    TEST_ASSERT_EQUAL_INT16(-1664, codesForMillivolts(-13.0f));  // polarity resolved on the bench
    // 1.6 bar ppO2 at the design gain, for scale: a galvanic cell's output is roughly proportional
    // to ppO2, so ~13 mV * (1.6/0.21) would rail the +/-256 mV input. The gain-16 choice is the
    // reason the air-cal to 1.6 bar range is asserted in AGENTS.md, not assumed.
    TEST_ASSERT_EQUAL_INT16(32767, codesForMillivolts(1000.0f));  // clamped, never wrapped
    TEST_ASSERT_EQUAL_INT16(-32768, codesForMillivolts(-1000.0f));
    // Round trip: mV -> code -> mV is where a wrong LSB shows up as a scale error.
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.008f, 13.0f, millivoltsOf(codesForMillivolts(13.0f)),
                                     "mV round trip lost scale");
}

// Conversion timing: what the coil-pulse interlock is sized on (AGENTS.md 8), and what separates an
// ADS1115 from an ADS1015 at the same register field.
void test_conversion_period_matches_the_design_rate_and_separates_the_parts() {
    TEST_ASSERT_EQUAL_UINT32(31250, convUsField(2));   // 32 SPS
    TEST_ASSERT_EQUAL_UINT32(125000, convUsField(0));  // 8 SPS, the power-on default
    TEST_ASSERT_EQUAL_UINT32(1162, convUsField(7));    // 860 SPS
    // Per-cell rate after walking both pairs: two conversions per cycle at 32 SPS = 16 SPS/cell.
    TEST_ASSERT_EQUAL_UINT32(62500, 2 * convUs(designConfig()));
    // An ADS1015 given the same field runs ~100x faster; the driver's identity window has to sit
    // above that difference, so pin the margin instead of trusting it.
    TEST_ASSERT_TRUE_MESSAGE(convUs(designConfig()) / 100u < 1000u,
                             "ADS1015/ADS1115 timing margin has collapsed");
}

// Power-down mux encodings must never be selected: they float the input, and the result reads as a
// sample rather than as a fault.
void test_mux_set_excludes_the_power_down_encodings() {
    const uint16_t allowed[] = {kMuxAin0Ain1, kMuxAin0Ain3, kMuxAin1Ain3, kMuxAin2Ain3};
    for (uint16_t m : allowed) {
        TEST_ASSERT_TRUE_MESSAGE(!isPowerDownMux(designConfig(m)), "design mux is a power-down mux");
        TEST_ASSERT_TRUE(muxField(designConfig(m)) < 4);
    }
    TEST_ASSERT_TRUE_MESSAGE(isPowerDownMux(designConfig(kMuxPowerDown)),
                             "power-down encoding no longer detected");
}

// The two parts share address, register map and field positions. Only the rate table differs, so a
// single field value means two different conversion periods — the measured difference the driver's
// identity check depends on.
void test_same_rate_field_separates_ads1115_from_ads1015_by_100x() {
    const uint32_t ads1115Us = convUsField(rateField(designConfig()));
    const uint32_t ads1015Us = ads1115Us / 100u;  // ADS1015 rate table is 100x faster at 32 SPS
    TEST_ASSERT_EQUAL_UINT32(31250, ads1115Us);
    TEST_ASSERT_EQUAL_UINT32(312, ads1015Us);
    // The driver accepts 25..60 ms; an ADS1015 lands far outside it and must be refused rather than
    // sampled at the wrong rate while the control loop assumed 32 SPS.
    TEST_ASSERT_TRUE(ads1015Us < 25000u);
    TEST_ASSERT_TRUE(ads1115Us >= 25000u && ads1115Us <= 60000u);
}

// The measured module does not implement the datasheet's rate table, and the firmware has to notice
// rather than assume. Anchored to the numbers from bench command 'd' on 2026-11-19 so a future
// change to the acceptance logic cannot quietly decide the bench module is fine.
void test_measured_bench_module_rate_table_is_not_the_datasheet_one() {
    // field 2 asks for 32 SPS; the module delivers 16.1 SPS, i.e. ~100% slower.
    const uint32_t nominal = convUsField(2);
    const uint32_t measured = 61978;  // us, measured
    TEST_ASSERT_EQUAL_UINT32(31250, nominal);
    TEST_ASSERT_EQUAL_INT_MESSAGE(98, rateErrorPercent(measured, nominal),
                                 "measured-vs-datasheet rate error changed");
    // The same part at the datasheet's 860 SPS field: it tops out near 63 SPS, never 860.
    // 15879 us measured where the datasheet promises 1162 us: an order of magnitude out.
    TEST_ASSERT_TRUE_MESSAGE(rateErrorPercent(15879, convUsField(7)) > 1000,
                             "the part's top rate is not as measured on the bench");
    // A genuine ADS1115 at the design field lands here; the acceptance window has to straddle both
    // this and the bench module, because refusing a converter that works is its own harm.
    TEST_ASSERT_EQUAL_INT(0, rateErrorPercent(31250, convUsField(2)));
    // And the recorded measured table: four rates, not eight.
    for (uint16_t f = 0; f < 8; ++f)
        TEST_ASSERT_TRUE_MESSAGE(kMeasuredSpsBenchModule[f] <= 64,
                                 "bench module recorded as reaching a rate it does not reach");
    TEST_ASSERT_EQUAL_INT(8, kMeasuredSpsBenchModule[0]);
    TEST_ASSERT_EQUAL_INT(63, kMeasuredSpsBenchModule[7]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_config_field_masks_tile_the_register_without_overlap);
    RUN_TEST(test_measured_power_on_config_decodes_as_the_datasheet_default);
    RUN_TEST(test_design_config_selects_gain16_32sps_and_explicit_single_shot);
    RUN_TEST(test_lsb_and_gain_tables_match_the_datasheet);
    RUN_TEST(test_cell_signal_at_design_gain_uses_its_expected_share_of_range);
    RUN_TEST(test_conversion_period_matches_the_design_rate_and_separates_the_parts);
    RUN_TEST(test_mux_set_excludes_the_power_down_encodings);
    RUN_TEST(test_same_rate_field_separates_ads1115_from_ads1015_by_100x);
    RUN_TEST(test_measured_bench_module_rate_table_is_not_the_datasheet_one);
    return UNITY_END();
}
