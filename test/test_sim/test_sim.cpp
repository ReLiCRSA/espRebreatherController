// Sensor-layer simulator (AGENTS.md 11). The point of these tests is not "the sim runs" but
// "the sim is a model, not a script": every expected number below is hand-computed from the
// stated physics (ideal-gas inventory, coil i(t), volt-seconds, mV = gain x ppO2), so a model
// that quietly drifted towards plausibility fails here the way risk 13 demands.
//
// The closed-loop case is the one that matters most: ppO2 at 30 m is not given to anything, it
// falls out of metabolism against the pulses the control law commanded through a valve that can
// refuse to open. If the injection maths is wrong, the setpoint is not held, and the assert fires.
#include <unity.h>

#include <math.h>
#include <string.h>

#include "CcrControlLaw.h"
#include "GasModel.h"
#include "LoopModel.h"
#include "SensorIf.h"
#include "SimCells.h"
#include "SimRunner.h"
#include "ValveModel.h"
#include "Voting.h"

void setUp() {}
void tearDown() {}

static const float SURF = 1.013f;

// --- helpers -------------------------------------------------------------------------------

static GasMix mix(const char* n, float fo2, float fhe) {
    GasMix g{};
    strncpy(g.name, n, sizeof(g.name) - 1);
    g.fo2 = fo2;
    g.fhe = fhe;
    return g;
}

// Flat setpoint at every depth: the sag/hold tests must not be perturbed by a crossover the
// test isn't about.
static sim::SimRunner makeRunner(float setpoint, bool inject) {
    sim::SimRunner r;
    sim::CalSet cal{};  // defaults: 47.6 mV/bar, 0 offset — the shipped air-cal shape
    zhl::Config ec{};
    ec.surfaceBar = SURF;
    ec.mPerBar = 10.0f;  // deliberately round: expected pressures are checkable by hand
    GasModelConfig gc{};
    gc.spDeepBar = setpoint;
    gc.spShallowBar = setpoint;
    gc.spCrossoverDepthM = 0.0f;
    sim::ControlConfig cc{};
    cc.enabled = inject;
    r.setConfig(ec, gc, cal, cc);
    r.setBattery(3.7f);
    return r;
}

static void seg(sim::Segment& s, float depthM, float min, float sp,
                BreathingState mode = BreathingState::CCR_CLOSED, sim::GasSel gas = sim::GAS_DILUENT,
                int gasIndex = 0) {
    s.depthM = depthM;
    s.durationMin = min;
    s.setpointBar = sp;
    s.mode = mode;
    s.gasSel = gas;
    s.gasIndex = gasIndex;
}

// --- hand-computed physics: the sim is a model, not a script ---------------------------------

