// ZHL-16C + gradient factors. These tests are behavioural rather than table-driven: they assert
// the invariants a deco model must satisfy (exact Haldane exponential, no obligation for a
// no-stop dive, monotonicity in depth/time/GF) and print each schedule so a human can eyeball it
// against a trusted computer. Passing assertions with implausible printed numbers is still a
// failure - read the output.
#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Zhl16c.h"

using namespace zhl;

void setUp() {}
void tearDown() {}

static const float SURF = 1.013f;

static GasMix mix(const char* n, float fo2, float fhe) {
    GasMix g{};
    strncpy(g.name, n, sizeof(g.name) - 1);
    g.fo2 = fo2;
    g.fhe = fhe;
    return g;
}

static Config baseCfg() {
    Config c{};
    c.surfaceBar = SURF;
    c.mPerBar = 10.0f; // round, so printed depths are checkable by hand
    c.ascentMPerMin = 10.0f;
    c.gfLow = 0.30f;
    c.gfHigh = 0.70f;
    c.icdGuard = true;
    c.icdRatio = 0.80f;
    c.fastCompMaxHalfTimeMin = 40.0f;
    c.stopStepM = 3.0f;
    c.lastStopM = 6.0f;
    return c;
}

static AscentGas openCircuit(const GasMix& g) {
    AscentGas a{};
    a.state = BreathingState::OC_OPEN;
    a.selected = g;
    a.diluent = g;
    a.bailout = g;
    return a;
}

static AscentGas closedCircuit(const GasMix& dil, float spShallow, float spDeep) {
    AscentGas a{};
    a.state = BreathingState::CCR_CLOSED;
    a.diluent = dil;
    a.selected = dil;
    a.oxygen = mix("O2", 1.0f, 0.0f);
    a.bailout = mix("BAIL 21/0", 0.21f, 0.0f);
    a.gas.spShallowBar = spShallow;
    a.gas.spDeepBar = spDeep;
    a.gas.spCrossoverDepthM = 21.0f;
    return a;
}

// Hold a constant depth for N minutes. The descent is deliberately ignored: NDL and ceiling
// questions are asked at the bottom, not during the drop.
static void hold(Engine& e, float depthM, float minutes, const AscentGas& g, const Config& c) {
    const float amb = e.ambAt(depthM, c);
    const Inspired i =
        resolveInspired(g.state, g.diluent, g.oxygen, g.bailout, g.selected, amb, depthM, g.gas);
    const int steps = (int)(minutes * 60.0f / 10.0f);
    for (int k = 0; k < steps; ++k) e.step(i, amb, 10.0f, c);
}

static void printPlan(const char* label, const DecoPlan& p) {
    printf("  %-22s ceiling %5.1f m  TTS %6.1f min  comp %2d  gfLowP %.2f bar  stops:", label,
           p.ceilingM, p.ttsMin, p.controllingComp, p.gfLowPressureBar);
    for (int i = 0; i < p.nStops; ++i) printf(" %g/%.0f", p.stopDepthM[i], p.stopMin[i]);
    printf("\n");
}

// --- the Haldane equation itself ------------------------------------------------------------

void test_compartment_kinematics_are_exact() {
    // Single-compartment check: p(t) = pi + (p0 - pi) * exp(-ln2 * t / ht).
    const Config c = baseCfg();
    Engine e;
    e.reset(SURF);
    float n2[NCOMP], he[NCOMP];
    for (int i = 0; i < NCOMP; ++i) { n2[i] = 0.0f; he[i] = 0.0f; }
    e.setTissues(n2, he);

    Inspired insp{};
    insp.po2 = 0.21f;
    insp.pn2 = 1.0f; // drive nitrogen to exactly 1 bar
    insp.phe = 0.0f;
    for (int k = 0; k < 30; ++k) e.step(insp, SURF, 10.0f, c); // 300 s

    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.5f, e.n2()[0]);    // 5 min half-time -> exactly half way
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.3516f, e.n2()[1]); // 8 min -> 1 - 2^(-300/480)
    TEST_ASSERT_TRUE(e.n2()[15] < 0.01f);               // 635 min: barely moved
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, Engine::halfTimeN2Min(0));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.88f, Engine::halfTimeHeMin(0));
    TEST_ASSERT_TRUE(Engine::halfTimeHeMin(15) < Engine::halfTimeN2Min(15)); // this parameterisation
}

