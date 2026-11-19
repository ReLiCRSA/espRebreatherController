// Gas limits, inspired-gas resolution and O2 exposure. The expected numbers here are hand
// computed from the published formulas (MOD = ppO2max/FO2, ideal-gas density, etc.) so a
// regression in the code shows up as a failed assertion rather than a plausible-looking screen.
#include <unity.h>

#include <math.h>
#include <string.h>

#include "GasMix.h"
#include "GasModel.h"
#include "O2Toxicity.h"

void setUp() {}
void tearDown() {}

static const float SURF = 1.013f;
static const float MPB_SEAWATER = 10.0f; // deliberately round so expected values are checkable

static GasMix mix(const char* n, float fo2, float fhe) {
    GasMix g{};
    strncpy(g.name, n, sizeof(g.name) - 1);
    g.fo2 = fo2;
    g.fhe = fhe;
    return g;
}

// --- depth/pressure conversion -------------------------------------------------------------

void test_metres_per_bar() {
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 10.197f, metresPerBar(1000.0f)); // freshwater
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 9.948f, metresPerBar(1025.0f));  // seawater
    // Seawater is denser, so a bar of head is fewer metres than freshwater.
    TEST_ASSERT_TRUE(metresPerBar(1025.0f) < metresPerBar(1000.0f));
}

void test_ambient_and_depth_round_trip() {
    const float amb = ambientAtDepth(30.0f, SURF, MPB_SEAWATER);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.013f, amb);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, depthForAmbient(amb, SURF, MPB_SEAWATER));
}

// --- hyperoxic MOD -------------------------------------------------------------------------

void test_mod_air() {
    GasLimits l{};
    l.ppO2MaxBar = 1.60f;
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    // 1.60 / 0.21 = 7.619 bar abs -> (7.619 - 1.013) * 10 = 66.1 m
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 7.619f, modPressure(air, l));
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 66.1f, depthForAmbient(modPressure(air, l), SURF, MPB_SEAWATER));
}