// Injection disabled, loop at 0.70 bar inspired. Hand-computed drain, and the anchor that pins
// the **metabolic-units rule**: metabolism is a molar draw quoted in *surface* litres, so the ppO2
// sag is depth-independent — rate/loopVolume = 1.2 x 1.013/8 = 0.15195 bar/min at *any* ambient.
// (Multiplying a surface rate by ambient was a real defect: it quadrupled consumption at 30 m
// while the closed loop still held setpoint, because the law simply injected 4x too much. The
// gas totals it printed were 4x wrong and *looked* plausible — risk 13 in unit form.)
//   two minutes: 0.70 - 2 x 0.15195 = 0.3961 at both 10 m and 30 m, identical to 4 decimals;
//   the zero-mole floor arrives at 0.70 x 8 / (1.2 x 1.013/60) = 276 s from either depth —
//   the model must hit the floor there, not stop at the setpoint.
void test_no_injection_sags_the_loop_by_hand_computed_amount() {
    sim::CcrControlLaw ctrl;
    sim::ControlConfig cc{};
    cc.enabled = false;  // the "solenoid disabled" bench path: compute, inject nothing
    sim::ValveModel valve;

    const float sag2min = 0.70f - 2.0f * 1.2f * 1.013f / 8.0f;
    float at10m = 0.0f;
    const float amb[2] = {2.013f, 4.013f};
    for (int d = 0; d < 2; ++d) {
        sim::LoopModel loop;
        loop.setConfig(sim::LoopConfig{});  // the documented unmeasured defaults
        loop.reset();
        loop.setPpO2(0.70f);
        for (int i = 0; i < 120; ++i)
            loop.step(1.0f, amb[d], 3.7f, loop.ppO2(), 0.7f, ctrl, cc, valve);
        TEST_ASSERT_FLOAT_WITHIN(0.005f, sag2min, loop.ppO2());
        TEST_ASSERT_EQUAL_UINT32(0, loop.pulsesTotal());  // disabled law must fire nothing
        if (d == 0) at10m = loop.ppO2();
        else TEST_ASSERT_FLOAT_WITHIN(0.0005f, at10m, loop.ppO2());  // depth-independent: the regression
    }

    {   // floor time: 276 s from 0.70 bar; still positive at 270, at zero by 280.
        sim::LoopModel loop;
        loop.setConfig(sim::LoopConfig{});
        loop.reset();
        loop.setPpO2(0.70f);
        for (int i = 0; i < 270; ++i)
            loop.step(1.0f, 4.013f, 3.7f, loop.ppO2(), 0.7f, ctrl, cc, valve);
        TEST_ASSERT_TRUE_MESSAGE(loop.ppO2() > 0.0f, "floor arrived early: drain too fast");
        for (int i = 0; i < 10; ++i)
            loop.step(1.0f, 4.013f, 3.7f, loop.ppO2(), 0.7f, ctrl, cc, valve);
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, loop.ppO2());  // the floor is a floor, not a setpoint
    }
}

// Closed loop, injection ON, 30 m, setpoint 0.70. ppO2 is nowhere supplied after the reset:
// metabolism pulls it down, the law commands pulses, the valve opens (3.7 V x 60 ms = 0.222
// volt-seconds > the 0.185 seat criterion) and gas arrives. The limit cycle is hand-checkable
// **in pulse quanta**, which is what makes this a test of the whole chain and not of the gains:
//   one pulse = 0.15 amb L x 4.013/8 = 0.0752 bar of ppO2;
//   metabolic sag = 0.152 bar/min, so the gap one pulse covers closes in ~0.50 min -> ~2 pulses/min;
//   the deadband (0.03) is *smaller* than one quantum, so injection fires just below setpoint-0.03
//   and one pulse lands ~0.075 higher: the cycle lives between ~0.665 and ~0.745.
// 300..720 s is 7 min of hold: hand demand ~14 pulses, ~2.15 amb L. Measured: 14 pulses, band
// 0.665..0.742. The mole balance closing at the hand number is the point — this is the assertion
// that would have failed on the metabolic-units defect (which needed ~4x the gas).
void test_closed_loop_holds_setpoint_at_30m() {
    sim::SimRunner r = makeRunner(0.70f, true);
    sim::Profile p{};
    seg(p.seg[0], 30.0f, 3.0f, 0.70f);   // reach depth: ramp done at 225 s at 8 m/min
    seg(p.seg[1], 30.0f, 10.0f, 0.70f);  // hold
    p.count = 2;
    p.defaultRateMPerMin = 8.0f;
    r.setProfile(p);
    r.runProfile();

    const float lo = r.windowMinPpO2(300, 720);
    const float hi = r.windowMaxPpO2(300, 720);
    TEST_ASSERT_TRUE_MESSAGE(lo >= 0.62f, "loop drooped below the hold band");
    TEST_ASSERT_TRUE_MESSAGE(hi <= 0.78f, "loop overshot the hold band");
    const int pulses = r.windowPulses(300, 720);
    TEST_ASSERT_TRUE_MESSAGE(pulses >= 12 && pulses <= 18,
                             "pulse rate off the hand-computed 2/min demand: gas accounting wrong");
    TEST_ASSERT_EQUAL(VoteStatus::OK, r.last().vote);
    TEST_ASSERT_EQUAL_INT(3, r.last().passing);
}