// --- no obligation cases --------------------------------------------------------------------

void test_fresh_engine_has_no_ceiling() {
    const Config c = baseCfg();
    Engine e;
    e.reset(SURF);
    const DecoPlan p = e.plan(openCircuit(mix("AIR", 0.21f, 0.0f)), SURF, c);
    TEST_ASSERT_FALSE(p.inDeco);
    TEST_ASSERT_FALSE(p.violation);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, p.ceilingM);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, p.ttsMin);
    TEST_ASSERT_EQUAL_INT(0, p.nStops);
}

void test_shallow_long_dive_needs_no_stops() {
    const Config c = baseCfg();
    Engine e;
    e.reset(SURF);
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));
    hold(e, 10.0f, 60.0f, air, c);
    DecoPlan p = e.plan(air, e.ambAt(10.0f, c), c);
    printPlan("10 m / 60 min air", p);
    TEST_ASSERT_FALSE(p.inDeco);
    TEST_ASSERT_TRUE(p.ttsMin < 2.0f); // ascent time only
    TEST_ASSERT_EQUAL_INT(0, p.nStops);
}

void test_ndl_is_unlimited_shallow_and_short_deep() {
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));

    Engine a; a.reset(SURF);
    const float ndl10 = a.ndlMin(air, a.ambAt(10.0f, c), c);
    printf("  NDL 10 m air = %.1f min\n", ndl10);
    TEST_ASSERT_TRUE(ndl10 > 60.0f); // a long shallow dive: no-stop limit is far away, not absent

    Engine b; b.reset(SURF);
    const float ndl50 = b.ndlMin(air, b.ambAt(50.0f, c), c);
    printf("  NDL 50 m air = %.1f min\n", ndl50);
    TEST_ASSERT_TRUE(ndl50 > 0.0f);
    TEST_ASSERT_TRUE(ndl50 < 25.0f);

    Engine d; d.reset(SURF);
    Engine f; f.reset(SURF);
    const float ndl40 = d.ndlMin(air, d.ambAt(40.0f, c), c);
    const float ndl30 = f.ndlMin(air, f.ambAt(30.0f, c), c);
    printf("  NDL 40 m air = %.1f min, 30 m air = %.1f min\n", ndl40, ndl30);
    TEST_ASSERT_TRUE(ndl30 > ndl40);
    TEST_ASSERT_TRUE(ndl40 > ndl50);
}

void test_ndl_responds_to_the_breathing_state_not_just_depth() {
    // Same depth, same diluent, different setpoint. In a CCR a richer setpoint *displaces*
    // inert gas at that depth, so the tissues load more slowly and the no-stop limit is longer.
    const Config c = baseCfg();
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    const AscentGas lean = closedCircuit(air, 0.70f, 0.70f);
    const AscentGas rich = closedCircuit(air, 1.60f, 1.60f);

    Engine e1; e1.reset(SURF);
    Engine e2; e2.reset(SURF);
    const float amb = e1.ambAt(40.0f, c);
    const float ndlLean = e1.ndlMin(lean, amb, c);
    const float ndlRich = e2.ndlMin(rich, amb, c);
    printf("  NDL 40 m CCR sp0.7 = %.1f min, sp1.6 = %.1f min\n", ndlLean, ndlRich);
    TEST_ASSERT_TRUE(ndlRich > ndlLean);
}

// --- decompression obligation ---------------------------------------------------------------

void test_50m_20min_air_requires_deco() {
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));
    Engine e;
    e.reset(SURF);
    hold(e, 50.0f, 20.0f, air, c);
    DecoPlan p = e.plan(air, e.ambAt(50.0f, c), c);
    printPlan("50 m / 20 min air", p);
    TEST_ASSERT_TRUE(p.inDeco);
    TEST_ASSERT_FALSE(p.violation);
    TEST_ASSERT_FALSE(p.bounded);
    TEST_ASSERT_TRUE(p.ceilingM > 3.0f);
    TEST_ASSERT_TRUE(p.ceilingM < 50.0f);
    TEST_ASSERT_TRUE(p.nStops > 0);
    TEST_ASSERT_TRUE(p.ttsMin > 5.0f);
    TEST_ASSERT_TRUE(p.ttsMin < 90.0f);
    TEST_ASSERT_TRUE(p.controllingComp >= 0 && p.controllingComp < NCOMP);
    TEST_ASSERT_TRUE(p.gfLowPressureBar > SURF);
}