void test_mod_ena_32() {
    GasLimits l{};
    l.ppO2MaxBar = 1.60f;
    const GasMix ean32 = mix("EAN32", 0.32f, 0.0f);
    // 1.60 / 0.32 = 5.00 bar abs -> 39.9 m, the textbook 40 m MOD.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, modPressure(ean32, l));
    const GasAssessment a = assessGas(ean32, 38.0f, ambientAtDepth(38.0f, SURF, MPB_SEAWATER),
                                      20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 39.87f, a.modM);
    TEST_ASSERT_TRUE(a.modOk);
    const GasAssessment b = assessGas(ean32, 45.0f, ambientAtDepth(45.0f, SURF, MPB_SEAWATER),
                                      20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_FALSE(b.modOk);
    TEST_ASSERT_FALSE(b.selectable);
}

// --- hypoxic floor (AGENTS.md 7: enforced, not just displayed) -------------------------------

void test_hypoxic_floor_trimix() {
    GasLimits l{};
    l.ppO2MinBar = 0.18f;
    const GasMix tx = mix("TX 10/80", 0.10f, 0.80f);
    // 0.18 / 0.10 = 1.8 bar abs -> 7.9 m: shallower than this the diver is hypoxic.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.8f, hypoxicFloorPressure(tx, l));
    const GasAssessment shallow =
        assessGas(tx, 3.0f, ambientAtDepth(3.0f, SURF, MPB_SEAWATER), 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_FALSE(shallow.hypoxicOk);
    TEST_ASSERT_FALSE(shallow.selectable);
    const GasAssessment deep =
        assessGas(tx, 20.0f, ambientAtDepth(20.0f, SURF, MPB_SEAWATER), 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_TRUE(deep.hypoxicOk);
}

void test_air_is_not_hypoxic_at_surface() {
    // A guard against a sign error: 0.21 at 1 bar must stay breathable.
    GasLimits l{};
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    const GasAssessment a =
        assessGas(air, 0.0f, 1.0f, 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_TRUE(a.hypoxicOk);
    TEST_ASSERT_TRUE(a.selectable);
}

// --- END -----------------------------------------------------------------------------------

void test_end_of_trimix() {
    GasLimits l{};
    l.heNarcFactor = 0.60f;
    const GasMix tx = mix("TX 21/35", 0.21f, 0.35f);
    const float amb = ambientAtDepth(60.0f, SURF, MPB_SEAWATER); // 7.013
    // (0.44 + 0.6*0.35)/0.79 * 7.013 = 5.77 bar -> 47.6 m END for a 60 m dive.
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.77f, endPressure(tx, amb, l));
    const GasAssessment a = assessGas(tx, 60.0f, amb, 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 47.6f, a.endM);

    // Air's END is its own depth, to within the FN2 rounding (0.79 vs 0.21+0.79=1.00).
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    const float endAir = endPressure(air, amb, l);
    TEST_ASSERT_TRUE(fabsf(endAir - amb) < 0.02f * amb);
}

// --- gas density (HPNS / work of breathing) ------------------------------------------------

void test_density_matches_known_values() {
    GasLimits l{};
    l.densityMaxGL = 6.0f;
    const float amb60 = ambientAtDepth(60.0f, SURF, MPB_SEAWATER); // 7.013
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    // Air at 60 m / 20 C is the well-known ~8.3 g/L, far over the limit.
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 8.3f, gasDensityGL(air, amb60, 20.0f));
    TEST_ASSERT_FALSE(gasDensityGL(air, amb60, 20.0f) <= l.densityMaxGL);

    const GasMix tx = mix("TX 18/45", 0.18f, 0.45f);
    const float amb90 = ambientAtDepth(90.0f, SURF, MPB_SEAWATER);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 7.4f, gasDensityGL(tx, amb90, 20.0f));
    // Helium helps, but not enough at 90 m: the limit must still refuse it.
    TEST_ASSERT_FALSE(assessGas(tx, 90.0f, amb90, 20.0f, SURF, MPB_SEAWATER, l).densityOk);
    // At 60 m the same mix is inside the limit.
    TEST_ASSERT_TRUE(assessGas(tx, 60.0f, amb60, 20.0f, SURF, MPB_SEAWATER, l).densityOk);
    // Cold gas is denser: breathing it chilled makes it worse, not better.
    TEST_ASSERT_TRUE(gasDensityGL(tx, amb90, 4.0f) > gasDensityGL(tx, amb90, 20.0f));
}

// --- best mix ------------------------------------------------------------------------------

void test_best_mix_picks_richest_legal_fo2() {
    GasLimits l{};
    const GasMix gases[4] = {mix("AIR", 0.21f, 0.0f), mix("EAN50", 0.50f, 0.0f),
                             mix("TX 18/45", 0.18f, 0.45f), mix("O2", 1.0f, 0.0f)};
    const float amb10 = ambientAtDepth(10.0f, SURF, MPB_SEAWATER);
    const int i = bestMixIndex(gases, 4, 10.0f, amb10, 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_EQUAL_INT(1, i); // EAN50: MOD 21.9 m, the richest legal mix at 10 m

    const float amb60 = ambientAtDepth(60.0f, SURF, MPB_SEAWATER);
    const int j = bestMixIndex(gases, 4, 60.0f, amb60, 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_EQUAL_INT(2, j); // air's MOD covers 60 m but its density is 8.3 g/L: refused

    const float amb2 = ambientAtDepth(2.0f, SURF, MPB_SEAWATER);
    const int k = bestMixIndex(gases, 4, 2.0f, amb2, 20.0f, SURF, MPB_SEAWATER, l);
    TEST_ASSERT_EQUAL_INT(3, k); // O2's MOD is 5.9 m: at 2 m it is the richest legal mix
}

void test_gas_validation() {
    char why[64];
    TEST_ASSERT_TRUE(gasMixValid(mix("TX 21/35", 0.21f, 0.35f), why, sizeof(why)));
    TEST_ASSERT_FALSE(gasMixValid(mix("BAD", 0.05f, 0.0f), why, sizeof(why)));  // fo2 < 0.10
    TEST_ASSERT_FALSE(gasMixValid(mix("BAD", 0.60f, 0.60f), why, sizeof(why))); // sums > 1
    TEST_ASSERT_FALSE(gasMixValid(mix("", 0.21f, 0.0f), why, sizeof(why)));        // empty name
    TEST_ASSERT_FALSE(gasMixValid(mix("O2<b>", 0.21f, 0.0f), why, sizeof(why)));  // markup
}

// --- inspired gas: the CCR/OC switch must change tissue loading ----------------------------

void test_setpoint_crossover() {
    GasModelConfig c{};
    c.spDeepBar = 0.70f;
    c.spShallowBar = 1.30f;
    c.spCrossoverDepthM = 21.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.30f, setpointAt(c, 12.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.70f, setpointAt(c, 30.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.30f, setpointAt(c, 21.0f)); // crossover itself: shallow sp
}

void test_ccr_inspired_pressures_sum_to_ambient() {
    GasModelConfig c{};
    c.pCO2Bar = 0.053f;
    c.pH2OBar = 0.0627f;
    c.spDeepBar = 0.70f;
    const GasMix dil = mix("TX 18/45", 0.18f, 0.45f);
    const GasMix o2 = mix("O2", 1.0f, 0.0f);
    const float amb = ambientAtDepth(40.0f, SURF, MPB_SEAWATER); // 5.013

    const Inspired i = resolveInspired(BreathingState::CCR_CLOSED, dil, o2, dil, dil, amb, 40.0f, c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.70f, i.po2);
    // rest = 5.013 - 0.7 - 0.053 - 0.0627 = 4.1973, split by the diluent's 0.37/0.45 inert ratio
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.894f, i.pn2);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 2.303f, i.phe);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, amb, i.po2 + i.pn2 + i.phe + c.pCO2Bar + c.pH2OBar);
}

void test_mode_switch_changes_inert_loading() {
    GasModelConfig c{};
    const GasMix dil = mix("AIR", 0.21f, 0.0f);
    const GasMix o2 = mix("O2", 1.0f, 0.0f);
    const float amb = ambientAtDepth(21.0f, SURF, MPB_SEAWATER); // 3.113

    const Inspired ccr =
        resolveInspired(BreathingState::CCR_CLOSED, dil, o2, dil, dil, amb, 21.0f, c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.30f, ccr.po2); // shallow setpoint
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.697f, ccr.pn2);

    const Inspired oc = resolveInspired(BreathingState::OC_OPEN, dil, o2, dil, dil, amb, 21.0f, c);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 2.410f, oc.pn2);
    TEST_ASSERT_TRUE(oc.pn2 > ccr.pn2 + 0.5f); // the switch alone changes the tissue driver

    // A diluent flush loads the tissues with the diluent, not the setpoint.
    const GasMix tx = mix("TX 10/70", 0.10f, 0.70f);
    const Inspired flush =
        resolveInspired(BreathingState::CCR_DILUENT_FLUSH, tx, o2, tx, tx, amb, 21.0f, c);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.305f, flush.po2);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.610f, flush.pn2);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 2.135f, flush.phe);
}