// The valve's own limits decide whether a commanded pulse is gas. At 2.0 V the 60 ms width is
// 0.120 volt-seconds, under the 0.185 seat criterion: the shunt still clicks, no gas arrives.
// This is the SOLENOID_FAIL-shaped signature and it must come from the physics, not a flag.
void test_low_battery_clicks_but_delivers_nothing() {
    sim::ValveModel v;
    sim::ClickSignature sig{};
    const float gas37 = v.pulse(60.0f, 3.7f, 4.0f, &sig);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, v.params.pulseVolAmbL, gas37);
    TEST_ASSERT_TRUE(sig.present);

    sim::ClickSignature sigLo{};
    const float gas20 = v.pulse(60.0f, 2.0f, 4.0f, &sigLo);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, gas20);
    TEST_ASSERT_TRUE_MESSAGE(sigLo.present, "an unopened valve on a live rail still draws current");
    TEST_ASSERT_TRUE(sigLo.peakA > 0.0f);
}

// Voltage compensation: width_used = width_table * V_nom / V_bat, clamped. Hand-computed.
void test_pulse_width_compensates_battery_voltage() {
    sim::ValveModel v;
    // 60 ms table at 4.1 V -> 60 * 3.7/4.1 = 54.15 ms
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 54.15f, v.compensatedWidthMs(60.0f, 4.1f, 20.0f, 250.0f));
    // at 3.4 V -> 65.29 ms
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 65.29f, v.compensatedWidthMs(60.0f, 3.4f, 20.0f, 250.0f));
    // clamps, both ends
    TEST_ASSERT_EQUAL_FLOAT(v.compensatedWidthMs(60.0f, 3.4f, 70.0f, 250.0f), 70.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 250.0f, v.compensatedWidthMs(240.0f, 3.4f, 20.0f, 250.0f));
}

// Three-way click taxonomy: the signature is what separates NO_CLICK from SLOW from the
// electrically-invisible click-but-no-gas. Assert the signatures differ in the right dimension.
void test_solenoid_fault_signatures_are_distinguishable() {
    sim::ValveModel v;
    sim::ClickSignature healthy{};
    const float gasH = v.pulse(80.0f, 3.7f, 4.0f, &healthy);
    TEST_ASSERT_TRUE(healthy.present);
    TEST_ASSERT_TRUE(gasH > 0.0f);

    sim::SolenoidFault f{};
    f.noClick = true;
    v.setFault(f);
    sim::ClickSignature none{};
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, v.pulse(80.0f, 3.7f, 4.0f, &none));
    TEST_ASSERT_FALSE(none.present);  // electrically silent: this one the shunt DOES catch

    f = sim::SolenoidFault{};
    f.damped = true;
    v.setFault(f);
    sim::ClickSignature damped{};
    const float gasD = v.pulse(80.0f, 3.7f, 4.0f, &damped);
    TEST_ASSERT_TRUE(damped.present);
    TEST_ASSERT_TRUE_MESSAGE(damped.peakA < healthy.peakA, "damped must show lower peak");
    TEST_ASSERT_TRUE_MESSAGE(damped.riseMs > healthy.riseMs, "damped must show slower rise");
    TEST_ASSERT_TRUE(gasD > 0.0f && gasD < gasH);  // partial delivery

    f = sim::SolenoidFault{};
    f.clickButNoGas = true;
    v.setFault(f);
    sim::ClickSignature blind{};
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, v.pulse(80.0f, 3.7f, 4.0f, &blind));
    TEST_ASSERT_TRUE_MESSAGE(blind.present, "this fault is electrically invisible by construction");
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, healthy.peakA, blind.peakA);  // identical signature
}