void test_tts_grows_with_depth_and_with_bottom_time() {
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));

    Engine a; a.reset(SURF); hold(a, 40.0f, 20.0f, air, c);
    Engine b; b.reset(SURF); hold(b, 50.0f, 20.0f, air, c);
    const DecoPlan pa = a.plan(air, a.ambAt(40.0f, c), c);
    const DecoPlan pb = b.plan(air, b.ambAt(50.0f, c), c);
    printPlan("40 m / 20 min air", pa);
    printPlan("50 m / 20 min air", pb);
    TEST_ASSERT_TRUE(pb.ttsMin > pa.ttsMin);
    TEST_ASSERT_TRUE(pb.ceilingM >= pa.ceilingM);

    Engine d; d.reset(SURF); hold(d, 50.0f, 35.0f, air, c);
    const DecoPlan pd = d.plan(air, d.ambAt(50.0f, c), c);
    printPlan("50 m / 35 min air", pd);
    TEST_ASSERT_TRUE(pd.ttsMin > pb.ttsMin);
}

void test_conservative_gradients_give_a_longer_tts() {
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));

    Config loose = baseCfg();
    loose.gfLow = 0.40f;
    loose.gfHigh = 0.80f;
    Engine e1; e1.reset(SURF); hold(e1, 50.0f, 20.0f, air, loose);
    const DecoPlan p1 = e1.plan(air, e1.ambAt(50.0f, loose), loose);

    Config tight = baseCfg();
    tight.gfLow = 0.20f;
    tight.gfHigh = 0.45f;
    Engine e2; e2.reset(SURF); hold(e2, 50.0f, 20.0f, air, tight);
    const DecoPlan p2 = e2.plan(air, e2.ambAt(50.0f, tight), tight);

    printPlan("50/20 air GF40/80", p1);
    printPlan("50/20 air GF20/45", p2);
    TEST_ASSERT_TRUE(p2.ttsMin >= p1.ttsMin);
    TEST_ASSERT_TRUE(p2.ceilingM >= p1.ceilingM);
}

void test_stops_sit_on_the_configured_grid() {
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));
    Engine e;
    e.reset(SURF);
    hold(e, 50.0f, 25.0f, air, c);
    const DecoPlan p = e.plan(air, e.ambAt(50.0f, c), c);
    printPlan("50 m / 25 min air", p);
    TEST_ASSERT_TRUE(p.nStops > 0);
    float stopTotal = 0.0f;
    for (int i = 0; i < p.nStops; ++i) {
        TEST_ASSERT_TRUE(p.stopMin[i] > 0.0f);
        TEST_ASSERT_TRUE(p.stopDepthM[i] >= c.lastStopM - 0.001f);
        const float rem = fabsf(p.stopDepthM[i] / c.stopStepM - roundf(p.stopDepthM[i] / c.stopStepM));
        TEST_ASSERT_TRUE(rem < 0.01f);
        stopTotal += p.stopMin[i];
    }
    // The plan walks the whole ascent, so TTS includes stop time plus the travel.
    TEST_ASSERT_TRUE(p.ttsMin >= stopTotal);
}

void test_violation_when_ascending_above_the_ceiling() {
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));
    Engine e;
    e.reset(SURF);
    hold(e, 50.0f, 20.0f, air, c);
    hold(e, 12.0f, 2.0f, air, c); // jumped up far too fast
    const DecoPlan p = e.plan(air, e.ambAt(12.0f, c), c);
    printPlan("50/20 then 12 m", p);
    TEST_ASSERT_TRUE(p.inDeco);
    TEST_ASSERT_TRUE(p.violation);
    TEST_ASSERT_TRUE(p.ceilingM > 12.0f);
}

// --- gradient-factor ramp --------------------------------------------------------------------

