// Depth arithmetic tests (lib/core/src/Depth.h) — every anchor hand-computed.
//
// This suite is also where a 10x error in the design record dies (AGENTS.md decision 38): the
// record asserted "BMP085 fallback ~9 m usable depth" for years of design text, while the
// arithmetic (1100 mbar absolute − ~1013 mbar surface = ~87 mbar gauge, and 1 mbar supports
// ~1.02 cm of water) gives UNDER 1 m at sea level. The error class is risk 21 again — a
// plausible number asserted, never derived. The tests below are the derivation, so the claim
// can never silently outrun the arithmetic again.
#include "Depth.h"
#include "unity.h"

using namespace depth;

void setUp() {}
void tearDown() {}

// hand: 1e5 / (1000 x 9.80665) = 10.19717 m/bar fresh; 1e5 / (1025 x 9.80665) = 9.94845 salt.
// Cross-check the record's own anchor: 10 m seawater = 0.9806 bar ("10 m per bar" to <0.5%).
void test_meters_per_bar_is_hydrostatics_not_folklore() {
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.19717f, metersPerBar(1.000f));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 9.94845f, metersPerBar(1.025f));
    // the ratio must be exactly the specific gravities
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.025f, metersPerBar(1.000f) / metersPerBar(1.025f));
    // 10 m of salt water = 1.00518 bar gauge: the diver-rule "10 m/bar", verified not assumed.
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.00518f, 10.0f / metersPerBar(1.025f));
}

// The BMP180 fallback ceiling, DERIVED: (1.100 - psurf) bar x 9.94845 m/bar.
// Sea-level reference (1013.25 mbar): 0.08675 x 9.94845 = 0.863 m — under 1 m.
// Stockholm bench (learned 977.8 mbar 2026-11-20): 0.1222 x 9.94845 = 1.216 m.
// Anything in this repo that says "9 m" for this part is quoting 10x that number from nowhere;
// this test is the counter-proof and the reason the banner prints a computed ceiling now.
void test_bmp180_fallback_ceiling_is_under_1m_at_sea_level() {
    const float sea = ceilingMeters(1.01325f, 1.1f, 1.025f);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.863f, sea);
    TEST_ASSERT_TRUE(sea < 1.0f);  // the correction, asserted
    const float bench = ceilingMeters(0.9778f, 1.1f, 1.025f);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.216f, bench);
    // and the MS5837 side for contrast: (30.0 - 0.9778) x 9.94845 = 288.73 m — the ~300 m claim
    // in AGENTS.md 2 survives the same arithmetic.
    TEST_ASSERT_FLOAT_WITHIN(1e-1f, 288.73f, ceilingMeters(0.9778f, 30.0f, 1.025f));
}

// Psurf learning: N=16 samples AND a sigma gate, per the lock rule. Hand-computed transient:
// 15 samples at 0.9778 and one at 1.0100 -> mean 0.979813, sigma 7.8 mbar >> 2 mbar gate,
// so no lock. Sixteen steady samples after that replace the window and it locks at 0.9778.
void test_psurf_learner_locks_on_steady_data_only() {
    SurfaceLearner l;
    for (int i = 0; i < 15; ++i) {
        TEST_ASSERT_FALSE(l.push(0.9778f));
        TEST_ASSERT_FALSE(l.locked());
    }
    TEST_ASSERT_FALSE(l.push(1.0100f));  // 16th sample: window full, sigma too big
    TEST_ASSERT_FALSE(l.locked());
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0078f, l.sigmaBar());
    TEST_ASSERT_EQUAL_UINT32(16, l.samples());
    for (int i = 0; i < 15; ++i) TEST_ASSERT_FALSE(l.push(0.9778f));  // window still mixed
    TEST_ASSERT_TRUE(l.push(0.9778f));                                // 32nd: window all steady
    TEST_ASSERT_TRUE(l.locked());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9778f, l.psurfBar());
    // a wild sample after locking must not move a locked Psurf (the surface was set at the surface)
    TEST_ASSERT_TRUE(l.push(1.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9778f, l.psurfBar());
}

void test_measure_gauge_depth_is_amb_minus_learned_psurf() {
    const Result r = measure(1.0778f, 0.9778f, 1.025f, 1.1f);
    TEST_ASSERT_FALSE(r.clamped);
    TEST_ASSERT_FALSE(r.aboveSurface);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.1f, r.gaugeBar);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.99484f, r.depthM);  // 0.1 bar x 9.94845
}

// Risk 1 in code: past the part's absolute limit the number STOPS (ceiling value + clamped
// flag) and the raw gauge delta is still visible beside it — clamping may hide an extrapolation,
// it must never hide that clamping happened.
void test_clamp_stops_at_the_parts_limit_and_says_so() {
    const Result r = measure(1.1200f, 0.9778f, 1.025f, 1.1f);
    TEST_ASSERT_TRUE(r.clamped);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.21570f, r.depthM);  // ceiling, not the raw 1.41467
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.14220f, r.gaugeBar);  // raw delta survives, unclamped
    const Result ok = measure(1.0999f, 0.9778f, 1.025f, 1.1f);
    TEST_ASSERT_FALSE(ok.clamped);
    // range unknown (0) disables the clamp — bench-only path, asserted so it stays deliberate
    const Result nb = measure(1.1200f, 0.9778f, 1.025f, 0.0f);
    TEST_ASSERT_FALSE(nb.clamped);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.41467f, nb.depthM);
}

void test_above_surface_is_reported_not_wrapped() {
    const Result r = measure(0.9770f, 0.9778f, 1.025f, 1.1f);
    TEST_ASSERT_TRUE(r.aboveSurface);
    TEST_ASSERT_TRUE(r.depthM < 0.0f);
    TEST_ASSERT_FALSE(r.clamped);  // a wave crest is not a sensor range fault
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_meters_per_bar_is_hydrostatics_not_folklore);
    RUN_TEST(test_bmp180_fallback_ceiling_is_under_1m_at_sea_level);
    RUN_TEST(test_psurf_learner_locks_on_steady_data_only);
    RUN_TEST(test_measure_gauge_depth_is_amb_minus_learned_psurf);
    RUN_TEST(test_clamp_stops_at_the_parts_limit_and_says_so);
    RUN_TEST(test_above_surface_is_reported_not_wrapped);
    return UNITY_END();
}