// In the closed loop, a no-click fault must show as commanded-but-not-clicked AND sag ppO2:
// the fault has to be visible both electrically and in the gas, as the design requires.
void test_no_click_fault_sags_ppo2_and_flags_electrically() {
    sim::SimRunner r = makeRunner(0.70f, true);
    sim::Profile p{};
    seg(p.seg[0], 30.0f, 1.0f, 0.70f);
    seg(p.seg[1], 30.0f, 6.0f, 0.70f);
    p.count = 2;
    p.defaultRateMPerMin = 20.0f;
    r.setProfile(p);
    sim::SolenoidFault f{};
    f.noClick = true;
    r.valve.setFault(f);
    r.runSeconds(420);

    TEST_ASSERT_TRUE_MESSAGE(r.last().noClick, "commanded pulses with zero clicks must be flagged");
    TEST_ASSERT_TRUE(r.windowPulses(0, 420) > 0);   // the law kept trying
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, r.last().gasDeliveredAmbL);  // nothing arrived
    // From ~0.21 inspired the loop drains at the depth-independent 0.152 bar/min (1.2 surf L/min
    // into 8 L): the whole inventory is gone 0.70*8/(1.2*1.013/60) = 276 s after starting at 0.70,
    // and far sooner from a loop never charged. By t=420 it must be on the floor, and the
    // commanded-pulse counter must have climbed while delivered gas stayed exactly zero.
    TEST_ASSERT_TRUE_MESSAGE(r.last().ppO2Used < 0.05f, "no gas delivered but ppO2 did not sag");
    TEST_ASSERT_TRUE(r.last().pulsesTotal > 0);     // injectedAmbL stays 0: accounting follows delivery
}

// --- cell faults reach the voter, as millivolts, through the real chain -----------------------

// The AGENTS.md 19 risk 3 case: a current-limited cell is byte-identical to a healthy one in air
// and under-reads above the knee. Air calibration cannot see it; the vote at depth can.
void test_current_limited_cell_passes_in_air_and_fails_at_depth() {
    sim::SimCells cells;
    cells.setCurrentLimited(0, true, 0.90f, 1.75f);

    CellCal cal[3];
    CellReading rd[3];
    float mv[3];
    bool pres[3];

    // In air: identical mV to the healthy cells (below the knee), votes OK.
    cells.advance(1.0f, 0.21f);
    cells.readMv(mv, pres);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, mv[1], mv[0]);
    for (int i = 0; i < 3; ++i) {
        rd[i].mv = mv[i];
        rd[i].present = true;
    }
    VoteOutcome vAir = evaluateCells(rd, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL(VoteStatus::OK, vAir.status);
    TEST_ASSERT_EQUAL_INT(3, vAir.passingCount);

    // At a 1.35 bar setpoint: the aged cell saturates flat at the model's ceiling,
    // clStart + 0.5*(clFull-clStart) = 1.05 bar once ppO2 passes clFull (1.20), i.e. 49.98 mV
    // against 64.3 mV from the healthy pair — 0.30 bar off a 1.35 median, outside the 0.27
    // (=20%) band -> DRIFT_ONE, and the two healthy cells still carry the vote.
    cells.setCurrentLimited(0, true, 0.90f, 1.20f);
    cells.advance(1.0f, 1.35f);
    cells.advance(1.0f, 1.35f);  // the cell's own state fully settled at the new ppO2
    cells.readMv(mv, pres);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 47.6f * 1.05f, mv[0]);
    for (int i = 0; i < 3; ++i) rd[i].mv = mv[i];
    VoteOutcome vDeep = evaluateCells(rd, cal, VoteConfig{}, false);
    // Status vocabulary, pinned here because it is easy to guess wrong: DRIFT_ONE means *one*
    // cell passed. Two-of-three surviving is status OK with passingCount 2 and the aged cell's
    // passesVote false — the per-cell flags are the drift signal, not the enum.
    TEST_ASSERT_EQUAL(VoteStatus::OK, vDeep.status);
    TEST_ASSERT_EQUAL_INT(2, vDeep.passingCount);
    TEST_ASSERT_FALSE(vDeep.cell[0].passesVote);
    TEST_ASSERT_TRUE(vDeep.cell[1].passesVote && vDeep.cell[2].passesVote);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.35f, vDeep.ppO2Used);  // mean of the two passing, not the aged one
}