void test_gf_ramp_is_anchored_monotonic_and_clamped() {
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));
    Engine e;
    e.reset(SURF);
    hold(e, 50.0f, 20.0f, air, c);
    const float anchor = e.gfLowPressureBar();
    printf("  gf anchor %.2f bar: gf(surface)=%.3f gf(anchor)=%.3f gf(deep)=%.3f\n", anchor,
           e.gfAt(SURF, c), e.gfAt(anchor, c), e.gfAt(anchor + 1.0f, c));
    TEST_ASSERT_TRUE(anchor > SURF);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, c.gfHigh, e.gfAt(SURF, c));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, c.gfLow, e.gfAt(anchor, c));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, c.gfLow, e.gfAt(anchor + 1.0f, c)); // clamped, never lower

    float prev = 1.1f;
    for (float p = SURF; p <= anchor + 0.5f; p += 0.25f) {
        const float g = e.gfAt(p, c);
        TEST_ASSERT_TRUE(g >= c.gfLow - 1e-5f);
        TEST_ASSERT_TRUE(g <= c.gfHigh + 1e-5f);
        // GF falls as pressure rises: gfHigh at the surface, gfLow from the anchor downward.
        TEST_ASSERT_TRUE(g <= prev + 1e-5f);
        prev = g;
    }
}

// --- helium ----------------------------------------------------------------------------------

void test_helium_dive_engages_fast_compartments() {
    const Config c = baseCfg();
    const GasMix tx = mix("TX 21/35", 0.21f, 0.35f);
    const GasMix air = mix("AIR", 0.21f, 0.0f);

    Engine e;
    e.reset(SURF);
    const AscentGas g = openCircuit(tx);
    hold(e, 45.0f, 25.0f, g, c);
    const DecoPlan p = e.plan(g, e.ambAt(45.0f, c), c);
    printPlan("45 m / 25 min 21/35", p);
    TEST_ASSERT_TRUE(p.inDeco);
    TEST_ASSERT_TRUE(p.controllingComp >= 0 && p.controllingComp < 10); // He pulls this forward

    Engine f;
    f.reset(SURF);
    const AscentGas ga = openCircuit(air);
    hold(f, 45.0f, 25.0f, ga, c);
    const DecoPlan pa = f.plan(ga, f.ambAt(45.0f, c), c);
    printPlan("45 m / 25 min air", pa);

    // Helium must actually be in the model, not just in the gas table.
    bool heLoaded = false;
    for (int i = 0; i < NCOMP; ++i)
        if (e.he()[i] > 0.5f) heLoaded = true;
    TEST_ASSERT_TRUE(heLoaded);

    // NOTE: no assertion on which of the two schedules is longer. In the ZH-L16A
    // parameterisation a trimix dive at this depth/time can demand slightly *more* deco than
    // the equivalent air dive, because the fast helium compartments reach deep first ceilings
    // quickly. Whether helium helps or hurts is model- and profile-dependent, so it is printed
    // for a human to compare rather than asserted as if it were a law.
    printf("  TTS trimix %.1f min vs air %.1f min\n", p.ttsMin, pa.ttsMin);
    TEST_ASSERT_TRUE(p.bounded == pa.bounded); // both schedules must converge, or both fail
    // The a/b blend is driven by helium, which shows up as a different controlling compartment.
    printf("  controlling comp: trimix %d vs air %d\n", p.controllingComp, pa.controllingComp);
    TEST_ASSERT_TRUE(p.controllingComp != pa.controllingComp);
}

void test_icd_guard_keeps_nitrogen_in_fast_tissues() {
    const Config c = baseCfg();
    float n2[NCOMP], he[NCOMP];
    for (int i = 0; i < NCOMP; ++i) { n2[i] = 2.3f; he[i] = 0.0f; } // saturated at ~30 m on air

    Engine guard, free;
    guard.reset(SURF); free.reset(SURF);
    guard.setTissues(n2, he);
    free.setTissues(n2, he);
    Config loose = c;
    loose.icdGuard = false;

    // Switch to heliox at the same depth: helium loads, nitrogen is now below inspired zero and
    // would wash out freely. The guard must hold nitrogen up in the fast compartments.
    Inspired insp{};
    insp.po2 = 0.5f;
    insp.pn2 = 0.0f;
    insp.phe = 3.0f;
    for (int k = 0; k < 60; ++k) {
        guard.step(insp, 4.0f, 10.0f, c);
        free.step(insp, 4.0f, 10.0f, loose);
    }

    bool anyDifference = false;
    for (int i = 0; i < NCOMP; ++i) {
        if (Engine::halfTimeN2Min(i) <= c.fastCompMaxHalfTimeMin) {
            TEST_ASSERT_TRUE(guard.n2()[i] >= free.n2()[i] - 1e-4f);
            if (guard.n2()[i] > free.n2()[i] + 1e-3f) anyDifference = true;
        } else {
            // Slow compartments are outside the guard by design.
            TEST_ASSERT_FLOAT_WITHIN(1e-4f, free.n2()[i], guard.n2()[i]);
        }
    }
    TEST_ASSERT_TRUE(anyDifference);
    printf("  ICD: comp0 N2 %.3f guarded vs %.3f unguarded\n", guard.n2()[0], free.n2()[0]);
}

