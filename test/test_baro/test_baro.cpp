// BMP085/BMP180 register-map, cal-ROM and compensation tests (lib/core/src/BaroBosch.h).
//
// Provenance discipline, because this exact datasheet was mis-transcribed twice on 2026-11-20
// (AGENTS.md decision 36, risk 21): facts asserted here are either (a) text-extracted from
// BST-BMP180-DS000-09, (b) taken from the deployed Adafruit BMP085 library (cloned, read, and
// cross-checked 2026-11-20 — four transcription faults in the first version of BaroBosch.h were
// found that way), or (c) measured on the bench module. The anchor set is the library's debug
// block commented "use datasheet numbers!": under the deployed arithmetic it reproduces the
// example's documented 15.0 C EXACTLY; under the (wrong) first transcription it gave 431.6 C.
// Self-consistency at the documented result was the discriminator — see the PROVENANCE block in
// BaroBosch.h. This suite pins the arithmetic against accidental future edits; belief in the
// algorithm still comes from hardware: bench 'b' must show a real local-station mbar that moves
// when the port is stimulated, and agreement with the Adafruit library on the same physical
// module. (Captured 2026-11-20: agreement 0.02 mbar, stimulus proven, hand recompute from the
// dumped words matches the on-target driver. The absolute value read 977.7 mbar at a near-sea-
// level bench against a 1017 hPa weather reference — an open part-level finding, AGENTS.md
// §20 item 18 — so ~1013 was the wrong expectation to state here; arithmetic is exonerated.)
#include "BaroBosch.h"
#include "unity.h"

using namespace baro;

void setUp() {}
void tearDown() {}

// The library's "use datasheet numbers!" set — DS Table 5 order, signed/unsigned per the
// reference. mb is a rail value (-32768) but not 0 or 0xFFFF, so the DS validity rule passes.
static Cal dsExampleCal() {
    Cal c;
    c.ac1 = 408;  c.ac2 = -72;  c.ac3 = -14383;
    c.ac4 = 32741; c.ac5 = 32757; c.ac6 = 23153;
    c.b1 = 6190;  c.b2 = 4;
    c.mb = -32768; c.mc = -8711; c.md = 2868;
    return c;
}

// DS-verified command bytes and conversion times (BMP180 DS, measurement section).
void test_pressure_commands_and_conversion_times_match_the_datasheet() {
    TEST_ASSERT_EQUAL_HEX8(0x34, pressureCommand(0));
    TEST_ASSERT_EQUAL_HEX8(0x74, pressureCommand(1));
    TEST_ASSERT_EQUAL_HEX8(0xB4, pressureCommand(2));
    TEST_ASSERT_EQUAL_HEX8(0xF4, pressureCommand(3));
    TEST_ASSERT_EQUAL_UINT32(4500, pressureConvUs(0));
    TEST_ASSERT_EQUAL_UINT32(7500, pressureConvUs(1));
    TEST_ASSERT_EQUAL_UINT32(13500, pressureConvUs(2));
    TEST_ASSERT_EQUAL_UINT32(25500, pressureConvUs(3));
    TEST_ASSERT_EQUAL_UINT32(4500, kTempConvUs);
    TEST_ASSERT_EQUAL_HEX8(0x2E, kCmdTemperature);
    // Out-of-range oss clamps rather than inventing a command the part does not define.
    TEST_ASSERT_EQUAL_HEX8(0x34, pressureCommand(-4));
    TEST_ASSERT_EQUAL_HEX8(0xF4, pressureCommand(9));
}