// Stuck, dead-zero, and channel-absent must be three distinguishable outcomes, not one.
void test_stuck_dead_and_absent_cells_are_distinguishable() {
    sim::SimCells cells;
    CellCal cal[3];
    CellReading rd[3];
    float mv[3];
    bool pres[3];

    // Stuck high while the loop sits at 0.7: reads as if at 1.05 bar, 0.35 off a 0.70 median —
    // outside the 0.14 (=20%) band. (0.95 would *not* do this: 0.25 sits inside the 20% band at
    // that median, which is a fact about the band worth knowing, not a bug.) One cell flagged,
    // the vote still stands on the other two.
    cells.setStuckMv(1, 1.05f * 47.6f);  // reads as if at 1.05 bar
    cells.advance(1.0f, 0.70f);
    cells.readMv(mv, pres);
    for (int i = 0; i < 3; ++i) {
        rd[i].mv = mv[i];
        rd[i].present = pres[i];
    }
    VoteOutcome v = evaluateCells(rd, cal, VoteConfig{}, false);
    // (Status vocabulary: two cells surviving is OK + passingCount 2; DRIFT_ONE is reserved for
    // the one-cell-passing case.)
    TEST_ASSERT_EQUAL(VoteStatus::OK, v.status);
    TEST_ASSERT_EQUAL_INT(2, v.passingCount);
    TEST_ASSERT_FALSE(v.cell[1].passesVote);

    // A dead cell reading 0 mV is *inside* the -50..1000 mV bias band, so it participates in the
    // vote as a 0 bar reading rather than being filtered out. The vote must catch it anyway.
    cells.resetFaults();
    cells.setDead(2, true);
    cells.advance(1.0f, 0.70f);
    cells.readMv(mv, pres);
    for (int i = 0; i < 3; ++i) {
        rd[i].mv = mv[i];
        rd[i].present = pres[i];
    }
    v = evaluateCells(rd, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL(VoteStatus::OK, v.status);
    TEST_ASSERT_EQUAL_INT(2, v.passingCount);
    TEST_ASSERT_FALSE(v.cell[2].passesVote);
    TEST_ASSERT_EQUAL(FaultCode::NONE, v.cell[2].fault);  // 0 mV is not itself out-of-band
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.70f, v.ppO2Used);   // the zero must not drag the used value

    // An absent channel is a different failure: the voter marks the cell CELL_DEAD and the
    // remaining pair votes OK with the pair median.
    cells.resetFaults();
    cells.setChannelPresent(0, false);
    cells.advance(1.0f, 0.70f);
    cells.readMv(mv, pres);
    TEST_ASSERT_FALSE(pres[0]);
    for (int i = 0; i < 3; ++i) {
        rd[i].mv = mv[i];
        rd[i].present = pres[i];
    }
    v = evaluateCells(rd, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL(VoteStatus::OK, v.status);
    TEST_ASSERT_EQUAL_INT(2, v.passingCount);
    TEST_ASSERT_EQUAL(FaultCode::CELL_DEAD, v.cell[0].fault);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.70f, v.ppO2Used);
}

// Noise must be reproducible: a verify run that is a coin toss is not a regression suite.
void test_injected_noise_is_deterministic() {
    float first[3] = {0}, second[3] = {0};
    for (int pass = 0; pass < 2; ++pass) {
        sim::SimCells cells;
        cells.setNoiseMv(0, 0.8f);
        for (int i = 0; i < 20; ++i) cells.advance(1.0f, 0.70f);
        float mv[3];
        bool pres[3];
        cells.readMv(mv, pres);
        (pass == 0 ? first : second)[0] = mv[0];
        // The noisy cell must actually differ from the clean one, or "deterministic" is vacuous.
        if (pass == 0) TEST_ASSERT_TRUE(fabsf(mv[0] - mv[1]) > 0.001f);
    }
    TEST_ASSERT_EQUAL_FLOAT(first[0], second[0]);
}