// --- state persistence and CCR behaviour ------------------------------------------------------

void test_tissue_state_round_trips() {
    const Config c = baseCfg();
    Engine e;
    e.reset(SURF);
    hold(e, 40.0f, 15.0f, openCircuit(mix("AIR", 0.21f, 0.0f)), c);

    float n2[NCOMP], he[NCOMP];
    for (int i = 0; i < NCOMP; ++i) { n2[i] = e.n2()[i]; he[i] = e.he()[i]; }

    Engine f;
    f.reset(SURF);
    f.setTissues(n2, he);
    // The GF anchor is part of the persisted state: tissues without the anchor restore a
    // different ceiling, which is how a power loss would silently reset the diver's model.
    f.setGfLowPressureBar(e.gfLowPressureBar());
    for (int i = 0; i < NCOMP; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, n2[i], f.n2()[i]);
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, he[i], f.he()[i]);
    }
    // A restored model must reproduce the same obligation, otherwise a power loss silently
    // resets the diver's residual loading (AGENTS.md 19 risk 8).
    const DecoPlan p1 = e.plan(openCircuit(mix("AIR", 0.21f, 0.0f)), e.ambAt(40.0f, c), c);
    const DecoPlan p2 = f.plan(openCircuit(mix("AIR", 0.21f, 0.0f)), f.ambAt(40.0f, c), c);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p1.ceilingM, p2.ceilingM);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p1.ttsMin, p2.ttsMin);

    // And the anchor on its own must be recoverable, because TissuePersist writes it separately.
    Engine g3;
    g3.reset(SURF);
    g3.setGfLowPressureBar(e.gfLowPressureBar());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, e.gfLowPressureBar(), g3.gfLowPressureBar());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, e.gfAt(3.0f, c), g3.gfAt(3.0f, c));
}

void test_ccr_ceiling_follows_the_setpoint() {
    // The setpoint is a tissue-loading control, not just a display number: a richer setpoint
    // displaces inert gas at the same depth, so the ceiling and TTS must both come down.
    const Config c = baseCfg();
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    const AscentGas lean = closedCircuit(air, 0.70f, 0.70f);
    const AscentGas rich = closedCircuit(air, 1.60f, 1.60f);

    Engine e1; e1.reset(SURF); hold(e1, 40.0f, 20.0f, lean, c);
    Engine e2; e2.reset(SURF); hold(e2, 40.0f, 20.0f, rich, c);
    const DecoPlan p1 = e1.plan(lean, e1.ambAt(40.0f, c), c);
    const DecoPlan p2 = e2.plan(rich, e2.ambAt(40.0f, c), c);
    printPlan("40/20 CCR sp0.7", p1);
    printPlan("40/20 CCR sp1.6", p2);
    TEST_ASSERT_TRUE(p2.gfLowPressureBar < p1.gfLowPressureBar);
    TEST_ASSERT_TRUE(p2.ceilingM <= p1.ceilingM + 0.001f);
    TEST_ASSERT_TRUE(p2.ttsMin <= p1.ttsMin);
}