void test_bailout_uses_bailout_gas_not_diluent() {
    GasModelConfig c{};
    const GasMix dil = mix("TX 10/70", 0.10f, 0.70f);
    const GasMix o2 = mix("O2", 1.0f, 0.0f);
    const GasMix bail = mix("EAN50", 0.50f, 0.0f);
    const float amb = ambientAtDepth(30.0f, SURF, MPB_SEAWATER);
    const Inspired i =
        resolveInspired(BreathingState::MANUAL_BAILOUT, dil, o2, bail, dil, amb, 30.0f, c);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.975f, i.po2);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.975f, i.pn2);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, i.phe); // no helium on the escape gas
}

// --- CNS / OTU -----------------------------------------------------------------------------

void test_cns_single_exposure_table() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 240.0f, O2Toxicity::singleExposureLimitMin(1.6f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, O2Toxicity::singleExposureLimitMin(1.4f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1800.0f, O2Toxicity::singleExposureLimitMin(0.6f));
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 1350.0f, O2Toxicity::singleExposureLimitMin(0.75f)); // interp
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, O2Toxicity::singleExposureLimitMin(1.7f));    // no limit
}

void test_cns_accumulates() {
    O2Toxicity o;
    // 120 min at 1.6 bar = 50% of the 240 min limit; washout over 120 min removes ~37% of what
    // accumulated, so the total must sit below 50% but well above 25%.
    for (int i = 0; i < 120; ++i) o.step(1.6f, 60.0f, 125.0f);
    const float cns = o.state().cnsPercent;
    TEST_ASSERT_TRUE(cns > 25.0f);
    TEST_ASSERT_TRUE(cns < 50.0f);

    // Nothing accumulates below the 0.5 bar threshold, and it cannot go backwards.
    O2Toxicity q;
    q.step(0.21f, 3600.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, q.state().cnsPercent);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, q.state().otu);
}

void test_cns_washes_out() {
    O2Toxicity o;
    o.step(1.6f, 60.0f, 125.0f); // small dose
    const float after = o.state().cnsPercent;
    for (int i = 0; i < 125; ++i) o.step(0.21f, 60.0f, 125.0f); // one half-time of air
    TEST_ASSERT_TRUE(o.state().cnsPercent < after * 0.55f);
    TEST_ASSERT_TRUE(o.state().cnsPercent > after * 0.45f);
}

void test_otu_accumulates() {
    O2Toxicity o;
    o.step(1.6f, 3600.0f, 0.0f); // no washout term for OTU
    // 60 min * (1.6/0.5)^1.318 = 60 * 4.13 = ~248 OTU
    TEST_ASSERT_TRUE(o.state().otu > 200.0f);
    TEST_ASSERT_TRUE(o.state().otu < 300.0f);
    O2Toxicity z;
    z.step(0.4f, 3600.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, z.state().otu);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_metres_per_bar);
    RUN_TEST(test_ambient_and_depth_round_trip);
    RUN_TEST(test_mod_air);
    RUN_TEST(test_mod_ena_32);
    RUN_TEST(test_hypoxic_floor_trimix);
    RUN_TEST(test_air_is_not_hypoxic_at_surface);
    RUN_TEST(test_end_of_trimix);
    RUN_TEST(test_density_matches_known_values);
    RUN_TEST(test_best_mix_picks_richest_legal_fo2);
    RUN_TEST(test_gas_validation);
    RUN_TEST(test_setpoint_crossover);
    RUN_TEST(test_ccr_inspired_pressures_sum_to_ambient);
    RUN_TEST(test_mode_switch_changes_inert_loading);
    RUN_TEST(test_bailout_uses_bailout_gas_not_diluent);
    RUN_TEST(test_cns_single_exposure_table);
    RUN_TEST(test_cns_accumulates);
    RUN_TEST(test_cns_washes_out);
    RUN_TEST(test_otu_accumulates);
    return UNITY_END();
}