// --- diluent flush: the independent check, through the whole runner ---------------------------

// After a diluent flush at depth, the three cells must agree with fo2*(Pamb-pH2O) — the same
// expression expectedPpO2ForFlush() uses, reached from the other side (loop fraction -> mV ->
// calibration -> vote). If any link double-counts or drops the water-vapour term, this fails.
void test_diluent_flush_cells_agree_with_expected_ppo2() {
    sim::SimRunner r = makeRunner(0.70f, true);
    sim::Profile p{};
    seg(p.seg[0], 30.0f, 3.0f, 0.70f);
    seg(p.seg[1], 30.0f, 2.0f, 0.70f, BreathingState::CCR_DILUENT_FLUSH);
    p.count = 2;
    p.defaultRateMPerMin = 20.0f;  // get to depth inside the first segment
    r.setProfile(p);
    sim::GasSet g{};
    g.diluent = mix("AIR", 0.21f, 0.0f);
    r.setGases(g);
    // A real flush never quite replaces every litre of loop gas; 98% keeps the model honest and
    // the assert band physically meaningful.
    r.loop.config.flushEffectiveness = 0.98f;

    // Hold at depth: descent done at 90 s, loop settled at setpoint; ticks 0..179, then the
    // tick that enters the flush segment is t=180.
    r.runSeconds(180);
    r.tick();
    const sim::Snapshot& s = r.last();
    const float expected = expectedPpO2ForFlush(0.21f, s.ambBar, 0.0627f);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, expected, s.ppO2Used);
    TEST_ASSERT_EQUAL(VoteStatus::OK, s.vote);

    // And the check must have teeth: with one cell stuck high, voting still says OK while the
    // flush check fails — the case voting cannot fake (AGENTS.md 5).
    r.cells.setStuckMv(2, 1.30f * 47.6f);
    r.tick();
    VoteOutcome vs{};
    CellReading rd[3];
    for (int i = 0; i < 3; ++i) {
        rd[i].mv = r.last().cellMv[i];
        rd[i].present = r.last().cellPresent[i];
    }
    CellCal cal[3];
    vs = evaluateCells(rd, cal, VoteConfig{}, false);
    float worst = 0.0f;
    TEST_ASSERT_FALSE_MESSAGE(diluentFlushAgrees(vs, expected, 0.10f, &worst),
                              "flush check must fail against a stuck cell");
    TEST_ASSERT_TRUE(worst > 0.10f);
}

// --- source stamping (AGENTS.md 19 risk 11) ----------------------------------------------------

struct RealBaroStub : public Barometer {
    bool init() override { return true; }
    bool read(float* ambBar, float* tempC) override {
        *ambBar = 1.013f;
        if (tempC) *tempC = 20.0f;
        return true;
    }
};

void test_source_stamps_are_honest() {
    sim::SimRunner r = makeRunner(0.70f, true);
    sim::Profile p{};
    seg(p.seg[0], 0.0f, 1.0f, 0.70f);
    p.count = 1;
    r.setProfile(p);
    r.runSeconds(5);
    // Sim baro + sim cells = SIM. A dive computer saying REAL here would be lying.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceMix::SIM), static_cast<uint8_t>(r.last().sources));

    // The MIXED bench case: real cells + simulated depth is exactly the no-chamber setup.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceMix::MIXED),
                            static_cast<uint8_t>(combineSources(SourceKind::REAL, SourceKind::SIM)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceMix::MIXED),
                            static_cast<uint8_t>(combineSources(SourceKind::SIM, SourceKind::REAL)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceMix::REAL),
                            static_cast<uint8_t>(combineSources(SourceKind::REAL, SourceKind::REAL)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceMix::SIM),
                            static_cast<uint8_t>(combineSources(SourceKind::SIM, SourceKind::SIM)));
    RealBaroStub real;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceKind::REAL), real.source());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SourceKind::SIM), r.baro.source());
}