void test_plan_converges_across_a_spread_of_profiles() {
    // Regression guard for the search stall: a stop rung whose exit criterion was satisfied at
    // zero elapsed time repeated forever, and the plan was silently reported as unbounded.
    struct Profile { float depthM; float min; };
    const Profile profiles[] = {
        {10.0f, 30.0f},  {20.0f, 20.0f},  {30.0f, 20.0f},  {40.0f, 20.0f},  {50.0f, 20.0f},
        {56.0f, 40.0f},  {60.0f, 15.0f},  {45.0f, 45.0f},  {21.0f, 90.0f},  {33.0f, 60.0f},
        {12.0f, 240.0f}, {66.0f, 10.0f},
    };
    const Config c = baseCfg();
    const AscentGas air = openCircuit(mix("AIR", 0.21f, 0.0f));
    for (const Profile& pr : profiles) {
        Engine e;
        e.reset(SURF);
        hold(e, pr.depthM, pr.min, air, c);
        const DecoPlan p = e.plan(air, e.ambAt(pr.depthM, c), c);
        printf("  %4.0f m / %5.0f min -> ceiling %5.1f m, TTS %7.1f min, stops %d\n", pr.depthM,
               pr.min, p.ceilingM, p.ttsMin, p.nStops);
        TEST_ASSERT_FALSE_MESSAGE(p.bounded, "plan search failed to converge");
        TEST_ASSERT_TRUE_MESSAGE(p.ttsMin >= 0.0f && p.ttsMin < 1440.0f, "TTS not plausible");
        if (p.nStops > 0) {
            float sum = 0.0f;
            for (int i = 0; i < p.nStops; ++i) sum += p.stopMin[i];
            TEST_ASSERT_TRUE_MESSAGE(p.ttsMin >= sum, "TTS shorter than its own stop times");
        }
        // The stop schedule must never put a stop shallower than the configured last stop, and
        // a dive with no obligation must not invent one.
        for (int i = 0; i < p.nStops; ++i)
            TEST_ASSERT_TRUE_MESSAGE(p.stopDepthM[i] >= c.lastStopM - 0.01f, "stop below floor");
        if (p.ceilingM <= 0.0f) TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.nStops, "stops without ceiling");
    }
}

void test_last_stop_setting_moves_the_schedule_not_the_ceiling() {
    // lastStopM decides where stops may be made, and must never change the model's own ceiling:
    // hiding the real ceiling behind the stop grid is forbidden (AGENTS.md 19 risk 10).
    const GasMix air = mix("AIR", 0.21f, 0.0f);
    const AscentGas g = openCircuit(air);

    Config deep6 = baseCfg();
    deep6.lastStopM = 6.0f;
    Config deep3 = baseCfg();
    deep3.lastStopM = 3.0f;

    Engine e;
    e.reset(SURF);
    hold(e, 50.0f, 20.0f, g, deep6);
    const DecoPlan p6 = e.plan(g, e.ambAt(50.0f, deep6), deep6);
    const DecoPlan p3 = e.plan(g, e.ambAt(50.0f, deep3), deep3);
    printPlan("50/20 last stop 6 m", p6);
    printPlan("50/20 last stop 3 m", p3);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, p6.ceilingM, p3.ceilingM);
    TEST_ASSERT_TRUE(p3.ttsMin >= p6.ttsMin); // a deeper permitted last stop cannot be less work
    for (int i = 0; i < p3.nStops; ++i)
        TEST_ASSERT_TRUE(p3.stopDepthM[i] >= deep3.lastStopM - 0.01f);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_compartment_kinematics_are_exact);
    RUN_TEST(test_fresh_engine_has_no_ceiling);
    RUN_TEST(test_shallow_long_dive_needs_no_stops);
    RUN_TEST(test_ndl_is_unlimited_shallow_and_short_deep);
    RUN_TEST(test_ndl_responds_to_the_breathing_state_not_just_depth);
    RUN_TEST(test_50m_20min_air_requires_deco);
    RUN_TEST(test_tts_grows_with_depth_and_with_bottom_time);
    RUN_TEST(test_conservative_gradients_give_a_longer_tts);
    RUN_TEST(test_stops_sit_on_the_configured_grid);
    RUN_TEST(test_violation_when_ascending_above_the_ceiling);
    RUN_TEST(test_gf_ramp_is_anchored_monotonic_and_clamped);
    RUN_TEST(test_helium_dive_engages_fast_compartments);
    RUN_TEST(test_icd_guard_keeps_nitrogen_in_fast_tissues);
    RUN_TEST(test_tissue_state_round_trips);
    RUN_TEST(test_ccr_ceiling_follows_the_setpoint);
    RUN_TEST(test_plan_converges_across_a_spread_of_profiles);
    RUN_TEST(test_last_stop_setting_moves_the_schedule_not_the_ceiling);
    return UNITY_END();
}