// The DS's own communication check: no word 0 or 0xFFFF. It is NOT a value check — AC1 measured
// 0x219C on the bench module (8604) and decision 36 wrongly treated that as a fault. It must pass.
void test_cal_rom_rule_is_the_datasheet_rule_not_a_value_check() {
    const Cal good = dsExampleCal();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, calRomFirstBadWord(good), "example cal must pass the 0/0xFFFF rule");

    // The bench module's measured AC1 word with the rest as a plausible trim: passes. This is
    // the regression that a remembered-number check would have failed.
    Cal bench = good;
    bench.ac1 = static_cast<int16_t>(0x219C);
    TEST_ASSERT_TRUE(calRomPlausible(bench));

    // Each of the 11 positions, poisoned with 0 and with 0xFFFF, is caught at its own index.
    // Poison through a switch, not pointer arithmetic across mixed-type members.
    for (int pos = 0; pos < kCalWords; ++pos) {
        const int16_t bad16 = static_cast<int16_t>(0xFFFF);
        Cal c = good;
        switch (pos) {
            case 0: c.ac1 = 0; break;   case 1: c.ac2 = 0; break;  case 2: c.ac3 = 0; break;
            case 3: c.ac4 = 0; break;   case 4: c.ac5 = 0; break;  case 5: c.ac6 = 0; break;
            case 6: c.b1 = 0; break;    case 7: c.b2 = 0; break;   case 8: c.mb = 0; break;
            case 9: c.mc = 0; break;    case 10: c.md = 0; break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(pos, calRomFirstBadWord(c), "zero word not caught at its index");
        c = good;
        switch (pos) {
            case 0: c.ac1 = bad16; break;  case 1: c.ac2 = bad16; break; case 2: c.ac3 = bad16; break;
            case 3: c.ac4 = 0xFFFF; break; case 4: c.ac5 = 0xFFFF; break; case 5: c.ac6 = 0xFFFF; break;
            case 6: c.b1 = bad16; break;   case 7: c.b2 = bad16; break;  case 8: c.mb = bad16; break;
            case 9: c.mc = bad16; break;   case 10: c.md = bad16; break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(pos, calRomFirstBadWord(c), "0xFFFF word not caught at its index");
    }
}

// DS 5.7: at oss>0, 0xF6/0xF7 are only the TOP 16 bits; 0xF8 completes the word. One raw 24-bit
// word, shifted per oss, must reproduce the oss=0 16-bit value and scale by 2 per oss step.
void test_assembleup_bit_alignment_scales_with_oss() {
    // Raw wire bytes that encode UP=137286 at oss=2 (the bisection value for ~1013 mbar below).
    const uint8_t msb = 0x86, lsb = 0x11, xlsb = 0x80;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x8611, assembleUp(msb, lsb, xlsb, true, 0),
                                     "oss=0 with XLSB read must equal the plain 16-bit read");
    TEST_ASSERT_EQUAL_UINT32(0x8611, assembleUp(msb, lsb, 0x00, false, 0));
    TEST_ASSERT_EQUAL_UINT32(137286, assembleUp(msb, lsb, xlsb, true, 2));
    // The exact per-oss values of one wire word: UP = w >> (8-oss). Doubling across oss steps is
    // floor-division only — 0x80 in xlsb survives into oss=1 (68643 = 2*34321+1) and is lost at
    // oss=0. That resolution loss IS oversampling; asserting the truncated chain, not a tidy one.
    TEST_ASSERT_EQUAL_UINT32(34321, assembleUp(msb, lsb, xlsb, true, 0));
    TEST_ASSERT_EQUAL_UINT32(68643, assembleUp(msb, lsb, xlsb, true, 1));
    TEST_ASSERT_EQUAL_UINT32(274572, assembleUp(msb, lsb, xlsb, true, 3));  // one wire word, all oss
    TEST_ASSERT_EQUAL_UINT16(0x6D5A, assembleUt(0x6D, 0x5A));  // example UT=27898, unsigned 16-bit
}

// ANCHOR: the deployed library's "use datasheet numbers!" run. UT=27898, UP=23843, oss=0 with
// the example cal gives B5=2400 -> 15.0 C exactly (the example's documented temperature), and
// p=69964 Pa by the deployed arithmetic. The first (wrong) transcription of computeB5 gave
// 431.6 C — that is the regression this exact-word assertion exists for.
void test_datasheet_example_reproduces_exactly() {
    const Reading r = compensate(dsExampleCal(), 27898, 23843, 0);
    TEST_ASSERT_TRUE(r.converted);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2400, r.b5, "B5 is the documented example intermediate");
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 15.0f, r.tempC);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 69964.0f, r.pressurePa);
}