// Time compression (1x/10x/60x) is wall-clock only: the engine integrates fixed 1 s steps, so a
// profile run twice must be bit-identical. This test is the structural guarantee risk 9 leans on.
void test_runs_are_numerically_identical() {
    sim::Profile p{};
    seg(p.seg[0], 40.0f, 15.0f, 0.70f);
    seg(p.seg[1], 21.0f, 5.0f, 0.70f);
    seg(p.seg[2], 6.0f, 8.0f, 1.30f);
    p.count = 3;

    sim::SimRunner a = makeRunner(0.70f, true);
    a.setProfile(p);
    a.runProfile();
    sim::SimRunner b = makeRunner(0.70f, true);
    b.setProfile(p);
    b.runProfile();

    TEST_ASSERT_EQUAL_FLOAT(a.last().ppO2Used, b.last().ppO2Used);
    TEST_ASSERT_EQUAL_FLOAT(a.last().ndlMin, b.last().ndlMin);
    TEST_ASSERT_EQUAL_FLOAT(a.last().ttsMin, b.last().ttsMin);
    TEST_ASSERT_EQUAL_FLOAT(a.last().otu, b.last().otu);
    TEST_ASSERT_EQUAL_UINT32(a.last().pulsesTotal, b.last().pulsesTotal);
}

// verify() is the regression harness the web Simulator panel will call: it must report per-item
// PASS/FAIL with a readable message, not just a boolean.
void test_verify_reports_passes_and_failures() {
    sim::SimRunner r = makeRunner(0.70f, true);
    sim::Profile p{};
    seg(p.seg[0], 30.0f, 3.0f, 0.70f);
    seg(p.seg[1], 30.0f, 9.0f, 0.70f);
    p.count = 2;
    p.defaultRateMPerMin = 20.0f;  // depth reached at 90 s, loop settles well before 360 s
    r.setProfile(p);
    r.runProfile();

    sim::Assertion a[2]{};
    a[0].metric = sim::Metric::PP02_MAX;  // "held near setpoint, never off it" — the window max
    a[0].fromSec = 360;
    a[0].toSec = 540;
    a[0].low = 0.60f;
    a[0].high = 0.80f;
    a[0].note = "setpoint held";
    a[1].metric = sim::Metric::PULSES_TOTAL;
    a[1].low = -1.0f;
    a[1].high = -0.5f;  // deliberately impossible: a closed loop at 30 m fires pulses
    a[1].note = "deliberate failure";
    sim::VerifyReport rep = r.verify(a, 2);
    TEST_ASSERT_EQUAL_INT(2, rep.total);
    TEST_ASSERT_EQUAL_INT(1, rep.passed);
    TEST_ASSERT_FALSE(rep.allPassed());
    TEST_ASSERT_TRUE(rep.item[0].passed);
    TEST_ASSERT_TRUE(rep.item[0].evaluated);
    TEST_ASSERT_TRUE(rep.item[0].samples > 0);   // every answer must be quotable against a sample count
    TEST_ASSERT_FALSE(rep.item[1].passed);
    TEST_ASSERT_TRUE(strlen(rep.item[1].message) > 0);
    TEST_ASSERT_NOT_EQUAL(nullptr, strstr(rep.item[1].message, "expected"));
}