// Same relationship, computed a second way — the reference arithmetic transcribed verbatim from
// Adafruit_BMP085.cpp (read 2026-11-20) — over a grid of sane inputs and every oss. A change to
// BaroBosch.h's arithmetic that departs from the deployed reference fails here. It cannot catch a
// fault shared by both copies; the hardware cross-check in bench 'b' is what covers that.
static int32_t refB5(const Cal& c, int32_t UT) {
    int32_t X1 = (UT - (int32_t)c.ac6) * ((int32_t)c.ac5) >> 15;
    int32_t X2 = ((int32_t)c.mc << 11) / (X1 + (int32_t)c.md);
    return X1 + X2;
}
static int32_t refPressure(const Cal& c, int32_t UT, uint32_t UP, int oss) {
    int32_t B5 = refB5(c, UT);
    int32_t B6 = B5 - 4000;
    int32_t X1 = ((int32_t)c.b2 * ((B6 * B6) >> 12)) >> 11;
    int32_t X2 = ((int32_t)c.ac2 * B6) >> 11;
    int32_t X3 = X1 + X2;
    int32_t B3 = ((((int32_t)c.ac1 * 4 + X3) << oss) + 2) / 4;
    X1 = ((int32_t)c.ac3 * B6) >> 13;
    X2 = ((int32_t)c.b1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    uint32_t B4 = ((uint32_t)c.ac4 * (uint32_t)(X3 + 32768)) >> 15;
    uint32_t B7 = ((uint32_t)UP - B3) * (uint32_t)(50000UL >> oss);
    int32_t p = (B7 < 0x80000000) ? (int32_t)((B7 * 2) / B4) : (int32_t)((B7 / B4) * 2);
    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    return p + ((X1 + X2 + (int32_t)3791) >> 4);
}

void test_compensation_matches_the_deployed_reference_on_a_grid() {
    const int32_t uts[] = {20000, 27898, 35000, 45000};
    const uint32_t ups[] = {20000, 23843, 34321, 50000};
    int mismatches = 0, compared = 0;
    for (int oss = 0; oss <= kOssMax; ++oss) {
        for (int32_t ut : uts) {
            for (uint32_t up : ups) {
                const Reading r = compensate(dsExampleCal(), ut, up, oss);
                const int32_t ref = refPressure(dsExampleCal(), ut, up, oss);
                ++compared;
                if (!r.converted || r.pressurePa != (float)ref) ++mismatches;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, mismatches, "BaroBosch.h diverged from the deployed reference");
    TEST_ASSERT_EQUAL_INT(64, compared);  // 4 UT x 4 UP x 4 oss
}

// Pressure is strictly monotonic in UP over the sane band (B7 does not wrap below B3), and one
// physical pressure is reached at UP = 34321 << oss — the same 2x-per-oss scaling as assembleUp,
// which is what makes oss=0 and oss=3 agree. Bisection brackets per oss; the whole-scale sweep is
// NOT asserted monotonic because UP below B3 wraps unsigned into nonsense on purpose (next test).
void test_pressure_is_monotonic_and_oss_scale_invariant() {
    const Cal c = dsExampleCal();
    int32_t prev = -0x7FFFFFFF;
    int violations = 0;
    for (uint32_t up = 20000; up <= 50000; up += 25) {
        const Reading r = compensate(c, 27898, up, 0);
        TEST_ASSERT_TRUE(r.converted);
        if (r.pressurePa <= prev) ++violations;
        prev = r.pressurePa;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, violations, "non-monotonic pressure inside the sane band");

    float at_oss0 = 0.0f;
    for (int oss = 0; oss <= kOssMax; ++oss) {
        const int32_t hi0 = (1 << (16 + oss)) - 1;  // per-oss full scale (16..19 bit), DS 5.7
        int32_t lo = 0, hi = hi0;
        for (int i = 0; i < 40; ++i) {
            const int32_t mid = lo + (hi - lo) / 2;
            if (compensate(c, 27898, (uint32_t)mid, oss).pressurePa < 101300.0f) lo = mid; else hi = mid;
        }
        const Reading r = compensate(c, 27898, (uint32_t)lo, oss);
        TEST_ASSERT_TRUE_MESSAGE(r.inRange, "1013 mbar point must be inside the DS range flag");
        TEST_ASSERT_FLOAT_WITHIN(30.0f, 101300.0f, r.pressurePa);
        if (oss == 0) at_oss0 = r.pressurePa;
        // One physical pressure at every oss: the 2x-per-oss UP scaling and the (50000>>oss)
        // scaling cancel. If either half drifts alone, this is where it shows.
        TEST_ASSERT_FLOAT_WITHIN(2.0f, at_oss0, r.pressurePa);
        // Measured anchors: 34321 / 68643 / 137286 / 274572 (UP for ~1013 mbar).
        TEST_ASSERT_INT32_WITHIN(4, 34321 << oss, lo);
    }
}

// Outside the DS 300-1100 hPa window the reading must be FLAGGED, not extrapolated (risk 1).
// Railed ADC inputs drive B7 through the unsigned wrap and produce nonsense — the flag carries
// that; the dive path is what refuses.
void test_out_of_range_results_are_flagged_not_hidden() {
    const Cal c = dsExampleCal();
    const Reading lo = compensate(c, 27898, 0, 2);
    const Reading hi = compensate(c, 27898, 262143, 2);
    TEST_ASSERT_TRUE(lo.converted);
    TEST_ASSERT_TRUE(hi.converted);
    TEST_ASSERT_FALSE(lo.inRange);
    TEST_ASSERT_FALSE(hi.inRange);
    // And the DS range endpoints themselves bracket real readings.
    TEST_ASSERT_FLOAT_WITHIN(30.0f, 101300.0f, compensate(c, 27898, 137286, 2).pressurePa);
}

// A corrupt cal ROM must produce NO number at all: converted=false, zeros, no divide-by-zero.
void test_garbage_cal_rom_is_refused_before_arithmetic() {
    Cal z = dsExampleCal();
    z.ac1 = 0;  // caught by the DS rule
    const Reading rz = compensate(z, 27898, 23843, 0);
    TEST_ASSERT_FALSE(rz.converted);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rz.pressurePa);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rz.tempC);

    Cal f = dsExampleCal();
    f.md = static_cast<int16_t>(0xFFFF);
    TEST_ASSERT_FALSE(compensate(f, 27898, 23843, 0).converted);

    // den == 0 exactly (md = -X1 for this UT) with an otherwise-valid ROM: b5FromUt returns 0
    // instead of trapping, and the chain still runs without crashing.
    Cal d = dsExampleCal();
    d.md = -4743;  // X1 for UT=27898 with this ROM is 4743
    TEST_ASSERT_EQUAL_INT(0, b5FromUt(d, 27898));
    const Reading rd = compensate(d, 27898, 23843, 0);
    TEST_ASSERT_TRUE(rd.b5 == 0 || !rd.inRange);  // either way: no trustworthy number
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_pressure_commands_and_conversion_times_match_the_datasheet);
    RUN_TEST(test_cal_rom_rule_is_the_datasheet_rule_not_a_value_check);
    RUN_TEST(test_assembleup_bit_alignment_scales_with_oss);
    RUN_TEST(test_datasheet_example_reproduces_exactly);
    RUN_TEST(test_compensation_matches_the_deployed_reference_on_a_grid);
    RUN_TEST(test_pressure_is_monotonic_and_oss_scale_invariant);
    RUN_TEST(test_out_of_range_results_are_flagged_not_hidden);
    RUN_TEST(test_garbage_cal_rom_is_refused_before_arithmetic);
    return UNITY_END();
}