// The history is a ring, and an assertion over a window the ring no longer holds must say
// "NO DATA", never compare the empty window's 0.0 against a band. An assertion that silently
// measures nothing and reports a pass is the same false-plausible-number failure as risk 13 —
// on a 26-minute on-target run (96-entry ring) every early-window assertion would have done this.
void test_history_ring_overwrites_and_reports_no_data() {
    sim::SimRunner r = makeRunner(0.70f, true);
    sim::Profile p{};
    seg(p.seg[0], 30.0f, 11.0f, 0.70f);   // 660 s > the host ring's 512 entries
    p.count = 1;
    p.defaultRateMPerMin = 20.0f;
    r.setProfile(p);
    r.runProfile();

    TEST_ASSERT_TRUE_MESSAGE(r.histDropped() > 0, "ring did not overwrite: run shorter than capacity?");
    TEST_ASSERT_TRUE(r.histOldestSec() > 0);      // the start of the run is gone...
    TEST_ASSERT_EQUAL_UINT32(659, r.histNewestSec());  // ...the tail is intact

    sim::Assertion a[1]{};
    a[0].metric = sim::Metric::PP02_MAX;
    a[0].fromSec = 0;
    a[0].toSec = 10;  // entirely inside the overwritten head of the run
    a[0].low = 0.0f;
    a[0].high = 2.0f;  // a band wide enough that 0.0 from an empty window would "pass"
    a[0].note = "dropped window";
    sim::VerifyReport rep = r.verify(a, 1);
    TEST_ASSERT_EQUAL_INT(1, rep.unevaluated);
    TEST_ASSERT_FALSE(rep.item[0].evaluated);
    TEST_ASSERT_FALSE(rep.item[0].passed);   // not evaluated is not a pass
    TEST_ASSERT_EQUAL_INT(0, rep.passed);
    TEST_ASSERT_FALSE(rep.allPassed());
    TEST_ASSERT_NOT_EQUAL(nullptr, strstr(rep.item[0].message, "NO DATA"));
    TEST_ASSERT_TRUE(rep.histDropped >= (unsigned long)r.histDropped());
    TEST_ASSERT_TRUE(rep.histOldestSec == r.histOldestSec());

    // A stride stretches coverage: 96 entries x 16 s covers the whole 26-minute bench profile,
    // which is what the on-target runner uses. Note keeps land on multiples of the stride minus
    // one (first at t=15), so a 0..10 s window is *still* empty at stride 16 — the assertion
    // below moves to a window the stride does retain, rather than pretending it is covered.
    r.setHistoryStride(16);
    r.resetRun();
    r.runProfile();
    TEST_ASSERT_EQUAL_UINT32(0, r.histDropped());
    TEST_ASSERT_TRUE(r.windowCount(0, 10) == 0);      // honest even under a stride
    a[0].fromSec = 300;
    a[0].toSec = 316;
    a[0].low = 0.60f;
    a[0].high = 0.80f;
    sim::VerifyReport rep2 = r.verify(a, 1);
    TEST_ASSERT_TRUE(rep2.item[0].evaluated);
    TEST_ASSERT_EQUAL_INT(0, rep2.unevaluated);
    TEST_ASSERT_TRUE(rep2.item[0].passed);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_injection_sags_the_loop_by_hand_computed_amount);
    RUN_TEST(test_closed_loop_holds_setpoint_at_30m);
    RUN_TEST(test_low_battery_clicks_but_delivers_nothing);
    RUN_TEST(test_pulse_width_compensates_battery_voltage);
    RUN_TEST(test_solenoid_fault_signatures_are_distinguishable);
    RUN_TEST(test_no_click_fault_sags_ppo2_and_flags_electrically);
    RUN_TEST(test_current_limited_cell_passes_in_air_and_fails_at_depth);
    RUN_TEST(test_stuck_dead_and_absent_cells_are_distinguishable);
    RUN_TEST(test_injected_noise_is_deterministic);
    RUN_TEST(test_diluent_flush_cells_agree_with_expected_ppo2);
    RUN_TEST(test_source_stamps_are_honest);
    RUN_TEST(test_runs_are_numerically_identical);
    RUN_TEST(test_verify_reports_passes_and_failures);
    RUN_TEST(test_history_ring_overwrites_and_reports_no_data);
    return UNITY_END();
}
